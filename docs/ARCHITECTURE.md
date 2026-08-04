# Architecture

Three lanes, each with different rules about what it may do. Almost every design
decision here follows from one constraint: the audio lane must never block.

## Threads and data flow

```
  WORKERS (ingest, analysis, export)          CONTROL (main thread)
  ─────────────────────────────────           ─────────────────────
  FFmpeg decode        libsamplerate          FTXUI render loop
  onset detection      peak pyramid           RtMidi callbacks
  yt-dlp subprocess    file I/O               Lua VM, project state
          │                                            │
          │ publishes immutable                        │ PadEvent, ParamChange
          │ Sample objects                             │ (SpscRing, lock-free)
          ▼                                            ▼
  ┌────────────────┐                          ┌─────────────────────┐
  │  sample pool   │◄─────shared_ptr──────────│   AUDIO (RtAudio)   │
  │  (shared_ptr)  │                          │   render callback   │
  └────────────────┘                          └─────────────────────┘
          ▲                                            │
          │ frees here, never on audio                 │ retired shared_ptrs
          │                                            ▼
  ┌────────────────┐                          ┌─────────────────────┐
  │    JANITOR     │◄────────collect()────────│    GarbageRing      │
  └────────────────┘                          └─────────────────────┘
```

**Workers** do everything slow or fallible: decoding, resampling, analysis, file
and network I/O. They build `Sample` objects fully, then publish them as
immutable, shared-ownership values. Nothing half-constructed is ever visible to
the audio lane.

**Control** owns the UI, MIDI input, the Lua VM, and project state. It never
touches audio state directly; it sends messages through SPSC rings.

**Audio** drains the rings at block start, runs the voice pool through the mixer
graph, and returns. It allocates nothing, locks nothing, and never calls Lua.

## The real-time rules, and what enforces them

The rules are in CLAUDE.md. What matters here is that each has a mechanism, not
just a convention:

| Rule | Enforcement |
|---|---|
| No allocation in the callback | `RT_SCOPE()` — thread-local depth counter plus replaced global `operator new`; violation aborts. `tests/integration/rt_safety_test.cpp` runs the engine under it. |
| No exceptions | `-fno-exceptions -fno-rtti` on `cratedig_engine`; `tests/compile/all_rt_headers.cpp` compiles every `src/rt/` header that way. Errors travel as `rt::Result<T, E>`. |
| No locks, no blocking | Lock-free SPSC rings only. TSan is the authority. |
| Never drop the last `shared_ptr` | `rt::GarbageRing` — the audio thread retires, the janitor destroys. |
| No device headers in the engine | `Engine::render()` takes plain buffers. RtAudio/RtMidi are confined to `src/io/`. |

### RT_SCOPE granularity, and its limit

The guard replaces the global `operator new`/`operator delete` family and checks
a thread-local depth counter. This is deliberate:

- It is standard C++, so macOS and Linux behave identically — no `malloc_zone`
  pointer swapping (those zones are write-protected on current macOS), no
  `DYLD_INSERT_LIBRARIES` (fights SIP and does not survive ctest child
  processes), no Linux-only symbol interposition.
- It forwards to `malloc`/`free`, which ASan and TSan interpose, so the
  sanitizers keep working underneath it.
- It catches everything our own code can do to the heap: `new`, `std::vector`
  growth, `std::string`, `std::function` assignment.

**Not active under TSan.** ThreadSanitizer's runtime defines the whole
`operator new`/`delete` family as *strong* symbols, so our replacements collide
at link time on Linux (ASan's equivalents are weak and coexist; macOS runtimes
interpose rather than define). Under TSan the replacements are therefore compiled
out on every platform — predictably absent everywhere beats present on one and
absent on the other — while scope depth tracking still works. Tests that assert
allocation detection check `rt::kAllocationDetectionEnabled` and `SKIP()` loudly
instead of passing vacuously. Nothing is left unchecked: `dev`, `asan`, and
`ubsan` enforce the allocation rule on the same code, and TSan's job is races.

**What it does not catch:** a raw C `malloc` from inside a third-party library.
Nothing in the callback is permitted to call such a library, so this is currently
a theoretical gap. Closing it means real malloc interposition — a
`__DATA,__interpose` section on macOS, symbol override on Linux — which is
tracked as hardening once plugin hosting (M8) starts running foreign code on the
audio thread.

The guard lives in an **OBJECT library**, not a static library. Replacement
operators resolve no undefined symbol, so a static library's objects would be
dropped at link time and the guard would silently enforce nothing. A unit test
asserts the operators really are linked in, for exactly that reason.

## Ring topology

Three ring types, because they have genuinely different invariants.

**`rt::SpscRing<T, Capacity>`** — control → audio messages. Requires trivially
copyable `T`: it overwrites slots by assignment and never runs a destructor.
Indices are monotonic and masked only when indexing the buffer, so all `Capacity`
slots are usable and full/empty is unambiguous without a sacrificial slot.
Unsigned wraparound at `SIZE_MAX` is correct rather than merely tolerated: 2^64
is an exact multiple of any power-of-two capacity, so both the difference and the
masked offset stay consistent across the wrap.

Each side caches the other's index and only re-reads the shared atomic when the
ring appears full (producer) or empty (consumer). That keeps the shared cache
line off the common path.

Ordering, and why:

- Producer loads its own write index **relaxed** — it is the only writer.
- Producer loads the read index **acquire** before reusing a slot — it must see
  the consumer's reads as complete.
- Producer stores the write index **release** — this publishes the slot contents.
- Consumer mirrors all three.

**`rt::GarbageRing<Capacity>`** — audio → janitor, holding `shared_ptr<void>`.
Not a `SpscRing` of `shared_ptr`, because `shared_ptr` is neither trivially
copyable nor safe to overwrite by assignment. The invariant is different in kind:
`collect()` empties each slot *before* publishing the new read index, so
`retire()` always moves into an already-empty slot and never releases a reference
on the audio thread.

`retire()` returning `false` means the ring is full. **The caller still owns the
pointer** and must hold it until a later block. Dropping it there would destroy
the object on the audio thread — the exact failure the type exists to prevent.
A non-zero `overflow_count()` means the janitor is behind or the ring is too
small.

**`rt::HandoffRing<T, Capacity>`** — control → audio, holding
`shared_ptr<const T>`. The `GarbageRing` discipline pointed the other way: pop
moves *out* of the slot and leaves it empty, so push always moves into an empty
slot and no destructor runs on either side. A sibling of `SpscRing` rather than a
change to it, for the same reason `GarbageRing` is one — a `shared_ptr` is
neither trivially copyable nor safe to overwrite by assignment.

`push()` returning `false` means the ring is full, and **the caller still owns
the object**: the edit did not happen, and `Engine::rejected_pad_configs()`
counts it. Reporting the refusal is better than dropping it silently, because the
control-side view of the pads must not diverge from what the audio thread has.

**Five rings, as of M4**, and the count is a design statement rather than an
accident:

| Ring | Direction | Why it is separate |
|---|---|---|
| `m_events` | control → audio | keyboard `PadEvent`s |
| `m_midi_events` | **MIDI thread** → audio | `SpscRing` is single-producer; sharing the keyboard's would be a race that only shows up under load |
| `m_transport_commands` | control → audio | play/stop/seek as one command, so the audio thread cannot see a position without the state that goes with it |
| `m_pad_handoff` | control → audio | owning `PadConfig` handles |
| `m_sequencer_handoff` | control → audio | one owning `SequencerState` handle |

**The handoff rings only drain when something renders**, which is the trap
`--no-audio` sets: nothing calls `render()`, so nothing adopts what is published,
the rings fill, and every later edit is refused for the rest of the session.

It was hit twice. First on the sequencer, where every step toggle publishes, so
eight keystrokes exhausted eight slots. Then on the pads, where `:chop` publishes
sixteen configs at once and `:slot assign 1-8 1` publishes eight — four commands,
and pad assignment stopped working entirely. The second was reported as "slot
assign is not working", which it was.

The answer is `Engine::adopt_offline()`, called from the frame tick when no
device is open: it does what the top of `render()` does, and it is safe
*because* there is no audio thread in that mode. One place has to know, rather
than every publisher guarding itself — which is what the first fix did, and why
the same bug was still waiting one ring over.

## Determinism contract

`Engine::render()` with a fixed seed and fixed input is bit-exact across runs on
the same platform. Concretely:

- **Block-size invariance.** Rendering N frames as one call, as N/128 calls, or
  as a ragged sequence must produce identical bytes. This is what catches state
  leaking across block boundaries.
- **Seeded.** `Engine::Config::seed` anchors every stochastic element (humanize,
  noise, dither). It exists from M0, before anything consumes it, so the
  guarantee is not retrofitted later.
- **Fixed tables.** Windows, sinc kernels, and dB curves are `constexpr`, never
  built at runtime, so no floating-point accumulation order can drift.

Cross-*platform* bit-exactness is explicitly not promised: different FP
contraction and libm implementations make it unaffordable. Golden hashes are
per-platform.

One golden is nevertheless cross-platform, and it is worth understanding why it
is an exception rather than evidence the rule is too cautious. The M3 chop
acceptance (`tests/e2e/chop_e2e_test.cpp`) renders at a 1:1 rate ratio with the
default flat-sustain envelope, so the phase fraction is exactly zero — the
Hermite kernel collapses to an exact copy — and `Envelope::level()` never
evaluates its `m_start + m_step * position` ramp. With no `a*b+c` left in the
path there is nothing for an FMA to fuse differently, and the hash was then
*checked* on AppleClang/arm64 and clang-18/x86-64 rather than reasoned about and
assumed. Retune a pad or give it an attack and the exception evaporates;
`docs/TESTING.md` records when a constant may be committed and when only
invariance may be.

`rt::kCacheLine` is a fixed 64 rather than
`std::hardware_destructive_interference_size` for the same reason — that value
varies by compiler and standard library version, which would change struct layout
across toolchains.

## Module map

| Path | Owns | May depend on |
|---|---|---|
| `src/rt/` | SPSC rings, garbage ring, RT guard, `Result`, `Sample`, interpolator, voice pool, mixer graph, DSP primitives | nothing outside `src/rt/`; header-only where possible |
| `src/engine/` | Engine facade, transport, sequencer, offline bounce | `src/rt/` |
| `src/ingest/` | FFmpeg decode, resampling, peak pyramid, onset detection, the sample pool, yt-dlp subprocess | `src/rt/` types only |
| `src/tui/` | FTXUI components: waveform, pad grid, mixer, command line. The **only** module that may include `ftxui/` headers | `src/engine/` via messages, `src/ingest/` for loading |
| `src/lua/` | sol2 bindings, config loader, chop-algo and macro API | `src/engine/` |
| `src/host/` | CLAP hosting; LV2 via lilv | `src/rt/` process interface |
| `src/io/` | RtAudio + RtMidi device layer — the **only** files that may include those headers | `src/engine/` |

`scripts/check_layering.sh` enforces this table mechanically and runs inside
`scripts/ci.sh`. These are the rules that decay silently: nothing breaks the day
someone includes `RtAudio.h` in the engine "just for the device enum" — it
builds, it runs, and the cost only appears later when offline export or a
container needs an engine that works with no sound card. A grep in CI is cheap;
discovering at M6 that the engine cannot be built headless is not.

It checks that device headers appear only in `src/io/` and only from a `.cpp`
(a device type in a public header leaks one level down instead), that `src/rt/`
includes nothing outside itself, that the engine never sees `io/` or `tui/`, that
ingest never sees the engine, that `ftxui/` headers appear only in `src/tui/`, and
that nothing in `src/tui/` includes `termios.h` — FTXUI owns raw mode and signal
handling now, and two owners of one global means a user's terminal left unusable.

## Sample lifetime

A `Sample` is built by a worker, published as `shared_ptr<const Sample>`, and
read — never written — by the audio thread. The lifetime question is who
releases the last reference, because that runs a destructor and therefore a
`free()`.

- **Triggering** copies the pad table's `shared_ptr` into a voice. That is an
  atomic increment: no allocation, no lock, legal in the callback. Since M3 the
  voice holds the **`PadConfig`**, not the `Sample` — a voice must keep alive
  everything it reads, and that now includes the envelope and the slice bounds.
  The `Sample` stays alive transitively, through the config that names it.
- **Finishing** hands the voice's reference to the `GarbageRing`, and the janitor
  destroys it. A voice that cannot retire (ring full) keeps its reference and
  stays un-reusable until the next block, rather than dropping it.
- **Retriggering the same pad** reuses the reference already in the stolen voice.
  Rolling one pad is the most common thing anyone does with a sampler; retiring
  on every hit filled the ring and started dropping hits.

The pad table itself is **written only by the audio thread**, in
`adopt_pad_configs()` at the top of each block. The control thread never touches
it — it publishes through `HandoffRing` instead — which is what keeps triggering
free of atomics while still allowing a pad to be reassigned mid-stream. Before M3
the table was written by the control thread and `set_pad_sample()` was documented
pre-start only; the protocol above is what removed that caveat.

## The peak pyramid

The waveform is drawn from a multi-resolution min/max summary
(`src/ingest/peak_pyramid.hpp`), not from the samples. At full zoom-out one
column of a five-minute file spans ~147 000 frames; rescanning them 196 times per
frame at 30 Hz is not a thing a UI can do.

Min/max rather than averages, because a waveform drawn from averages loses every
transient: a one-sample kick spike averaged over 147 000 frames is
indistinguishable from silence.

- **Base bin 256 frames, ratio 4.** The small ratio is the point: the level whose
  bins fit inside a column is never more than 4x finer than the column, so
  `summarize()` reads about five bins per column at *any* zoom. That bound, not
  memory, is why the ratio is small.
- **Memory is ~0.8% of the audio.** Five minutes of 48 kHz stereo is 115 MB of
  samples and ~1.2 MB of pyramid.
- **The guarantee is one-sided.** A column's reported range is a *superset* of the
  truth, over-reporting by at most one bin. A transient can smear sideways by
  less than a character; it can never disappear. Under-reporting would be a lie
  about the audio; over-reporting is invisible at four dots per character.
- **Exact when zoomed in.** Below one base bin per column, `summarize()` reads raw
  frames. The pyramid is an acceleration structure, never a source of
  approximation error the caller cannot reason about.
- Built on the control thread at load — ~46 ms for five minutes of stereo.
  Moving it onto a worker so the interface can appear first is M6's ingest job.

Measured cost of one redraw of a five-minute file at full zoom-out: 0.004 ms with
the pyramid, 6.8 ms without. `tests/tui/waveform_perf_test.cpp` asserts it.

## Telemetry: what the audio thread tells the interface

The audio thread publishes a small block of relaxed atomics at the end of every
`render()` — playhead, per-pad level, master level — and the UI reads them
whenever it draws. Everything is on its own cache lines
(`alignas(kCacheLine)`), grouped rather than scattered through `Engine`, so a UI
poll does not keep invalidating the line the control thread's event ring lives
on.

- **Relaxed, everywhere.** The UI wants a *recent* value, not a synchronised one.
  An acquire/release pair here would put a barrier in the audio thread's hot path
  to make a meter one frame fresher.
- **The playhead is one packed word, not two atomics.** Pad in the top 8 bits,
  frame in the low 56 (about 15 000 years at 48 kHz). Two atomics would let the
  UI read a frame position from one voice with the pad label of another —
  cosmetic today, wrong once M4 shows transport position.
- **Pad glow is one packed word per pad**, for the same reason: frames since the
  trigger in the low 24 bits (349 s at 48 kHz), quantised velocity in the top 8.
  Two atomics would let the UI pair one hit's age with another's velocity, and a
  pad flashing at the wrong brightness is a visible wrong answer rather than a
  rounding. The count saturates one short of the mask so the "never triggered"
  sentinel stays unreachable however long the program runs — that state is
  distinct from "hit a long time ago", and the two look identical if zero has to
  mean both.
- **Meters fall linearly to zero over 0.4 s**, derived from `num_frames` so the
  behaviour does not change with the block size the device negotiated. Linear
  rather than exponential keeps a transcendental out of the callback. Both
  failure modes it avoids are real: no fall pins the meter at the loudest thing
  that ever happened, and no hold shows whichever 5 ms block a 30 Hz redraw
  sampled — which, for a drum pattern, is usually silence.
- **Telemetry is NOT part of the determinism contract.** The fall rate depends on
  block size; `render()`'s output does not. The silence golden and the block-size
  invariance tests are what guard that publishing it never perturbs a sample.

## The interface is a pure function

`tui::render(const UiState&, columns, rows) -> ftxui::Element`. `UiState` holds no
`Engine`, no `AudioDevice`, no `Sample` and no clock, and the terminal size is a
parameter rather than read from `ftxui::Terminal::Size()`.

That is what makes the layout testable: a snapshot test builds a `UiState`
literal and renders it to an offscreen `Screen` at three sizes in one process.
Determinism comes from there being nothing non-deterministic in scope, rather
than from suppressing sources of variance one at a time.

The app assembles that struct once per frame from engine telemetry and a
freshly-summarised set of peak bins, and re-summarises against the *current*
width so a resize is a correct redraw rather than a stretched one.

**Redraws are posted with `App::Post`, not `App::PostEvent`.** FTXUI's own
documentation recommends `PostEvent(Event::Custom)` from a refresh thread, and in
7.0.1 that is a data race: it lands in `MultiReceiverBuffer::Push`, which does an
unsynchronised `push_back` on a deque shared with the main loop. `Post` carries
the same event through `TaskRunner::PostTask`, whose queue is mutex-guarded. TSan
found it; reading FTXUI's source is what settled that it was real rather than
TSan mis-reading an uninstrumented library.

## Playback position is fixed point

Voice phase is 32.32 fixed point (`rt::PhaseFixed`), not a float or a double.

A float accumulator loses fractional bits as the integer part grows, so
block-size invariance would hold by luck for short samples and quietly fail for
long ones — exactly the kind of bug that reproduces only on the one file someone
cares about. Integer addition is exact, so the invariance holds by construction.
32 fractional bits give a step resolution of ~2.3e-10 frames; over a ten-minute
48 kHz file the accumulated position error is zero.

## Dependency staging

The dependency set is fixed (CLAUDE.md). Dependencies are *activated* at the
milestone that first needs them; `cmake/deps.cmake` carries commented,
tag-pinned blocks for the rest. This schedules the fixed set, it does not change
it.

| Dependency | Activates | Notes |
|---|---|---|
| Catch2 | M0 | tests only, never shipped |
| RtAudio, libsamplerate, CLI11 | M1 | static; RtAudio only reachable from `src/io/`, and its API set is pinned (ALSA on Linux, CoreAudio on macOS) so host-detected JACK/Pulse cannot change the binary |
| FFmpeg | M1 | **system package, dynamically linked.** Not a FetchContent dependency — see docs/LICENSING.md. Version floor 5.1 (`AVChannelLayout` API) |
| RtMidi | M4 | static; `src/io/` only |
| FTXUI | M2 | v7.0.1, static; `src/tui/` only. Owns raw mode, signal handling and terminal restoration |
| PFFFT | M3 | onset detection |
| Lua 5.4 + sol2 | M7 | |
| CLAP, lilv | M8 | lilv is a system package |

## Build and CI shape

- Presets `dev` (RelWithDebInfo), `asan`, `tsan`, `ubsan` — all four must pass
  before any task is done; `tsan` is mandatory for `src/rt/` changes.
- `scripts/ci.sh <presets…>` is the single configure/build/test loop, used
  identically by Docker and by GitHub Actions, so local and CI cannot drift.
- `docker compose -f docker/compose.yml run --rm ci` runs the whole matrix on
  Linux/clang-18 and is the ROADMAP acceptance path.
- CMake's C++20 module scanning is off: we use no modules, and the `.modmap`
  response files it emits into the compile database are unresolvable by
  clang-tidy.
- Audio fixtures are fetched, never committed; `scripts/verify_fixtures.py` runs
  inside `ci.sh` so checksum drift fails the build (docs/TESTING.md).

## Live reconfiguration: one problem, one protocol

Three separate features need to change what the audio thread is using, while it
is using it:

- **Assigning a slice to a pad** (M3, built) — `:chop transient` or
  `:slot assign`, with the stream running.
- **Recording into a pad** (M6) — the whole point is that the new sample is
  playable the moment it exists.
- **Changing a pad's plugin chain** (M8) — instances cannot be constructed or
  destroyed on the audio thread.

They are one problem, and M3 built the protocol once for all three:

1. **Build off-thread.** The control thread constructs the whole new thing — a
   `PadConfig`, later a `Sample` or a plugin chain — with allocation, file I/O and
   instantiation all happening where they are allowed.
2. **Publish through `rt::HandoffRing`**, an SPSC ring of owning handles.
   `Engine::publish_pad_config()` move-assigns into a slot that is already empty,
   so no destructor runs on either side.
3. **Swap on the audio thread.** `adopt_pad_configs()` drains the ring at the top
   of every `render()`; the swap itself is a pointer exchange.
4. **Retire the displaced handle into the `GarbageRing`**, exactly as voices
   already do. Nothing is constructed or destroyed on the audio thread, ever.

**The displaced handle is an Engine member, not a local.** If `retire()` fails
because the garbage ring is full, a local `shared_ptr` going out of scope would
run the destructor this whole mechanism exists to avoid. Holding it in
`m_retiring` makes the retry free and turns the failure mode into "one block
late" rather than "freed in the callback".

**The rule that falls out of it:** anything the audio thread reads which the
control thread can change must be reachable through **one pointer**, so that
swapping it is a single operation. That is why `PadConfig` is immutable once
published — editing a pad means building a new one, not mutating the live one. A
config the control thread mutated field by field could not be published safely at
all: the audio thread would read a slice range from the new edit with an envelope
from the old one.

The control thread keeps its own copy of what it has published
(`Engine::pad_config()`), which is deliberately *one block ahead* of what the
audio thread is using. That is the honest answer for a UI — the interface should
show an edit the moment it is made, and the block boundary is not a fact about
the pad. Reading the audio thread's table from the control thread would be the
data race this protocol exists to remove.

## PadConfig: what a pad is

"Each pad can have different plugins and settings" was spread across three
milestones, which is how it ends up invented three times. One model, specified
once, filled in as the DSP lands:

```
PadConfig {
  sample + slice range          // M3, built
  amp envelope, choke group     // M3, built
  tuning, declick fades         // M3, built
  reverse, loop mode            // M5, when the DSP exists
  gain, pan, sends              // M5
  EQ / comp / saturation        // M5
  ordered insert chain          // M8
}
```

- **It carries its own pad index**, so the handoff ring stays a plain channel of
  owning handles instead of needing an envelope struct with its own move
  semantics. The engine validates the index on arrival rather than trusting it —
  it crossed a thread boundary, exactly like `PadEvent::pad`.
- **No `std::string` in it.** This is what the *audio thread* dereferences; a
  pad's display name belongs to `tui::PadState`, which already has one, and its
  project metadata to M6's save file.
- **Reverse and loop are deliberately absent** until the DSP for them lands in
  M5. A field nothing honours is worse than no field.
- **Declick is positional and separate from the envelope.** They solve different
  problems, and conflating them produces a sampler that clicks: a slice boundary
  lands wherever the chop put it and the waveform steps to and from zero there,
  which is a property of *position*; the envelope is musical and triggered by
  *events*. Zero-crossing snap removes most of the step, and the fade is the
  backstop for when it cannot. Clamped to half the slice at trigger time, so the
  two fades can never overlap however short the slice.
- **Envelope segments are linear and denominated in frames.** One add per frame,
  no transcendental in the callback, and frame-denominated so the block size
  cannot change the shape — the same reasoning that fixed the M2 meter fall.
- **Release falls from wherever the level is now**, not from sustain. A pad
  choked 5 ms into a 200 ms attack must fall from the quiet level it actually
  reached; jumping up to sustain first is an audible click and the classic way to
  get one.

## The sequencer runs on the audio thread

Not a design preference — the acceptance forces it. `docs/ROADMAP.md` asks for
"recorded pattern renders bit-exact offline", and offline bounce calls
`Engine::render()` in a plain loop with no control thread in existence. A
sequencer that generated events from the control thread would be simpler to write
and impossible to reproduce offline; it does not fail a taste test, it fails the
milestone. `tests/e2e/sequencer_e2e_test.cpp` is the proof: it renders patterns
with nothing running but `render()`.

Two consequences shape everything else about it.

**The pattern data obeys every `src/rt/` rule**, because the audio thread reads
it: fixed size, trivially copyable (asserted), no pointers, no allocation,
reached through one `shared_ptr<const SequencerState>` swapped whole through a
`HandoffRing`. That is the reconfiguration protocol carrying a second payload
with no change to it, which is what this document argued it was worth building
once for.
Editing one step means building a new state and publishing it — about 16 KB
memcpy per keystroke, which is microseconds at typing speed and buys the
one-pointer rule.

**Step positions are computed from the absolute frame, never accumulated.** Same
idiom as `chop_grid()`: `step_frame(step, rate, bpm, swing)` is a pure function of
the step index, so rounding cannot drift and the answer cannot depend on the block
size. An accumulator advanced once per block gives different results at 64 frames
and at 2048 — which is how a sequencer ends up a few milliseconds out after four
minutes, and how block-size invariance dies. Swing is folded into the step's
absolute position rather than applied as a delay, for the same reason. All of it
is integer arithmetic, so there is no float to round differently on another target.

The scan starts one step *before* the block, deliberately: a swung step can land
inside a block while its unswung base sits before it, and starting at the exact
index would silently drop it. `step_index_at()` and `step_scan_start()` are named
for their two different jobs because the transport readout used the wrong one
once and reported every step one early.

**M6 serialises `rt::SequencerState` directly.** It is plain data with no
indirection precisely so the project file has a model to write out rather than a
second one to invent. Adding a pointer, a `std::string` or a `std::vector` to it
breaks that and breaks RT-safety in the same stroke.

The metronome is **not** a voice. Routing a click through the voice pool would
give it a pad index, a glow slot and a telemetry entry it has no business owning,
and would let a dense pattern steal its voice. It is a `constexpr` table mixed
straight into the output buffer.

## MIDI input has a ring of its own

`rt::SpscRing` is **single-producer** — the header says so and TSan is the
authority on it. RtMidi delivers on a thread of its own, so a MIDI callback
calling `Engine::trigger_pad()` would put a second producer on the keyboard's
ring. It would appear to work.

So MIDI gets `Engine::submit_midi_event()` and `m_midi_events`, drained in the
same loop as the keyboard's by one templated function — the two paths differ in
capacity and producer thread, not in what happens to an event once it arrives. A
second copy of that loop is how they would drift into behaving differently.

Decoding bytes into a `PadEvent` is a **pure function** in `src/io/midi_message.cpp`
with no RtMidi types in it at all, which is what lets the meaning of a note be
tested against literal byte arrays on a machine with nothing plugged in. The
velocity curve lives there, as `rt::PadEvent` has said since M1, so the engine
never has to know where a hit came from. `midi_device.cpp` is the part that needs
hardware and contains no interpretation.

One upstream defect is worth recording because it cost a day: RtMidi's CoreMIDI
backend declares `getCoreMidiClientSingleton()` as `throw()` and then calls
`error()`, which throws — so a failing `MIDIClientCreate` is `std::terminate`
before any `catch` can run. It is reachable from enumeration as well as
construction, because the client is a lazy singleton. Measured at 0 in ~240 runs
on dev, 1 in ~240 under ASan and 4 in 25 under TSan, so the backend-touching tests
skip under TSan with a stated reason — the same precedent as the RT guard.

## The onset pipeline

`:chop transient` is a worker-thread analysis that produces positions. The audio
thread never sees any of this code.

Mono downmix → 1024/256 STFT (PFFFT) → half-wave-rectified **log**-magnitude
spectral flux → adaptive median threshold (`δ + λ·median`) → peak pick with a
refractory gap → backtrack to the preceding energy minimum.

- **Flux, not amplitude.** A snare landing over a ringing kick barely changes the
  amplitude envelope while changing the spectrum completely.
- **Log magnitude, not linear.** A real kick's pitch-falling decay does change the
  spectrum, and on linear magnitudes it changes it enough to look like a second
  hit.
- **The threshold is adaptive and uses the median, not the mean.** A fixed
  threshold has to be re-tuned per file, and within one file either misses the
  quiet half or invents hits in the loud half. The median is not dragged upward by
  the very peaks being detected. An absolute floor sits under it, because a
  passage of near-silence has a near-zero median and every ripple in it would
  otherwise clear the bar.
- **Backtracking is not a refinement.** Flux peaks once the transient is well
  inside the analysis window, which is systematically *late*, and a slice that
  starts late has its transient clipped — the one artefact that makes chopped
  drums sound obviously wrong.
- **Zero-crossing snap has a give-up bound, not a risk budget.** The search runs
  outward, so it always finds the nearest crossing and a wider radius never picks
  a worse one. What the 64-frame bound protects against is there being no nearby
  crossing at all: inside a sustained low tone the next crossing can be a whole
  half-period away, which at 40 Hz is 12 ms — no longer an inaudible correction
  but a moved hit. Better to leave the boundary where the detector put it and let
  the declick fade handle the step.
- **It works on the mono sum.** A stereo file's channels cross at different
  frames, so no single boundary is a zero crossing in both; the sum is what a
  listener hears stepping.

Slices run from one onset to the next, so material *before* the first onset is in
no slice — leading silence or a count-in is not a chop, which is what a hardware
sampler does and what a player expects.

## The keyboard: negotiated, decoded, and handed back

Held pads need key *release*, and a terminal only reports it under the Kitty
keyboard protocol. The path is built to be safe by construction rather than by
care:

- **Ask, and only act on an answer.** cratedig writes `CSI ? u` once the loop is
  running and enables the flags only if a reply arrives. A terminal without the
  protocol never replies, so silence *is* the answer and no timeout is needed —
  there is no third state to wait in.
- **Flag 8 is load-bearing, not optional.** The spec is explicit that key events
  producing text are reported as plain UTF-8 unless the application requests key
  report mode, so release for `q w e r` requires routing **every** keystroke
  through CSI-u. Enabling it means owning the decoder, which is why the decoder is
  a pure `string_view -> optional<KeyEvent>` function with its own unit tests: a
  terminal quirk must not be able to take the keyboard down untested.
- **The flag stack is per-screen.** Terminals maintain separate stacks for the
  main and alternate screens, so FTXUI leaving the alternate screen restores the
  shell's keyboard state *even if the program crashes*. That is what makes the
  feature safe to ship, and it is a property of the protocol rather than of our
  cleanup code.
- **Both input paths feed one `KeyEvent`.** The legacy path and the CSI-u path
  converge before any binding is written, so the pad map exists once.
- **Auto-repeat is not a hit.** Holding a pad must sustain it, not machine-gun it
  at the terminal's repeat rate — in one-shot mode a repeat would steal a voice
  from itself thirty times a second.
- `--legacy-keys` never asks, which keeps the old path exercised on demand.

## Current state (M4.5)

CRATEDIG chops and sequences. `cratedig <file>` decodes it, resamples it to the
engine rate, builds a peak pyramid, puts it on pad 1 and draws the PERFORM screen
from `docs/design`. `:chop transient` then runs the onset pipeline, cuts the file
at the hits, snaps the boundaries to zero crossings and lays slice *n* on pad *n*
across the sixteen-pad grid — live, with the stream running, and clearing the
pads it does not fill. `qwer asdf zxcv 1234` play them; `Enter` opens EDIT, where
`[`/`]` step slices and `h l H L` nudge the two boundaries a frame at a time
against a zero-crossing ruler, with `u` to undo.

M4 made it play itself. `Tab` brings up the pattern lane, `[`/`]` move a step
cursor and `t` writes the selected pad onto the step under it; `p` runs the
transport, always from the top. Sixteen patterns of up to thirty-two steps each,
chained into a song by `:song 1 2 3`, with per-pattern swing, a `constexpr`
metronome and a tempo held as hundredths so 89.5 bpm survives the trip exactly.
A MIDI controller plays the same pads at real velocity through a ring of its own.
Sequenced hits light their pads differently from live ones, scheduled in listener
time against a latency figure M9 will measure and which is zero until it does.

M4.5 was the round of fixes that came from playing it rather than from the
roadmap. The pad map runs in pad order now -- `1234 / qwer / asdf / zxcv`, number
row on top where it is on the keyboard -- from **one table**, in `src/tui/keys.hpp`,
because there were two and nothing checked they agreed. Space is the transport
and `p` is its alias. `.` stops every sounding voice and the transport with it,
because a long one-shot could not otherwise be stopped once it started; `:stop N`
is the surgical version. And `:slot assign 1-8 1` fills a bank in one line.

Implemented:

- `src/rt/` — `kCacheLine`, `Result`, `SpscRing`, `GarbageRing`, `HandoffRing`,
  `RT_SCOPE`, `Sample`, `hermite4`, `PadEvent`, `PadConfig`, `Envelope`,
  `VoicePool` (slice-ranged playback, ADSR, declick fades, choke groups, tuning,
  sample-accurate start offsets), `SequencerState` and its step arithmetic, the
  `constexpr` metronome table.
- `src/engine/` — pad configs and sequencer states adopted per block, event drain
  from two rings, the step scan, voice mixing, garbage retirement, transport, and
  telemetry including pad glow. Still device-free and thread-free: it spawns
  nothing, so offline rendering stays single-threaded and reproducible, which is
  what makes the sequencer testable at all.
- `src/ingest/` — FFmpeg demux/decode, libsamplerate conversion at load, the peak
  pyramid, PFFFT-backed STFT, onset detection, zero-crossing snap and the slice
  model (`chop_transient`, `chop_grid`).
- `src/io/` — the RtAudio adapter and the RtMidi adapter, the only files that
  include those headers, plus `midi_message.cpp`, which is pure decoding and
  includes neither. Both backends are constructed on first use, so `--no-audio`
  initialises neither.
- `src/tui/` — FTXUI. `waveform.cpp` (braille, no FTXUI dependency),
  `ui_state.cpp` (the view model), `render.cpp`, `render_edit.cpp`,
  `render_mix.cpp` and `render_browse.cpp` (four pure layout functions over one
  `UiState`), `render_detail.cpp` (what they share), `keys.{hpp,cpp}` (the CSI-u
  decoder, and the one pad map -- a keyboard fact rather than a layout one),
  `command.cpp` (the `:` grammar), `completion.cpp` (what Tab offers, also pure),
  `theme.hpp`, `app.cpp`, `cli.cpp`.

Not yet built: recording, export and the project file (M6). See
`docs/ROADMAP.md`.

### Varispeed, and what it costs (M5.7)

`PadConfig::pitch_ratio` is multiplied into the phase step in
`VoicePool::step_for()` and has been since M3; M5.7 added the control, in both
units a person thinks in — `rt::pitch.hpp` converts, so the parser, the interface
and the keymap share one idea of what an octave is.

**The alias floor, measured rather than argued** (`tests/unit/pitch_test.cpp`).
Reading the source `r` times faster puts a component at `f` on `f*r`; once that
passes Nyquist it folds back to `|sample_rate - f*r|`. With 15 kHz in the source:
1.5× lands it at 22.5 kHz and nothing folds; 2× folds it to 18 kHz and 4× to
12 kHz, **both level with the musical tone**. Folding relocates energy, it does
not attenuate it.

Two earlier versions of that measurement were wrong in ways worth not repeating:
an unwindowed DFT leaks at −46 dB and reported −46 dB at unity, where the output
is a bit-exact copy; and a single tone *below* Nyquist/r cannot alias at all,
however fast it is played, so a 6 kHz probe measured nothing but interpolator
error. Aliasing needs content above `sample_rate / (2r)` to fold.

### The crate, as built (M5.5)

`ingest::SamplePool` is the session's loaded files, on the **control thread and
nowhere else**. The audio thread never sees it and never needs to: `rt::PadConfig`
already carries its own `shared_ptr<const rt::Sample>`, so a pad playing a slice
of one record while its neighbour plays another is a fact about what was
published, not a change to the callback. That is why this milestone is a
control-side model plus a UI and touched the audio thread only for the audition
lane.

Identity is a monotonic `FileId`, never reused, **not an index**. A pad names a
file; if identity were positional, unloading anything would silently re-point
every pad after it at the wrong material -- a bug that reads as a corrupt project
rather than as a bad index. `remove()` therefore needs no coordination with the
audio thread at all: dropping the pool's entry drops the pool's reference and no
more, and anything still sounding holds its own.

**The audition lane** is the one part that is not control-side. Every route to
sound before M5.5 was a pad trigger -- the audio thread plays `m_pads[N]` -- so
nothing could address material no pad held. A preview now travels on its own
`HandoffRing` to its own two-voice pool, excluded from pad glow, telemetry and
choke: it is not a pad and must not light one.

Two voices rather than one, and rather than sixteen. An arriving preview
*releases* the one before it, so replacing does not click; two is what that fade
needs and one more would only let two previews sound at once, which is not what
"let me hear this" means. `kStopAll` reaches the lane too -- the panic key means
silence, and for one milestone it did not.

### The mixer graph, as built (M5)

`render()` clears the caller's buffers, then clears the graph, then walks it in
one fixed order every block:

```
  voices ─► strip 0..15 ─► bus a..d ─► master (the caller's buffer) ─► out
              gain            gain        metronome, then limiter
              EQ
              compressor
              balance
```

Six things about it that the rest of the system depends on:

- **The shape is fixed and preallocated.** Sixteen strips and four buses, one
  flat allocation in the `Engine` constructor sized from
  `Config::max_block_frames` — about 320 KB at the defaults. No node is created
  or destroyed while running, so there is no topology to branch on in the
  callback. `rt::kNumBuses` and `rt::kDefaultBus` live in `src/rt/strip.hpp`.
- **The master is not a node.** It is the caller's output buffer, already cleared,
  which everything sums into. One buffer fewer and one copy fewer, and an offline
  bounce writes exactly where the device would have.
- **`rt::StripConfig` is nested inside `rt::PadConfig`**, not a parallel table —
  the one-pointer rule, as this file predicted. A fader move is a new PadConfig
  through the same handoff ring a sample load uses.
- **DSP STATE IS NOT IN THE CONFIG.** A published config is immutable and shared
  between the pad and every voice holding it; filter and envelope history belong
  to one strip in one engine. The `rt::Biquad` and `rt::Compressor` instances
  live in `Engine`, indexed by pad (and band, and channel).
- **The master section travels by value.** `rt::MasterConfig` — the four bus gains
  and the limiter — goes through an `SpscRing` rather than the shared_ptr
  handoff, because it owns nothing. That is the one-pointer rule applied rather
  than copied: `PadConfig` owns a `Sample` and cannot be swapped atomically any
  other way; a struct of floats can.
- **A default strip is bit-transparent**, deliberately and by test. Gain 1.0,
  balance centre, EQ and compressor bypassed, limiter off. Every committed hash
  in the project still holds, which is only possible because bypass means the
  samples are not touched rather than multiplied by something that rounds to one.

Full signal flow and the DSP definitions: `docs/MIXER.md`.

### What M5 inherited

- **A release has a declick floor**, `PadConfig::release_floor_frames`, defaulting
  to the same `kDefaultFadeFrames` the boundary fades use. `AdsrFrames::release`
  defaults to zero, so before M4.5 a default pad released from full scale fell to
  silence in one frame -- a step discontinuity, and a click that choke groups and
  gate note-offs had both made since M3. `Envelope::release()` takes the floor as
  a required argument rather than a defaulted one, so every call site states its
  policy; that is what turned up the third caller.
- **The reconfiguration protocol has now carried a second payload** — the
  sequencer state, alongside pad configs — with no change to `HandoffRing` for
  either. M5's channel strips are the third, and the strip *is* the mixer half of
  `PadConfig` rather than a parallel struct, so it may not even be a new payload.
- **`Config::output_latency_frames` exists and is zero.** It delays sequenced
  glow only, and the path is built and exercised at zero on purpose so M9 fills
  in a figure rather than bolting on a mechanism.
- **`rt::SequencerState` is M6's serialisation model**, deliberately. It is plain
  trivially-copyable data so the project file consumes the struct rather than
  inventing a second one. M5 must keep it that way.
- **Telemetry now carries the transport** as two packed words — position with the
  playing flag, and step/slot/pattern together — for the same reason the playhead
  and the pad glow are packed: the UI must never pair fields from two blocks and
  name a step the pattern does not have.
- **Reverse and loop are STILL `PadConfig` fields nothing honours.** M5 was the
  milestone that was supposed to bring the DSP for them and did not: the mixer is
  about level, routing and tone, and reverse is a playback direction on the phase
  accumulator. They move to M5.5 with the varispeed work, which is where the
  phase step is being touched anyway.

### Known limitations

These are deliberate scope boundaries, not oversights:

- **Reverse and loop mode do not exist.** They are `PadConfig` fields waiting on
  the DSP for them, now M5.5's varispeed — a field nothing honours is worse than
  no field.
- **A chop assigns slices to pads positionally**: slice *n* to pad *n*, and
  anything past sixteen is not on a pad at all. `:slot assign 17-24 1` reaches
  the rest in one line, and banks are M6.
- **A slice that is not on a pad cannot be played, including in EDIT.** Every
  route to sound is a pad trigger, so there is nothing to address material no pad
  holds — a chop of more than sixteen leaves the rest editable, drawable and
  silent. EDIT's `space` says so rather than doing nothing, which is as far as it
  can go without an audition path; that is M5.5's, alongside the browser that
  needs the same mechanism to preview a file it has not loaded.
- **`--no-audio` never drains the handoff rings**, because nothing renders.
  Enough chops in one session will fill the pad ring, and `publish_pad_config()`
  then refuses rather than dropping silently. That is what that mode is, not a
  leak to size around — but it stopped being defensible for the sequencer, where
  every step toggle publishes, so that path does not publish without a stream at
  all. The pad path is the same bug with a longer fuse.
- **The transport does not run under `--no-audio`.** Nothing calls `render()`, so
  the sequencer never advances; pressing play says so rather than lighting up a
  transport that cannot move. Neither the transport ring nor the sequencer
  handoff is written to in that mode, for the same reason: there is no consumer,
  and a queue of orders for a thread that does not exist eventually fills.
- **That the panic key stops the transport is not covered by a test.** The two
  halves are: `tests/unit/engine_render_test.cpp` shows `kStopAll` alone gets
  retriggered by the next step, and that `kStopAll` plus a transport stop stays
  silent. What is unasserted is that `src/tui/app.cpp` sends both — under
  `--no-audio` the transport cannot be observed to have stopped, and app.cpp is
  not reachable from a unit test. Removing the transport half passes every test
  in the suite; it was tried.
- **The sequencer has no MIDI clock and no CC mapping.** Clock sync would make
  tempo a value arriving from a third thread, against the determinism story the
  offline acceptance rests on. Both stay available for a later milestone.
- **Step velocity is stored and honoured but nothing writes it yet.** The grid
  holds 0–127 per cell and the engine plays it; M6's recording is what fills it
  in from what was played. Zeroing it on a toggle would be a silent loss the day
  something does.
- **Held pads need a terminal that answers `CSI ? u`.** In one that does not,
  `:pad gate` behaves as one-shot rather than sticking on. That is the honest
  degradation, and it is why the feature is gated on a reply rather than on a
  terminal-name guess.
- **Onset detection has no tempo model.** It finds hits, not beats, so a chop of
  a swung loop is a chop of what was played rather than of a grid. `:chop grid N`
  is the answer for material already in time.
- The playhead is drawn *inside* the wave panel rather than as a tick in its top
  and bottom border as the mockups show. Reaching into an FTXUI `window`'s border
  would mean hand-drawing the panel, and contorting FTXUI to imitate the mockups
  is what CLAUDE.md says not to do.
- The mockups' near-black background is not painted. Repainting a user's whole
  terminal is the "un-terminal flourish" to drop silently; structure comes from
  the colour roles instead.
- **Chopping blocks the interface**, for the same reason loading does: the
  analysis runs on the control thread, between two frames. Measured with an -O2
  build: 0.6 ms for a 0.9 s loop, 5 ms for 10.7 s, and 161 ms for the 5.5-minute
  `long_form_drums.flac` (1499 slices). Under about ten seconds of material it is
  imperceptible, which is the material this is for; the long-form case drops a
  few frames once. It moves to a worker with the ingest path in M6, and the
  pipeline is already worker-shaped — it takes a `Sample` and returns positions,
  touching no engine state.
- **Loading blocks the interface.** `cratedig long_form_drums.flac` shows nothing
  for 3.85 s: decode, resample and pyramid all happen before the first frame.
  Measured split on the 5.5-minute fixture — 3.85 s at a 48 kHz engine rate
  against 0.86 s at 44.1 kHz, where the resampler short-circuits to bit-exact
  passthrough — so about three of those seconds are
  `SRC_SINC_BEST_QUALITY` over 14.5 million frames. Moving ingest onto a worker
  and showing a loading state is M6's job; the pyramid itself is only ~46 ms of
  it and is not the problem.
- A library that writes to stderr while the UI is up will corrupt the display —
  there is no redirection. This is why `--no-audio` no longer constructs an
  RtAudio: on a Linux box with no sound card, libasound wrote several lines onto
  the terminal the TUI was drawing on.

---

# Planned architecture

Everything below is **specified, not built**. It is here rather than only in
`docs/ROADMAP.md` because each item constrains code that already exists.

M3 emptied most of this section: the reconfiguration protocol, `PadConfig` and
pad glow moved up into the built half, and the record lane and plugin chains now
*reuse* the protocol rather than each needing their own. That was the point of
writing three features down as one problem — it is cheaper to discover the shared
shape once, on paper, than three times in code.

M4 took the rest of pad glow with it, and gave the protocol a second payload
without changing it. What is left below is the record lane, the DSP that fills in the
rest of `PadConfig`, and the latency work — and the last of those is now a
figure to measure rather than a mechanism to design, because M4 built and
exercised the mechanism at zero.

Milestone placement is in `docs/ROADMAP.md`.

## The record lane

A fourth lane, symmetric with ingest: the audio thread produces, a worker
consumes.

- **Capture is a duplex stream.** `RtAudio::openStream` already takes input
  parameters; `src/io/` opens input and output together so both run on one
  callback with one clock. Two streams would mean two clocks and resampling
  between them.
- **The callback writes into preallocated chunks, never a growing buffer.**
  Recording length is unbounded and allocation is banned, so the worker owns a
  pool of fixed chunks and hands empty ones to the audio thread through one ring;
  the audio thread fills them and hands them back through another. Running out of
  empty chunks drops audio and increments a counter — the same back-pressure
  shape as `GarbageRing` overflow, and for the same reason: the alternative is
  allocating in the callback.
- **The worker assembles a `Sample`** from the chunks, resamples if the input
  device rate differs from the engine rate, and publishes it to a pad through the
  reconfiguration protocol above.
- **Monitoring is a routing decision, not a feature.** Input frames can be summed
  into the output within the same callback. Whether that is useful depends
  entirely on output latency — see below.
- **Determinism is unaffected.** Live input is non-deterministic by nature; it
  enters the system as a finished `Sample`, and `Engine::render()` still has the
  bit-exactness contract it has now.
- **Resampling the master bus** — recording what the sampler itself plays — needs
  the mixer (M5) and is otherwise the same path with a different source.

## Per-pad processing, above what M3 built

The `PadConfig` model and the one-pointer rule are built and documented above.
What remains is the DSP that fills in the rest of the struct:

- **A "plugin chain" is ordered and variable-length**, which means the chain
  object owns its instances and is swapped whole. Bypass is a flag in the new
  chain, not a mutation of the live one.
- **Plugin latency is per-pad and therefore per-path.** Sixteen pads with
  different chains have sixteen different latencies, so plugin delay compensation
  is a graph property, not a number — it is deliberately v2, and until then the
  UI must *report* per-pad latency rather than silently misalign.
- **Persistence and scripting come free** if the model is one struct: project
  save/load (M6) serialises it, and the Lua config tier (M7) reads and writes it.
  They do not each need their own idea of what a pad is.

## Output latency, and the honest Bluetooth answer

M4 built the hook this milestone fills in, so the shape is already fixed:
`Config::output_latency_frames` delays **sequenced** pad glow only, and is zero
until something measures it. Live triggers get no such treatment and cannot — the
reference is the player's own finger, and delaying the light would only add to
that gap. This is why the latency work is not only a Bluetooth concern.


Bluetooth adds 100–200 ms with SBC or AAC, roughly 40 ms with aptX Low Latency,
and 20–30 ms with LC3 / LE Audio. Those figures are approximate and belong to the
codec, the link scheduler and the sink's own buffering.

**None of that is fixable from inside this application, and the spec must not
pretend otherwise.** An application cannot choose the A2DP codec, cannot shorten
the sink's buffer, and cannot play a sound before the pad is hit. "Low-latency
Bluetooth playback" is not an achievable feature; there is no clever engineering
that recovers it.

What *is* achievable is worth doing, and no DAW does it well:

1. **Measure it, rather than trusting the report.** `RtAudio::getStreamLatency()`
   is an under-report on macOS: the CoreAudio backend reads only
   `kAudioDevicePropertyLatency` and ignores `kAudioDevicePropertySafetyOffset`
   and `kAudioStreamPropertyLatency` (verified in `RtAudio.cpp`). The trustworthy
   number comes from a **loopback calibration** — emit a click, capture it back
   through the input, correlate, and report round-trip frames. That is one
   measurement, stored per device.
2. **Compensate everything that can be compensated.** Anything *scheduled* can be
   shifted: sequencer events, the metronome, sequenced pad glow, and the
   alignment of material recorded while monitoring. This is the difference
   between "in sync but late" and "out of sync", and it is the whole ballgame for
   overdubbing.
3. **Say what cannot be.** Live triggering is irreducible. The interface should
   state the measured figure plainly — a latency readout in the mode line, and a
   warning when the output device is one where playing live will feel wrong.
4. **Add nothing ourselves.** Our contribution is block size plus the engine, and
   it is already near zero; a per-device latency budget breakdown
   ("engine 5.3 ms + device 182 ms") makes that visible and keeps it honest.
5. **Prefer the low-latency path where the OS exposes one.** Detect LC3 / LE Audio
   and report it; recommend a wired path for live playing. Detection and advice
   only — the codec is not ours to choose.

The valuable product here is a sampler that knows exactly how late it is and
corrects everything it can, on a class of device every other DAW simply tells you
not to use.
