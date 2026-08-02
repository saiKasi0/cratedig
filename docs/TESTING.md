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

### When a golden hash may be committed, and when only invariance may be

The contract above says *on the same platform*, and that is the honest bound: a
hash of real audio is generally **not** portable. An `a*b+c` that one target fuses
into a single FMA and another does not differs in the last bit, and a hash
notices what the ear never would. arm64 always has FMA; baseline x86-64 does not.

So a committed constant is allowed only when the path under test provably has no
such expression in it, and the test must say which ones it avoided and why. The
M3 e2e (below) qualifies for two specific reasons — the phase fraction is exactly
zero, so the Hermite kernel collapses to an exact copy, and the pads use the
default flat-sustain envelope, so `Envelope::level()` never evaluates its
`m_start + m_step * position` ramp. Both were then **checked** on AppleClang/arm64
and clang-18/x86-64, not merely argued.

Anything outside that — a retuned pad, a real attack, decoded material through
libsamplerate — asserts run-to-run equality and block-size invariance instead. Two
runs on one machine agreeing is a real property and does not need a constant to
express it.

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
| Percussive | Short percussive material **with hand-labeled onset ground truth committed as text** next to the manifest (`*.onsets.txt`, one time-in-seconds per line) | M3 | **partly** — see below |
| Near-silent | Signal near the noise floor — metering, denormal, and auto-gain edge cases | M5 | pending |
| Clipped / loud | Material at or over 0 dBFS — limiter, clip indicator, and headroom paths | M5 | pending |

Onset ground truth is hand-labeled and committed; it is a golden file and falls
under the "never edit to make a test pass" rule above.
`scripts/verify_fixtures.py` checks every `*.onsets.txt` for a manifest-listed
audio file, parseable numbers, and ascending non-negative times — a malformed
line would otherwise parse as zero and quietly drag the measured accuracy down.

### The onset ground truth, and what it is not

The M3 acceptance is "onset precision/recall ≥ 0.9 on labeled set". *Which*
labeled set is the whole question, so the three used are described here rather
than left to be inferred from the test.

| Set | Ground truth from | Skips? |
|---|---|---|
| Synthetic patterns | **Construction** — hits placed at frames the test chose | never |
| Real hits, arranged | **Construction** — two CC0 kick recordings placed at known times | `[fixture]` |
| `drum_heavy_kick`, `bd_haus` | **Inspection** of the decoded waveform | `[fixture]` |

The two inspected files are single isolated hits. `bd_haus` is the one that
earns its place: it opens with ~5 ms of inaudible pre-ring before the real
attack, so a crude amplitude gate answers 0.23 ms where the onset is at 5 ms.
Each label file records how it was labelled, at sample resolution, in its own
header.

**The CC0 percussion loops are deliberately NOT labeled.** `loop_industrial`,
`loop_perc1`, `loop_tabla`, `loop_compus`, `loop_garzul` and `loop_safari` were
all examined at millisecond resolution while writing these tests, and every one
is a dense sustained texture — reverberant industrial noise, hand-percussion
washes, tabla rolls — rather than a pattern of discrete hits. None has isolated
attacks that could be labeled with confidence. Labeling them anyway would
produce ground truth that could not be defended, and a precision/recall figure
computed against it would be a number rather than a measurement.

**What is still missing**, stated plainly: a real performance with real timing
and overlapping hits, labeled by a human listening to it. The arranged-real-hits
set is the closest available substitute — real transients and real spectra, with
labels that are exact because the test placed them — but it has no timing feel
and no hits landing on top of each other. That gap needs ears, and archive.org
(where public-domain recordings live) has been unreachable from this machine
throughout M2 and M3.

**One thing the real material changed about the implementation.** The first
version of the detector used linear-magnitude spectral flux and scored precision
0.33 on the arranged real hits — 43 detections for 14 hits — while scoring a
perfect 1.0 on every synthetic case. The 29 extras were all *inside* the kick
decays, because a real kick's pitch-falling tail keeps pushing energy into bins
that were quiet. Log-magnitude flux fixed it: 14 for 14, none spurious. No
synthetic test could have found that, which is the argument for keeping real
material in the loop even when its labels have to be constructed.

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

## The M3 chop acceptance

`docs/ROADMAP.md` states M3's acceptance as one sentence — "import → `:chop
transient` → play chops end-to-end e2e script passes bit-exact" — and it is tested
in two layers, because the sentence makes two different claims.

| File | Answers |
|---|---|
| `tests/e2e/chop_e2e_test.cpp` | Does the pipeline produce the right audio? Real ingest and engine code, no terminal, no device, no file. |
| `tests/e2e/pty_chop_session.py` | Does typing those two words into the real binary do it? Real onset detection inside the real program. |

Both build their own percussive loop — eight hits, 0.25 s apart, after 0.1 s of
silence — from **integer arithmetic and no libm**, for the same reason
`write_fixture_wav()` does: `std::sin` is not required to give the same last bit
on two platforms, so a golden over material generated with it would be a golden
over the host's libm. And because the material is built rather than fetched, the
milestone's acceptance **cannot skip** — the one outcome an acceptance may never
have. The real starter-pack loop is covered by a `[fixture]` variant, which is an
addition rather than the acceptance.

What the offline half asserts, beyond the hash:

- The chop found the eight hits that were *constructed*, within 10 ms. Nothing is
  derived from what the detector reports, so the check is not circular — and a
  chop that fell back to one slice covering everything would otherwise produce
  entirely plausible audio and pass every other assertion in the file.
- **Each pad plays its own chop**, compared to the source frame by frame and
  bit-exactly. This is what makes it a chop test rather than a playback test: a
  hash cannot tell you all sixteen pads are playing the top of the file.
- Nothing was dropped — no refused pad config, no dropped trigger, no garbage-ring
  overflow.

Triggers are pushed at exactly their scheduled frame, by rendering up to it
first. The engine drains its event ring at the top of a block, so a trigger pushed
at an arbitrary moment would land at a different frame under a different block
size, and the invariance assertion would be failing for a reason that has nothing
to do with the engine.

Negative-controlled four ways, each fired: publishing every pad from frame 0
(fails the per-pad comparison *and* changes the hash), letting the render overshoot
to the next block boundary (fails block-size invariance on both the synthetic and
the fixture case), writing six hits into the PTY fixture while the assertion
expects eight, and sending `:chop grid 4` in place of `:chop transient`.

The PTY half deliberately does **not** assert that pads light. `--no-audio` opens
no device, so `render()` never runs and the audio thread never publishes a glow.
The acceptance item "pads light on trigger at any sample level" is asserted where
the signal exists — `tests/unit/engine_telemetry_test.cpp`, against a −60 dBFS
sample.

### M3 acceptance, item by item

`docs/ROADMAP.md` lists four criteria. Each is a test that runs in the default
suite, not a claim:

| Criterion | Where it is asserted |
|---|---|
| e2e chop script passes bit-exact | `tests/e2e/chop_e2e_test.cpp` — committed FNV-1a golden, block-size invariance, run-to-run equality |
| onset precision/recall ≥ 0.9 on a labeled set | `tests/unit/onset_accuracy_test.cpp` — `>= 0.9` on the synthetic set (never skips) and on the inspected loops (`[fixture]`) |
| a pad reassigned mid-stream allocates nothing and destroys nothing on the audio thread | `tests/integration/rt_safety_test.cpp` under the RT guard, and `tests/integration/engine_threading_test.cpp` under TSan |
| pads light on trigger **at any sample level** | `tests/unit/engine_telemetry_test.cpp` — against a −60 dBFS sample, which is why glow is driven from the trigger and not from `pad_peak` |

The last one is the reason `PadGlow` exists at all: peak follows audio level, so
a meter-driven pad would barely light for a quiet sample and would not light at
all for one triggered into silence. Negative-controlled by driving glow from
`pad_peak`, which fails exactly that test and nothing else.

### M4 acceptance, item by item

`docs/ROADMAP.md` states two criteria for M4. Both are tests that run in the
default suite, in the same two layers M3 established:

| Criterion | Where it is asserted |
|---|---|
| a recorded pattern renders bit-exact offline | `tests/e2e/sequencer_e2e_test.cpp` — committed FNV-1a golden, block-size invariance (straight *and* swung), run-to-run equality |
| MIDI integration test green | `tests/e2e/sequencer_e2e_test.cpp` — literal bytes through `decode_midi()` into the engine's MIDI ring and out as audio, which **cannot skip**; plus `tests/unit/midi_device_test.cpp`'s `[device]` case, which needs a port and skips loudly |

Two things the hash cannot say, asserted separately because a golden only proves
the output has not changed:

- **`e2e: every step lands on its own frame`** measures the onset of each hit on
  the rendered audio against `step_frame()`, at ragged block sizes. Every timing
  test would otherwise pass on an engine that fired each step at its block
  boundary — the bug sample-accurate offsets exist to prevent.
- **`e2e: swing pushes the odd steps late and leaves the even ones`** measures the
  shift in frames. A swing that moved every step, or moved them by the wrong
  amount, would still be block-size invariant and still hash consistently.

The PTY half (`tests/e2e/pty_sequencer_session.py`) writes the pattern by typing
at the real binary, and asserts what the offline test structurally cannot: that
the keys and the `:` verbs reach the **engine's** sequencer rather than a copy of
it in the interface. That is not a formality — it is what caught the sequencer
handoff ring filling up under `--no-audio`, where nothing calls `render()` so
nothing ever drains it and the ninth edit of a session was refused for good.
`tests/tui/pty_session.py` was making exactly seven.

**Why the M4 golden is portable**, in one line: every step is at velocity 127,
which is `127/127.0f == 1.0f` exactly, so the product in `amplitude * value` is
exact and an FMA cannot round it differently. The file says it at length, and the
rule it follows is the next section.

### M4.5, and what a test can and cannot claim

Three of the M4.5 changes are worth recording for the shape of their coverage
rather than for what they do.

**The pad map** is asserted twice, deliberately differently.
`tests/unit/keys_test.cpp` checks the lookup reads the table and that the order
is the rotated one — and says in the test that it *cannot* assert nobody adds a
second table, because one inside an anonymous namespace is unreachable from a
test. The end-to-end version is in `tests/e2e/pty_sequencer_session.py`: it reads
the key groups off the caption row **as painted**, presses the first key of the
first and last groups, and checks which lane row lights up. That is the only
place "the legend and the keyboard agree" is a claim about the program rather
than about a header.

**The panic key's transport half has no test, and the comment beside it says so.**
Two engine cases pin why it is needed — `kStopAll` alone is retriggered by the
next step, and `kStopAll` plus a transport stop stays silent — but that
`src/tui/app.cpp` sends both is unasserted. Under `--no-audio` the transport
cannot be observed to have stopped and app.cpp is not reachable from a unit test.
Removing the transport half passes the whole suite; it was tried. Writing that
down is worth more than a test that would have to fake the thing it checks.

**The handoff-ring trap was found twice, by a test that could not have found it
the first time.** `tests/e2e/pty_chop_session.py` assigned once and stayed under
the ring's depth, so it passed on a build where the fifth command would have been
refused. It now runs six rounds of eight assignments on top of a chop's sixteen —
48 publishes against a ring of 32 — which fails outright without a drain. The
lesson generalises: a test that exercises a bounded resource once proves the
first use works, and a bound is only observable by exceeding it.

**The release floor** broke three existing tests, which was the point: they
asserted a choked voice was gone at frame 0, and *that* was the click. They now
ask for a hard cut explicitly (`release_floor_frames = 0`) and go on testing
whether the release happened at all, while a new case covers the default. A
second envelope case asserts the floor is a MINIMUM and not an override — the
inverted version would turn every deliberate 200 ms release into a 0.7 ms
declick, which is a worse bug than the click and one nobody would look for here.

### The PTY harness reconstructs a screen from a byte stream, and that is a measurement

Worth writing down because it produced a convincing false positive. The sessions
capture bytes for a fixed wall-clock window and rebuild the grid from whatever
arrived; on a slow machine that window can end **in the middle of a three-byte
box-drawing character**. Decoded with `errors="replace"` the partial sequence
became a replacement glyph, painted at wherever the cursor had reached — one bad
cell, in the same place on every run, in the Docker CI only.

It looked exactly like a rendering bug and was not: the character completed on
the next read, and the full stream decoded cleanly under `errors="strict"`.
`decode_stream()` now drops a truncated tail and leaves genuine mid-stream
corruption visible, because a decoder that hid both would hide the failure this
one exists not to be mistaken for.

The general rule: **a test that samples a stream on a timer is measuring, not
observing.** When such a test disagrees with a golden, rule out the measurement
before changing the thing measured — and never re-baseline a snapshot to match
one.

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
