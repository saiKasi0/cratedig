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
| `src/rt/` | SPSC rings, garbage ring, RT guard, `Result`, voice pool, mixer graph, DSP primitives | nothing outside `src/rt/`; header-only where possible |
| `src/engine/` | Engine facade, transport, sequencer, offline bounce | `src/rt/` |
| `src/ingest/` | FFmpeg decode, resampling, peak pyramid, onset detection, yt-dlp subprocess | `src/rt/` types only |
| `src/tui/` | FTXUI components: waveform, pad grid, mixer, command line | `src/engine/` via messages |
| `src/lua/` | sol2 bindings, config loader, chop-algo and macro API | `src/engine/` |
| `src/host/` | CLAP hosting; LV2 via lilv | `src/rt/` process interface |
| `src/io/` | RtAudio + RtMidi device layer — the **only** files that may include those headers | `src/engine/` |

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
| FTXUI | M2 | |
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

## Current state (M0)

Implemented: `rt::kCacheLine`, `rt::Result`, `rt::SpscRing`, `rt::GarbageRing`,
`RT_SCOPE`, and `engine::Engine::render()` producing deterministic silence.
Everything else in the map above is a placeholder for the milestone that builds
it — see `docs/ROADMAP.md`.
