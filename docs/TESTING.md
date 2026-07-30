# Testing strategy

Determinism is a feature, not a test convenience (CLAUDE.md). Most of what follows
exists to keep it that way.

## Layers

| Layer | Location | Label | What it proves |
|---|---|---|---|
| Unit | `tests/unit/` | `unit` | One class/function. No threads unless the class is a threading primitive. Fast (< 1 s each). |
| Stress | `tests/unit/`, tagged | `stress` | Concurrency primitives under load. Only meaningful under TSan. |
| Integration | `tests/integration/` | `integration` | Several modules together — e.g. the engine running under the RT allocation guard. |
| Fixture | tagged `[fixture]` | `fixture` | Needs the fetched CC0 starter pack. **Skips** when it is absent. |
| Device | tagged `[device]` | `device` | Needs real audio hardware. **Skips** in a container. |
| TUI snapshot | `tests/tui/` | `tui` | Offscreen renders of the layout and one PTY-driven session, against committed snapshots. Also the M2 frame-budget acceptance. |
| End-to-end | `tests/e2e/` (M3+) | `e2e` | Offline render + scripted TUI session, checked by output hash. |

Run a layer with `ctest --preset dev -L <label>`.

`fixture` and `device` tests stay in the default run rather than being excluded,
because a test that skips loudly is more useful than one nobody remembers to
invoke. Neither can pass vacuously: both `SKIP()` with the reason and the command
that would fix it.

### Nothing important is allowed to depend on them

Both skipping layers cover *material*, never *correctness*:

- Decoder correctness is pinned by a WAV assembled byte by byte in
  `tests/unit/decode_test.cpp`, with every expected float known exactly. No file,
  no network, no ffmpeg CLI — it cannot skip.
- Real-time safety is pinned by `tests/integration/rt_safety_test.cpp`, which
  runs the engine and voices under the allocation guard with no hardware at all.
- Determinism is pinned by synthetic samples built in the test, so the goldens
  never depend on a fetched file.

A machine with no sound card and no network still checks everything that matters.

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

**Invariance tests must render something.** The M0 version of this test rendered
silence, which is block-invariant no matter how badly the phase is handled — it
could not have failed. The current one plays voices at a *fractional* rate ratio
(44.1 kHz material on a 48 kHz engine), and asserts the output is not all zeros
so that three identical hashes cannot be three identical silences.

That is what the 32.32 fixed-point voice phase exists for; see ARCHITECTURE.md.
Negative-controlled by dropping the phase fraction at each block boundary, which
fails both invariance tests and passes again when restored.

The M0 silence golden `0xEB05052EA5B62325` is still asserted for an engine with
nothing triggered. It costs nothing to keep and guards the whole render path.

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

## DSP characterisation: the interpolator SNR budget

`tests/unit/interpolator_test.cpp` is the pattern every DSP unit should follow:
measure first, then commit thresholds derived from the measurement.

Method: interpolate a pure sine at a playback ratio and compare against the same
sine evaluated analytically at the output instants;
`SNR = 10·log10(Σ reference² / Σ error²)`. Reference and accumulators are
`double`, so what is measured is the kernel's error rather than the rounding of
the yardstick. No FFT is involved — which is also why PFFFT stays staged for M3.

**Ratios 1.0 and 2.0 are not SNR test cases.** At an integer ratio the fraction
is always exactly zero, so *every* kernel — Hermite, linear, nearest — returns
input frames untouched and scores ~154 dB, the float noise floor. Asserting an
SNR there measures nothing. The committed cases use 44100/48000, which visits
every fractional phase and is the ratio real 44.1 kHz material actually plays at.
Ratio 1.0 gets its own *exactness* test instead.

Measured at ratio 44100/48000, 44.1 kHz, 20 000 frames (AppleClang, arm64):

| Sine | Hermite | Linear | Gain | Committed threshold |
|---|---|---|---|---|
| 100 Hz | 146.8 dB | 94.6 dB | 52.1 dB | > 140 dB |
| 440 Hz | 110.9 dB | 68.9 dB | 42.0 dB | > 105 dB |
| 1 kHz | 89.4 dB | 54.7 dB | 34.8 dB | > 85 dB |
| 4 kHz | 51.5 dB | 30.6 dB | 20.9 dB | > 48 dB |
| 10 kHz | 24.1 dB | 15.0 dB | 9.0 dB | > 22 dB |

Thresholds sit roughly 3–6 dB below measurement — enough for libm differences
across platforms, not enough to hide a regression. **They fall steeply with
frequency because that is what a 4-point kernel does**; a partial approaching
Nyquist has almost no oversampling to work with. A uniform "> 90 dB everywhere"
would be unmeetable and would end up being lowered rather than investigated.

The suite asserts the **margin over linear** as well as the absolute figure. An
absolute threshold alone is weak: a kernel that quietly degraded still clears
140 dB at 100 Hz. The margin is what pins down that the outer two taps are being
used. Negative-controlled by degrading `hermite4` to linear — the polynomial,
cubic, SNR and margin tests all fail; the `t == 0` and passthrough tests
correctly still pass, because those properties hold for linear too.

The kernel reproduces polynomials of degree ≤ 2 exactly and **does not reproduce
cubics** — there is a test asserting the inexactness, because `f(x) = x³` comes
out exact at `t = 0.5` and a spot-check there invites someone to "strengthen"
the test into something that fails everywhere else.

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

A fixture may also be **assembled from several manifest-listed CC0 sources**: a
list-valued `derived_from` means "concatenate these, in this order", then loop
and trim to `derived_duration_seconds`. Every source must itself be a fixture in
the manifest, and `verify_fixtures.py` enforces that — a derived file inherits
its licence and provenance from its sources, so an unlisted source would be
provenance laundering rather than a transcode.

`long_form_drums.flac` is built that way: three different permutations of nine
CC0 percussion loops, looped to 5.5 minutes. Three permutations rather than one
order repeated, because a 39-second period is plainly visible as a repeat when
the whole file is on screen and 116 seconds is not.

**Why assembled and not fetched.** A real public-domain recording would be
better material. archive.org, where the public-domain jazz lives, was
unreachable when this landed; and it is a harder provenance question than it
looks — under the Music Modernization Act only US recordings first published in
or before 1925 are public domain, those are acoustic-era and were recorded with
the drums deliberately kept off the horn, and an archive.org uploader's "public
domain" tag is a claim rather than a rights determination. The hunt is deferred
to M3, which needs hand-labelled percussive material anyway and is the right
moment to verify a specific item properly.

Files that must be produced locally (a codec no CC0 source publishes directly) are
**transcoded from a manifest-listed CC0 source with ffmpeg** by the fetch script.
Their manifest row records the source file and the exact ffmpeg recipe. Their own
hash is recorded as `derived_sha256` and is **informational**: ffmpeg version
differences change encoder output, so derived files are verified by decoding them
back and comparing sample data within tolerance, not by hash equality.

### Required coverage

Fixtures are added at the milestone whose tests need them — an unused fixture is
dead weight and unverifiable provenance risk.

| Fixture role | Requirement | Needed by | Status |
|---|---|---|---|
| Per codec/container | One file for each format the decoder claims to support (WAV, FLAC first; MP3/AAC/OGG/OPUS/M4A later), transcoded locally where no CC0 original exists | M1 (WAV/FLAC), M6 (all codecs) | **done for M1** — `drum_heavy_kick.flac`, `kick_44k.wav` |
| Sample rates | At least one 44.1 kHz and one 48 kHz file so the resampler path is always exercised | M1 | **done** — the Sonic Pi files are 44.1 kHz, `kick_48k.wav` is derived |
| Stereo | A file with genuinely different left and right content, so a decoder that duplicates channel 0 is caught | M1 | **done** — `ambi_choir.flac`, `bd_haus.flac` |
| Long-form | One file > 3 minutes, for buffering/streaming and peak-pyramid stress | M2 | **done** — `long_form_drums.flac`, 5.5 min, assembled locally from nine CC0 sources |
| Percussive | Short percussive material **with hand-labeled onset ground truth committed as text** next to the manifest (`*.onsets.txt`, one time-in-seconds per line) | M3 | pending |
| Near-silent | Signal near the noise floor — metering, denormal, and auto-gain edge cases | M5 | pending |
| Clipped / loud | Material at or over 0 dBFS — limiter, clip indicator, and headroom paths | M5 | pending |

Onset ground truth is hand-labeled and committed; it is a golden file and falls
under the "never edit to make a test pass" rule above.

### Source URLs are pinned to a commit, not a tag

`raw.githubusercontent.com` serves whatever a ref currently points at, and tags
can be moved. Every fixture URL therefore names a commit SHA. A fixture that
changed underneath us would otherwise change test results without changing this
repository — and the checksum check would report it as *our* drift.

### Fixture-dependent tests skip, loudly

Fixtures are fetched and never committed, so a fresh clone has none. Tests that
need one are tagged `[fixture]` and call `SKIP()` with the command to run,
rather than failing — and rather than passing while asserting nothing.

**Decoder correctness does not depend on them.** `tests/unit/decode_test.cpp`
assembles a 16-bit PCM WAV byte by byte in the test source and asserts every
decoded float exactly; it needs no file, no network and no ffmpeg CLI, so it can
never skip. The starter pack adds what cannot be built by hand — real FLAC
frames, real stereo, a rate that is not the engine's.

Run them with `ctest --preset dev -L fixture` after `scripts/fetch_starter_pack.sh`.

### What is verified, and how it was checked

`scripts/verify_fixtures.py` runs inside `scripts/ci.sh`. All three of its gates
are negative-controlled rather than assumed:

| Gate | Control | Result |
|---|---|---|
| Checksum drift | flip one byte of a fetched fixture | fails, printing manifest vs on-disk hash |
| License policy | change a manifest row to `CC-BY-4.0` | fails, naming the allowed set |
| Skip-when-absent | move the fetched files away | tests skip with a reason; none fail, none pass |
| Derived provenance | name a source in `derived_from` that is not itself a fixture | fails, naming the unlisted source |

Derived (transcoded) fixtures are **not** hash-enforced — ffmpeg writes an
encoder tag into the WAV header and the bytes differ between builds. They are
validated by decoding instead: `kick_44k.wav` is a lossless transcode of
`drum_heavy_kick.flac`, so the two must decode to identical samples through two
different containers and two different decoders. That is a stronger check than a
hash, because it would also catch a transcode that silently resampled.

## The TUI snapshots

`tests/tui/snapshots/*.txt` are **the ground truth for what CRATEDIG's interface
is**; the mockups in `docs/design/` are what it aspires to (CLAUDE.md). A diff of
one character is a real change to the product and has to be explained in the
commit the same way a changed golden hash does.

Two layers, and they catch different things:

| Test | Renders | Catches |
|---|---|---|
| `layout_snapshot_test.cpp` | `UiState` literal → offscreen `Screen`, at 100x30, 80x24, 120x40, 60x20 and one below-minimum | Layout, degradation, colour roles |
| `pty_session.py` | The real binary under a pseudo-terminal, driven by keystrokes | That the program starts, keys reach it, and it hands the terminal back |

Removing the tab-key handler fails the PTY test with a precise row diff while all
nine offscreen snapshots still pass — they set the tab in the `UiState` directly.
That is the whole reason both exist.

**Determinism comes from there being nothing non-deterministic in scope.**
`render()` is a pure function of a plain struct: no engine, no device, no file, no
clock. The one piece of global state that would leak in is FTXUI's *detected*
colour support — whether the escape says `38;2;255;79;0` or `38;5;202` otherwise
depends on which terminal launched the test — and the test pins it.

Escape codes are stripped from the goldens. The colour roles are asserted
separately through `Screen::CellAt`, because an ANSI-laden golden diff is
unreadable and an unreadable diff gets accepted without being read.

### Things learned the hard way here

- **Trailing whitespace is content.** Every line of a terminal frame is padded to
  the full width, so pre-commit's `trailing-whitespace`, `end-of-file-fixer` and
  `mixed-line-ending` hooks rewrote all nine snapshots and broke the tests.
  `tests/tui/snapshots/` is excluded from them.
- **FTXUI redraws differentially**, so the byte stream with escapes stripped is
  the first screen plus a pile of edits, not the screen. `pty_session.py`
  contains a small terminal emulator for that reason; asserting on the raw
  stream would pass just as happily if every frame after the first were garbage.
- **Read boundaries split UTF-8.** Decoding each `read()` on its own turned
  whichever box-drawing character straddled a boundary into a replacement
  character — an intermittent failure by construction. Bytes are buffered and
  decoded once.
- **Uniqueness must go in the path, not the name.** The PTY fixture's filename
  reaches the screen, in the header and the pad label, so a PID in it made the
  snapshot non-deterministic. Found by running the test three times in a row.

`scripts/update_tui_snapshots.sh` regenerates both layers and prints the diff.
Look at the rendered output in a real terminal before running it (CLAUDE.md).

## The frame-budget acceptance

`tests/tui/waveform_perf_test.cpp` is the ROADMAP's M2 criterion — "5-minute file
scrolls at full frame rate" — made measurable. It **never skips**: the buffer is
synthesised in the test, so the acceptance holds with no network and no fixtures.
A `[fixture]`-tagged variant runs the same measurement on the real
`long_form_drums.flac`.

Measured on an M-series Mac, worst case over 200 scrolling frames at full
zoom-out, redrawing exactly as the app does:

| Implementation | Worst ms/frame | |
|---|---|---|
| correct | 0.004 | |
| level selection pinned to level 0 | 0.089 | correct, ~20x slower |
| no pyramid, raw frames | 6.84 | **what the budget catches** |

Pyramid build for five minutes of 48 kHz stereo: ~46 ms.

The committed budget is **2 ms per frame**: ~500x above the real figure and ~3x
below the no-pyramid figure. 60 fps is 16.67 ms for *everything*, and the
waveform is one panel of one frame while the audio thread has a hard deadline of
its own.

**It deliberately does not catch a level-selection regression**, and the test says
so. Pinning to level 0 is still correct and still 250x inside budget; choosing a
level *coarser* than the column fails three of `peak_pyramid_test`'s containment
tests instead, which is where a correctness question belongs. A perf threshold
that claimed to guard correctness would be worse than one that admits it does not.

Timing is only asserted where it means something. Under a sanitizer every load
goes through instrumentation, so the budget is not checked, the buffer drops to
30 seconds, and the `[fixture]` variant is skipped under TSan — at full size it
took the TSan suite from 5 seconds to 80, for a single-threaded test with nothing
for TSan to inspect.

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
