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

Two ring types, because they have genuinely different invariants.

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

`rt::kCacheLine` is a fixed 64 rather than
`std::hardware_destructive_interference_size` for the same reason — that value
varies by compiler and standard library version, which would change struct layout
across toolchains.

## Module map

| Path | Owns | May depend on |
|---|---|---|
| `src/rt/` | SPSC rings, garbage ring, RT guard, `Result`, `Sample`, interpolator, voice pool, mixer graph, DSP primitives | nothing outside `src/rt/`; header-only where possible |
| `src/engine/` | Engine facade, transport, sequencer, offline bounce | `src/rt/` |
| `src/ingest/` | FFmpeg decode, resampling, peak pyramid, onset detection, yt-dlp subprocess | `src/rt/` types only |
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
  atomic increment: no allocation, no lock, legal in the callback.
- **Finishing** hands the voice's reference to the `GarbageRing`, and the janitor
  destroys it. A voice that cannot retire (ring full) keeps its reference and
  stays un-reusable until the next block, rather than dropping it.
- **Retriggering the same pad** reuses the reference already in the stolen voice.
  Rolling one pad is the most common thing anyone does with a sampler; retiring
  on every hit filled the ring and started dropping hits.

The pad table itself is **written only before the stream starts**
(`Engine::set_pad_sample`). The audio thread reads it without synchronisation,
which is what keeps triggering free of atomics. Hot-swapping a loaded pad needs
a publish protocol that arrives with the ingest path in M2/M3; offering it now
would be an API the tests could not honestly check.

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

## Current state (M2)

CRATEDIG shows you the audio. `cratedig <file>` decodes it, resamples it to the
engine rate, builds a peak pyramid, assigns it to pad 0, opens the default
output device, and draws the PERFORM screen from `docs/design`: a braille
waveform with a live playhead and a time ruler, a 4x4 pad grid with meters, a
tabbed information panel, and a mode line. Space plays; `h l` scroll, `+ -`
zoom, `f` fits, `g G` jump to the ends, `tab` switches the panel, `q` quits.

Implemented:

- `src/rt/` — `kCacheLine`, `Result`, `SpscRing`, `GarbageRing`, `RT_SCOPE`,
  `Sample`, `hermite4`, `PadEvent`, `VoicePool` (now carrying pad and per-block
  peak for telemetry).
- `src/engine/` — pad table, event drain, voice mixing, garbage retirement, and
  the published telemetry block. Still device-free and thread-free: it spawns
  nothing, so offline rendering stays single-threaded and reproducible.
- `src/ingest/` — FFmpeg demux/decode, libsamplerate conversion at load, and the
  peak pyramid.
- `src/io/` — the RtAudio adapter, and the only file that includes `RtAudio.h`.
  The backend is constructed on first use, so `--no-audio` never initialises one.
- `src/tui/` — FTXUI. `waveform.cpp` (braille, no FTXUI dependency),
  `ui_state.cpp` (the view model), `render.cpp` (the pure layout function),
  `theme.hpp` (colour roles), `app.cpp` (the loop), `cli.cpp` (`--version`,
  `--list-devices`).

The M1 termios shell is gone. FTXUI owns raw mode, signal handling and terminal
restoration.

Not yet built: chopping and the full pad map (M3), MIDI and the sequencer (M4),
the mixer graph (M5). See `docs/ROADMAP.md`.

### What M3 inherits

- **The wave panel's ruler row is a placeholder for slice markers.** The mockups
  draw numbered slice boundaries there; until there are slices, elapsed time is
  what a listener actually wants to read off a waveform. Replacing it is a
  change to `wave_ruler()` in `src/tui/render.cpp` and nothing else.
- **The pad grid is drawn but only pad 0 is wired.** The QWERTY map from the
  mockups is printed in the caption row already; binding all sixteen keys and
  the PERFORM-mode key handling is M3's.
- **`Voice::pad` exists** so choke groups have the attribution they need.
- The `pattern` tab is a selectable placeholder; M4 fills it in rather than
  re-cutting the layout.
- The six new CC0 percussion loops in the starter pack are chopping material.

### Known M2 limitations

These are deliberate scope boundaries, not oversights:

- One pad is wired to one key. The `PadEvent` path is general; the key map is not.
- `PadEvent::frame_offset` is carried but always zero — triggers land on block
  boundaries. Sample-accurate offsets are M4's job, and the field exists now so
  the wire format does not change when MIDI, the sequencer and the offline
  renderer all already speak it.
- Pad samples cannot be swapped while the stream runs: `set_pad_sample()` is
  pre-start only, because the audio thread reads the pad table without
  synchronisation. Hot-swapping needs a publish protocol, which arrives with the
  ingest path in M3/M6.
- Voices have no envelope, so a sample plays to its end and stops. Per-pad ADSR
  and choke groups are M3.
- The playhead is drawn *inside* the wave panel rather than as a tick in its top
  and bottom border as the mockups show. Reaching into an FTXUI `window`'s border
  would mean hand-drawing the panel, and contorting FTXUI to imitate the mockups
  is what CLAUDE.md says not to do.
- The mockups' near-black background is not painted. Repainting a user's whole
  terminal is the "un-terminal flourish" to drop silently; structure comes from
  the colour roles instead.
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
`docs/ROADMAP.md` because each item constrains code that already exists, and
because three of them turn out to be the same problem — which is worth writing
down once instead of discovering three times.

Milestone placement is in `docs/ROADMAP.md`.

## Live reconfiguration: one problem, one protocol

Three separate features need to change what the audio thread is using, while it
is using it:

- **Assigning a slice to a pad** (M3) — `:chop transient` then `slot assign`,
  with the stream running.
- **Recording into a pad** (M6) — the whole point is that the new sample is
  playable the moment it exists.
- **Changing a pad's plugin chain** (M8) — instances cannot be constructed or
  destroyed on the audio thread.

Today `Engine::set_pad_sample()` is documented pre-start only, because the audio
thread reads `m_pads` with no synchronisation at all. That is honest for M2 and
is a hard blocker for all three of the above.

**The protocol, once, for all of them:**

1. **Build off-thread.** The control thread constructs the whole new
   thing — a `Sample`, a plugin chain, a strip configuration — with allocation,
   file I/O and plugin instantiation all happening where they are allowed.
2. **Publish through an SPSC ring of owning handles.** A ring of
   `std::shared_ptr<const T>` works and needs no new primitive: the control
   thread *move-assigns* into a slot that is already empty, so no destructor runs
   on either side; the audio thread move-assigns out, leaving it empty again.
   The existing `SpscRing` requires trivially-copyable elements because it
   overwrites slots by assignment — so this is a **sibling** ring
   (`rt::HandoffRing`), not a change to that one.
3. **Swap on the audio thread**, which is a pointer exchange.
4. **Retire the displaced handle into the `GarbageRing`.** Exactly the mechanism
   voices already use. Nothing is destroyed on the audio thread, ever.

The pieces for this already exist and are tested. What is missing is the ring
that carries owning handles and the drain step in `Engine::render()`.

**The rule that falls out of it:** anything the audio thread reads which the
control thread can change must be reachable through one pointer, so that
swapping it is a single operation. That is a design constraint on `PadConfig`
and on the per-pad chain below, not an implementation detail — a config struct
the control thread mutates field by field cannot be published safely at all.

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

## PadConfig and per-pad processing

"Each pad can have different plugins and settings" is currently spread across
three milestones, which is how it ends up invented three times. One model,
specified once:

```
PadConfig {
  sample + slice range          // M3
  amp envelope, choke group     // M3
  tuning, reverse, loop mode    // M3
  gain, pan, sends              // M5
  EQ / comp / saturation        // M5
  ordered insert chain          // M8
}
```

- **The audio thread reaches it through one pointer per pad**, per the rule above.
  Editing a pad builds a new `PadConfig`, publishes it, and retires the old one.
  A struct edited in place cannot be published atomically.
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

## Pad glow

Pads should light on trigger and during playback. Most of the machinery exists —
per-pad peak is already published and already decays — but peak is the **wrong
signal**: it follows audio level, so a quiet sample barely lights its pad, and a
pad triggered at velocity 1 into silence does not light at all.

- **Publish frames-since-trigger and trigger velocity per pad**, alongside the
  existing peak. The audio thread sets the counter to zero on trigger and adds
  `num_frames` each block; two relaxed stores, into the telemetry block that
  already exists.
- **The UI maps that to a glow ramp**, independent of level: bold accent →
  accent → dim → off. In sixteen colours there is no glow, so the ramp is
  intensity and weight, and the brightest step may invert the cell.
- **Live and sequenced hits should look different** (M4). Hardware samplers
  distinguish "you played this" from "the machine played this", and the
  distinction is genuinely useful when overdubbing.
- **Sequenced glow belongs in listener time, not engine time.** With 180 ms of
  output latency the sound arrives 180 ms after the block that rendered it, so a
  pad that lights when the block renders lights early and looks wrong. Delaying
  the *sequenced* glow by the measured output latency lines it up with what is
  heard. Live triggers need no such treatment: the reference is the user's
  finger, and nothing can be done about that gap anyway.

That last point is why the latency work below is not only a Bluetooth concern.

## Output latency, and the honest Bluetooth answer

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
