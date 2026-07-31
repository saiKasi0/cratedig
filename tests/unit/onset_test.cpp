// Spectral-flux onset detection, against material whose onsets are known by
// CONSTRUCTION rather than by labelling.
//
// That is the point of doing it this way: every hit here was placed at a frame
// this file chose, so "did it find them" is a fact rather than an opinion, and
// the test can never skip. The accuracy figures on real CC0 material live in
// onset_accuracy_test.cpp, where the ground truth is inspected rather than
// constructed and the test says so.

#include "ingest/onset.hpp"

#include "rt/sample.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint32_t kRate = 48'000;

// A percussive hit: a burst of band-limited noise with a fast exponential
// decay, which is what a drum looks like to a spectral-flux detector.
//
// Deterministic by construction -- an integer recurrence rather than <random>,
// so the same test runs the same way on every platform and every standard
// library.
void add_hit(std::vector<float>& signal, std::size_t at, float amplitude, std::size_t decay_frames,
             std::uint32_t seed) {
  std::uint32_t state = seed;
  for (std::size_t offset = 0; offset < decay_frames && at + offset < signal.size(); ++offset) {
    state = (state * 1'664'525U) + 1'013'904'223U;
    const auto noise =
        (static_cast<float>(static_cast<std::int32_t>(state >> 8U) % 2'000) / 1'000.0F) - 1.0F;
    const float envelope = 1.0F - (static_cast<float>(offset) / static_cast<float>(decay_frames));
    signal[at + offset] += amplitude * envelope * envelope * noise;
  }
}

[[nodiscard]] rt::Sample sample_from(const std::vector<float>& mono) {
  rt::Sample sample{kRate, 1, mono.size()};
  const std::span<float> data = sample.mutable_channel(0);
  std::copy(mono.begin(), mono.end(), data.begin());
  return sample;
}

// How each detection lines up with the nearest true onset, and whether any true
// onset was missed. Returned rather than asserted so the cases below can report
// the measured numbers.
struct Alignment {
  std::size_t matched = 0;
  std::size_t spurious = 0;
  std::ptrdiff_t worst_early = 0;  // negative: detection before the true onset
  std::ptrdiff_t worst_late = 0;
};

[[nodiscard]] Alignment align(const std::vector<std::size_t>& detected,
                              const std::vector<std::size_t>& truth, std::size_t tolerance) {
  Alignment out;
  std::vector<bool> claimed(truth.size(), false);

  for (const std::size_t at : detected) {
    bool matched = false;
    for (std::size_t index = 0; index < truth.size(); ++index) {
      if (claimed[index]) {
        continue;
      }
      const auto delta =
          static_cast<std::ptrdiff_t>(at) - static_cast<std::ptrdiff_t>(truth[index]);
      if (static_cast<std::size_t>(std::abs(delta)) <= tolerance) {
        claimed[index] = true;
        matched = true;
        ++out.matched;
        out.worst_early = std::min(out.worst_early, delta);
        out.worst_late = std::max(out.worst_late, delta);
        break;
      }
    }
    if (!matched) {
      ++out.spurious;
    }
  }
  return out;
}

// 25 ms. Wide enough that a 5.3 ms analysis hop cannot fail it on rounding,
// narrow enough that a detection this close is on the transient rather than
// merely in the neighbourhood.
constexpr std::size_t kTolerance = kRate / 40;

}  // namespace

TEST_CASE("onsets are found on an evenly spaced pattern", "[unit]") {
  constexpr std::size_t kHits = 16;
  constexpr std::size_t kSpacing = kRate / 4;  // 250 ms, a slow 240 bpm quarter
  std::vector<float> signal(kSpacing * (kHits + 1), 0.0F);

  std::vector<std::size_t> truth;
  for (std::size_t hit = 0; hit < kHits; ++hit) {
    const std::size_t at = kSpacing * (hit + 1);
    truth.push_back(at);
    add_hit(signal, at, 0.7F, kRate / 20, 1'000U + static_cast<std::uint32_t>(hit));
  }

  const rt::Sample sample = sample_from(signal);
  const ingest::OnsetResult result = ingest::detect_onsets(sample);
  const Alignment alignment = align(result.frames, truth, kTolerance);

  INFO("detected " << result.frames.size() << " of " << kHits << ", matched " << alignment.matched
                   << ", spurious " << alignment.spurious << ", offset [" << alignment.worst_early
                   << ", " << alignment.worst_late << "] frames");

  CHECK(alignment.matched == kHits);
  CHECK(alignment.spurious == 0);
}

TEST_CASE("onset positions land on the attack, not after it", "[unit]") {
  // The property that matters for chopping: a slice starting even a few
  // milliseconds late has its transient clipped off, and that is the artefact
  // that makes chopped drums sound obviously wrong. Early is survivable --
  // a little silence in front of a hit is inaudible.
  constexpr std::size_t kHits = 12;
  constexpr std::size_t kSpacing = kRate / 6;
  std::vector<float> signal(kSpacing * (kHits + 1), 0.0F);

  std::vector<std::size_t> truth;
  for (std::size_t hit = 0; hit < kHits; ++hit) {
    const std::size_t at = kSpacing * (hit + 1);
    truth.push_back(at);
    add_hit(signal, at, 0.8F, kRate / 25, 500U + static_cast<std::uint32_t>(hit));
  }

  const rt::Sample sample = sample_from(signal);
  const ingest::OnsetResult result = ingest::detect_onsets(sample);
  const Alignment alignment = align(result.frames, truth, kTolerance);
  REQUIRE(alignment.matched == kHits);

  INFO("offset range [" << alignment.worst_early << ", " << alignment.worst_late << "] frames at "
                        << kRate << " Hz");

  // Measured [0, 1] frames -- sample-exact, because backtracking to the foot of
  // the flux rise is followed by a sample-resolution search for where the
  // waveform actually starts moving. 64 frames is 1.3 ms, a 64x margin over the
  // measurement, and still an order of magnitude tighter than the analysis hop.
  CHECK(alignment.worst_late <= 64);
  CHECK(alignment.worst_early >= -64);
}

TEST_CASE("backtracking is what puts the position on the attack", "[unit]") {
  // The negative control for the backtrack step, run as a comparison rather
  // than by editing the source: the same signal, detected both ways.
  constexpr std::size_t kHits = 12;
  constexpr std::size_t kSpacing = kRate / 6;
  std::vector<float> signal(kSpacing * (kHits + 1), 0.0F);

  std::vector<std::size_t> truth;
  for (std::size_t hit = 0; hit < kHits; ++hit) {
    const std::size_t at = kSpacing * (hit + 1);
    truth.push_back(at);
    add_hit(signal, at, 0.8F, kRate / 25, 77U + static_cast<std::uint32_t>(hit));
  }
  const rt::Sample sample = sample_from(signal);

  ingest::OnsetParams without;
  without.backtrack = false;
  const Alignment raw = align(ingest::detect_onsets(sample, without).frames, truth, kTolerance);
  const Alignment backtracked = align(ingest::detect_onsets(sample).frames, truth, kTolerance);

  const std::ptrdiff_t raw_worst = std::max(std::abs(raw.worst_early), raw.worst_late);
  const std::ptrdiff_t fixed_worst =
      std::max(std::abs(backtracked.worst_early), backtracked.worst_late);

  INFO("worst offset from the attack: "
       << raw_worst << " frames without backtracking, " << fixed_worst << " with (early/late raw ["
       << raw.worst_early << ", " << raw.worst_late << "], fixed [" << backtracked.worst_early
       << ", " << backtracked.worst_late << "])");

  // The magnitude of the error either way, not just lateness. Without the
  // backtrack-and-refine pass a position is quantised to the analysis hop, so it
  // is off by up to 256 frames in whichever direction the grid happens to fall;
  // with it, the position is found at sample resolution.
  //
  // Asserted as a strict improvement rather than as the two measured numbers,
  // because how far off the raw position is depends entirely on where the hop
  // grid lands relative to each hit -- but that the pass helps does not.
  CHECK(raw_worst > 0);
  CHECK(fixed_worst < raw_worst);
}

TEST_CASE("a dense roll is not collapsed into one hit", "[unit]") {
  // 16th notes at 160 bpm: 93 ms apart, comfortably outside the 30 ms
  // refractory period but close enough that a sloppy median window smears them
  // together.
  constexpr std::size_t kHits = 24;
  constexpr std::size_t kSpacing = (kRate * 93) / 1'000;
  std::vector<float> signal(kSpacing * (kHits + 2), 0.0F);

  std::vector<std::size_t> truth;
  for (std::size_t hit = 0; hit < kHits; ++hit) {
    const std::size_t at = kSpacing * (hit + 1);
    truth.push_back(at);
    add_hit(signal, at, 0.6F, kRate / 40, 9'000U + static_cast<std::uint32_t>(hit));
  }

  const rt::Sample sample = sample_from(signal);
  const ingest::OnsetResult result = ingest::detect_onsets(sample);
  const Alignment alignment = align(result.frames, truth, kTolerance);

  INFO("matched " << alignment.matched << " of " << kHits << ", spurious " << alignment.spurious);
  CHECK(alignment.matched >= kHits - 1);
  CHECK(alignment.spurious <= 1);
}

TEST_CASE("two hits inside the refractory period are one onset", "[unit]") {
  // Deliberately closer than min_gap_seconds. Reporting both would mean a
  // one-transient slice, which is a chop nobody wants.
  std::vector<float> signal(kRate, 0.0F);
  add_hit(signal, kRate / 2, 0.8F, kRate / 40, 11U);
  add_hit(signal, (kRate / 2) + (kRate / 200), 0.8F, kRate / 40, 12U);  // 5 ms later

  const rt::Sample sample = sample_from(signal);
  const ingest::OnsetResult result = ingest::detect_onsets(sample);

  INFO("detected " << result.frames.size());
  CHECK(result.frames.size() == 1);
}

TEST_CASE("a slow swell produces no onsets", "[unit]") {
  // The false-positive case that matters. A pad or a cymbal wash rises
  // continuously, and a detector without an adaptive threshold fires all the
  // way up it.
  std::vector<float> signal(static_cast<std::size_t>(kRate) * 2, 0.0F);
  for (std::size_t frame = 0; frame < signal.size(); ++frame) {
    const double phase =
        2.0 * std::numbers::pi * 220.0 * static_cast<double>(frame) / static_cast<double>(kRate);
    const auto ramp = static_cast<float>(frame) / static_cast<float>(signal.size());
    signal[frame] = 0.8F * ramp * static_cast<float>(std::sin(phase));
  }

  const rt::Sample sample = sample_from(signal);
  const ingest::OnsetResult result = ingest::detect_onsets(sample);
  INFO("detected " << result.frames.size() << " onsets in a two-second swell");
  CHECK(result.frames.size() <= 1);  // the very start is legitimately an onset
}

TEST_CASE("hits are found over a noisy bed without inventing extras", "[unit]") {
  // What the ADAPTIVE half of the threshold is for, and the case crate-digging
  // actually presents: sampling off a noisy record, where there is broadband
  // hiss under everything.
  //
  // Continuous noise keeps the flux permanently non-zero, so a fixed threshold
  // either sits above it -- and misses quiet hits -- or below it, and fires
  // constantly on the hiss. A median-based threshold rises with the local noise
  // floor and lets the hits stand out of it.
  //
  // Written after discovering the gap: removing the median term entirely left
  // every other case in this file passing, which meant nothing here was
  // actually testing it.
  constexpr std::size_t kHits = 6;
  constexpr std::size_t kSpacing = kRate / 3;
  std::vector<float> signal(kSpacing * (kHits + 2), 0.0F);

  // The bed: continuous noise at about -20 dBFS.
  std::uint32_t state = 424'242U;
  for (float& value : signal) {
    state = (state * 1'664'525U) + 1'013'904'223U;
    value =
        0.1F *
        ((static_cast<float>(static_cast<std::int32_t>(state >> 8U) % 2'000) / 1'000.0F) - 1.0F);
  }

  std::vector<std::size_t> truth;
  for (std::size_t hit = 0; hit < kHits; ++hit) {
    const std::size_t at = kSpacing * (hit + 1);
    truth.push_back(at);
    add_hit(signal, at, 0.9F, kRate / 25, 8'000U + static_cast<std::uint32_t>(hit));
  }

  const rt::Sample sample = sample_from(signal);
  const ingest::OnsetResult result = ingest::detect_onsets(sample);
  const Alignment alignment = align(result.frames, truth, kTolerance);

  INFO("detected " << result.frames.size() << ", matched " << alignment.matched << " of " << kHits
                   << ", spurious " << alignment.spurious);

  CHECK(alignment.matched == kHits);

  // The discriminating assertion. Measured with the median term removed: 67
  // detections, 64 of them spurious, and only 3 of the 6 real hits matched --
  // because the noise floor swamps a fixed threshold in both directions at
  // once. With it: 6 detections, 6 matched, 0 spurious.
  CHECK(alignment.spurious <= 1);
}

TEST_CASE("silence produces no onsets", "[unit]") {
  const std::vector<float> signal(kRate, 0.0F);
  const rt::Sample sample = sample_from(signal);
  const ingest::OnsetResult result = ingest::detect_onsets(sample);
  CHECK(result.frames.empty());
  CHECK(std::all_of(result.flux.begin(), result.flux.end(),
                    [](float value) { return value == 0.0F; }));
}

TEST_CASE("degenerate input does not crash the detector", "[unit]") {
  SECTION("an empty sample") {
    const rt::Sample sample{kRate, 1, 0};
    const ingest::OnsetResult result = ingest::detect_onsets(sample);
    CHECK(result.frames.empty());
  }

  SECTION("a sample shorter than one analysis window") {
    const std::vector<float> signal(64, 0.5F);
    const rt::Sample sample = sample_from(signal);
    const ingest::OnsetResult result = ingest::detect_onsets(sample);
    // Whatever it decides, every position must be inside the sample.
    for (const std::size_t at : result.frames) {
      CHECK(at < 64);
    }
  }

  SECTION("a zero hop is refused rather than dividing by it") {
    const std::vector<float> signal(kRate, 0.5F);
    const rt::Sample sample = sample_from(signal);
    ingest::OnsetParams params;
    params.hop = 0;
    const ingest::OnsetResult result = ingest::detect_onsets(sample, params);
    CHECK(result.frames.empty());
  }
}

TEST_CASE("detection is deterministic", "[unit]") {
  std::vector<float> signal(kRate, 0.0F);
  for (std::size_t hit = 0; hit < 4; ++hit) {
    add_hit(signal, (kRate / 5) * (hit + 1), 0.7F, kRate / 30,
            31U + static_cast<std::uint32_t>(hit));
  }
  const rt::Sample sample = sample_from(signal);

  const ingest::OnsetResult first = ingest::detect_onsets(sample);
  const ingest::OnsetResult second = ingest::detect_onsets(sample);
  CHECK(first.frames == second.frames);
  CHECK(first.flux == second.flux);
}

TEST_CASE("onsets survive a stereo source", "[unit]") {
  // Detection runs on a mono sum, so a hit present in only one channel must
  // still be found -- at half the level, which the normalisation absorbs.
  constexpr std::size_t kHits = 8;
  constexpr std::size_t kSpacing = kRate / 5;
  std::vector<float> left(kSpacing * (kHits + 1), 0.0F);

  std::vector<std::size_t> truth;
  for (std::size_t hit = 0; hit < kHits; ++hit) {
    const std::size_t at = kSpacing * (hit + 1);
    truth.push_back(at);
    add_hit(left, at, 0.8F, kRate / 25, 401U + static_cast<std::uint32_t>(hit));
  }

  rt::Sample sample{kRate, 2, left.size()};
  std::copy(left.begin(), left.end(), sample.mutable_channel(0).begin());
  // Channel 1 is silent: the mono sum halves everything, and nothing else
  // should change.

  const ingest::OnsetResult result = ingest::detect_onsets(sample);
  const Alignment alignment = align(result.frames, truth, kTolerance);
  INFO("matched " << alignment.matched << " of " << kHits);
  CHECK(alignment.matched == kHits);
}
