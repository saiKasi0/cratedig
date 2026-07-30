# Testing strategy

Determinism is a feature, not a test convenience (CLAUDE.md). Most of what follows
exists to keep it that way.

## Layers

| Layer | Location | Label | What it proves |
|---|---|---|---|
| Unit | `tests/unit/` | `unit` | One class/function. No threads unless the class is a threading primitive. Fast (< 1 s each). |
| Stress | `tests/unit/`, tagged | `stress` | Concurrency primitives under millions of operations. Only meaningful under TSan. |
| Integration | `tests/integration/` | `integration` | Several modules together — e.g. the engine running under the RT allocation guard. |
| TUI snapshot | `tests/tui/` (M2+) | `tui` | PTY-driven render of FTXUI components against committed snapshots. |
| End-to-end | `tests/e2e/` (M3+) | `e2e` | Offline render + scripted TUI session, checked by output hash. |

Run a layer with `ctest --preset dev -L <label>`.

## Catch2 conventions

- One test file per unit: `src/rt/spsc_ring.hpp` → `tests/unit/spsc_ring_test.cpp`.
- `TEST_CASE("what it does", "[unit]")` — the tag mirrors the ctest label.
- Prefer `STATIC_CHECK` for anything constexpr-evaluable; it moves the failure to
  compile time.
- `SECTION`s for variations that share setup; never for unrelated assertions.
- Tests may use exceptions and allocate freely — the `-fno-exceptions` constraint
  applies to `src/rt/` and `src/engine/` targets, not to test translation units.
  Every `src/rt/` header must therefore compile **both** ways; `tests/compile/`
  holds the `-fno-exceptions` compile check that guarantees it.

## Sanitizer matrix

| Preset | When it must pass |
|---|---|
| `dev` | Always. |
| `asan` + `ubsan` | Every task, before it is called done. |
| `tsan` | **Mandatory** for any change under `src/rt/`. |

**TSan is the authority on ring-buffer and atomic correctness.** A green single-
threaded unit test proves nothing about a lock-free ring; only the stress tests
under TSan do. A change to `src/rt/` with a red or skipped TSan run is not done,
regardless of how obviously correct it looks.

## Determinism policy

`Engine::render()` with a fixed seed and fixed input is bit-exact across runs on
the same platform. Tests enforce this in two ways:

1. **Block-size invariance** — rendering N frames as one call, as N/128 calls, and
   as a seeded-random sequence of block sizes must produce byte-identical output.
   This catches state that leaks across block boundaries.
2. **Golden hashes** — an FNV-1a hash over the raw output bytes, compared against a
   value committed in the test. A hash change is a *behavior* change: it must be
   explained and justified in the same commit, never silently updated.

## Golden vectors and golden audio

- Golden vectors (DSP input → expected output, committed under `tests/data/`) are
  written **before** the implementation is optimized, per CLAUDE.md.
- **Never edit a golden file to make a test pass** without a written justification
  that is audible or measurable — e.g. "filter redesigned, response now matches the
  analytic curve within 0.1 dB, plot attached". Byte-diffing golden audio is not
  review; plot it or listen to it.
- Golden vectors are numeric fixtures produced by us. They are not audio *sources*
  and are exempt from the fetch-don't-commit rule below (they are small, textual or
  short binary, and have no third-party provenance).

## Audio fixtures — CC0 only, fetched not committed

Source audio used by decode, onset, and metering tests is **never committed as a
binary**. It is fetched by `scripts/fetch_starter_pack.sh` and verified against
`assets/starter-pack/MANIFEST.toml`.

### Licensing rules (normative — see also docs/LICENSING.md)

- **CC0-1.0 or public domain only**, and the license must be verifiable at the
  source URL itself, not merely asserted by us.
- Acceptable sources: the Sonic Pi sample library (CC0 per its samples README),
  freesound.org files explicitly marked CC0, NASA / Library of Congress
  public-domain audio on archive.org.
- **Rejected**: "free for personal use", CC-BY (attribution-encumbered), any
  royalty-free-with-terms license, anything of unclear provenance, and **any
  Amen-break derivative regardless of how it is labeled** — including
  CC0-labelled sample-pack copies, because the underlying recording is not.
- A fixture whose provenance cannot be re-verified at its URL is removed, not
  grandfathered.

### Manifest

Every fetched file has a row in `assets/starter-pack/MANIFEST.toml` recording
`path`, `license`, `source_url`, `sha256`, plus a `used_by` note naming the tests
that consume it. `scripts/verify_fixtures.sh` recomputes every hash and fails on
drift; `scripts/ci.sh` runs it, so a changed or substituted fixture fails the
build rather than silently changing test results.

Files that must be produced locally (a codec no CC0 source publishes directly) are
**transcoded from a manifest-listed CC0 source with ffmpeg** by the fetch script.
Their manifest row records the source file and the exact ffmpeg recipe. Their own
hash is recorded as `derived_sha256` and is **informational**: ffmpeg version
differences change encoder output, so derived files are verified by decoding them
back and comparing sample data within tolerance, not by hash equality.

### Required coverage

Fixtures are added at the milestone whose tests need them — an unused fixture is
dead weight and unverifiable provenance risk.

| Fixture role | Requirement | Needed by |
|---|---|---|
| Per codec/container | One file for each format the decoder claims to support (WAV, FLAC first; MP3/AAC/OGG/OPUS/M4A later), transcoded locally where no CC0 original exists | M1 (WAV/FLAC), M6 (all codecs) |
| Long-form | One file > 3 minutes, for buffering/streaming and peak-pyramid stress | M2 |
| Percussive | Short percussive material **with hand-labeled onset ground truth committed as text** next to the manifest (`*.onsets.txt`, one time-in-seconds per line) | M3 |
| Near-silent | Signal near the noise floor — metering, denormal, and auto-gain edge cases | M5 |
| Clipped / loud | Material at or over 0 dBFS — limiter, clip indicator, and headroom paths | M5 |
| Sample rates | At least one 44.1 kHz and one 48 kHz file so the resampler path is always exercised | M1 |

Onset ground truth is hand-labeled and committed; it is a golden file and falls
under the "never edit to make a test pass" rule above.

## RT-safety testing

`src/rt/` correctness has three enforcement layers, tested as follows:

1. **`RT_SCOPE()` at runtime** — `tests/unit/rt_scope_test.cpp` installs a counting
   violation handler and asserts allocation inside a scope is detected and
   allocation outside one is not. `tests/integration/rt_scope_abort_test.cpp` runs
   the *default* handler in a separate process and is registered with
   `PASS_REGULAR_EXPRESSION`, proving the real abort path without relying on
   fragile exit-code semantics.
2. **`tests/integration/rt_safety_test.cpp`** — runs the engine under the guard for
   many render calls of varying block sizes; zero violations is the pass condition.
   This is the test CLAUDE.md names as the enforcement authority.
3. **clang-tidy** — `src/rt/.clang-tidy` bans the obvious offenders statically.

The guard catches allocations that go through C++ `operator new` (which is
everything our code does: `new`, `std::vector` growth, `std::string`,
`std::function`). Raw C `malloc` from a third-party library is not intercepted —
nothing in the callback is allowed to call into such a library, and malloc-level
interposition is tracked as later hardening in ARCHITECTURE.md.

**Allocation detection is compiled out in TSan builds** — TSan's runtime defines
the operator new/delete family itself and collides with ours (see
ARCHITECTURE.md). Those tests call `SKIP()` with a reason, so a TSan run reports
fewer tests than a dev run and says why. That is expected; the same assertions
run under `dev`, `asan`, and `ubsan`. If you are changing the guard, verify it on
`asan` — a green TSan run proves nothing about it.

## Adding a test

1. Put it in the right layer directory; add the source file to `tests/CMakeLists.txt`.
2. Tag it so the ctest label is right.
3. For DSP: characterization test with known input → known output **first**.
4. For anything touching `src/rt/`: add or extend a stress test and run `tsan`.
5. Run `ctest` on `dev`, `asan`, `ubsan` (and `tsan` if applicable) before calling it done.
