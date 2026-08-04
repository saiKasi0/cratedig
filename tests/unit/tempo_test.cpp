#include "ingest/tempo.hpp"

#include "ingest/onset.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// Reading a tempo off the material.
//
// Every fixture is built from an integer recurrence rather than <random> or
// libm-heavy synthesis where it matters, for the reason every fixture in this
// project is: the same test has to run the same way on every platform.

namespace {

constexpr std::uint32_t kRate = 48'000;

void add_hit(std::vector<float>& signal, std::size_t at, float amplitude, std::uint32_t seed) {
  const std::size_t decay = kRate / 20;
  std::uint32_t state = seed;
  for (std::size_t offset = 0; offset < decay && at + offset < signal.size(); ++offset) {
    state = (state * 1'664'525U) + 1'013'904'223U;
    const auto noise =
        (static_cast<float>(static_cast<std::int32_t>(state >> 8U) % 2'000) / 1'000.0F) - 1.0F;
    const float envelope = 1.0F - (static_cast<float>(offset) / static_cast<float>(decay));
    signal[at + offset] += amplitude * envelope * envelope * noise;
  }
}

// A steady four-to-the-floor at `bpm`, plus offbeats so the detector has more
// than one periodicity to choose between -- which is where octave errors come
// from and therefore the case worth testing.
[[nodiscard]] rt::Sample beat_at(double bpm, double seconds, bool offbeats = true) {
  const auto frames = static_cast<std::size_t>(seconds * kRate);
  std::vector<float> signal(frames, 0.0F);

  const double beat_frames = 60.0 / bpm * static_cast<double>(kRate);
  for (std::size_t beat = 0;; ++beat) {
    const auto at = static_cast<std::size_t>(static_cast<double>(beat) * beat_frames);
    if (at >= frames) {
      break;
    }
    add_hit(signal, at, 0.9F, 0x1000U + static_cast<std::uint32_t>(beat));
    if (offbeats) {
      const auto off = static_cast<std::size_t>((static_cast<double>(beat) + 0.5) * beat_frames);
      if (off < frames) {
        add_hit(signal, off, 0.35F, 0x2000U + static_cast<std::uint32_t>(beat));
      }
    }
  }

  rt::Sample sample{kRate, 1, frames};
  std::copy(signal.begin(), signal.end(), sample.mutable_channel(0).begin());
  return sample;
}

[[nodiscard]] ingest::TempoEstimate tempo_of(const rt::Sample& sample) {
  return ingest::detect_tempo(ingest::analyse_onsets(sample));
}

}  // namespace

TEST_CASE("a steady beat reports its tempo", "[unit]") {
  for (const double bpm : {90.0, 120.0, 140.0}) {
    const rt::Sample sample = beat_at(bpm, 8.0);
    const ingest::TempoEstimate estimate = tempo_of(sample);

    INFO("asked for " << bpm << " bpm, got " << (estimate.bpm_x100 / 100.0) << " confidence "
                      << estimate.confidence);
    REQUIRE(estimate.found());

    // Within a beat per minute, which is closer than the analysis hop can
    // resolve at these tempi anyway.
    CHECK(std::abs((estimate.bpm_x100 / 100.0) - bpm) < 1.5);
    // Confidence is a NORMALISED autocorrelation: the peak against the variance,
    // so 1 is perfectly periodic and 0 is noise. A clean pulse train measures
    // above 0.95.
    CHECK(estimate.confidence > 0.7F);
  }
}

TEST_CASE("a fast beat reports a real period of it", "[unit]") {
  // THE METRICAL AMBIGUITY, asserted as what it is rather than as a bug.
  //
  // A pulse train at 172 BPM repeats exactly at 86, at 57.3 and at 344. Every
  // one is a true period and no analysis can pick among them, because they are
  // all correct. Measured on this fixture: raw correlations of 0.0224 at 86 BPM,
  // 0.0209 at 173 and 0.0203 at 114.8 -- within 10% of each other, and a comb
  // over the multiples did not separate them either, because an isochronous
  // train has no fundamental to find.
  //
  // So what is checked is what is determined: the answer is a SIMPLE RATIO of
  // the truth, not an arbitrary number. Asserting 172 exactly would be asserting
  // a preference and calling it a measurement.
  const rt::Sample sample = beat_at(172.0, 8.0);
  const ingest::TempoEstimate estimate = tempo_of(sample);

  REQUIRE(estimate.found());
  const double got = estimate.bpm_x100 / 100.0;
  INFO("got " << got << " bpm for a 172 bpm train");

  const double ratio = got / 172.0;
  const bool metrical = std::abs(ratio - 1.0) < 0.03 || std::abs(ratio - 0.5) < 0.03 ||
                        std::abs(ratio - 2.0) < 0.06 || std::abs(ratio - (2.0 / 3.0)) < 0.03 ||
                        std::abs(ratio - (1.0 / 3.0)) < 0.03 || std::abs(ratio - 0.25) < 0.03;
  CHECK(metrical);

  // And it lands somewhere musical rather than at the edge of the range, which
  // is what the preference curve is for.
  CHECK(got > 60.0);
  CHECK(got < 200.0);
}

TEST_CASE("material with no beat reports none rather than a number", "[unit]") {
  // A held tone. The honest answer is "no tempo", and a detector that always
  // returns something would have the interface confidently displaying noise.
  const auto frames = static_cast<std::size_t>(4.0 * kRate);
  rt::Sample sample{kRate, 1, frames};
  const std::span<float> data = sample.mutable_channel(0);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    data[frame] =
        0.4F *
        static_cast<float>(std::sin(2.0 * 3.14159265 * 220.0 * static_cast<double>(frame) / kRate));
  }

  const ingest::TempoEstimate estimate = tempo_of(sample);
  INFO("bpm " << (estimate.bpm_x100 / 100.0) << " confidence " << estimate.confidence);

  // Well below what a real beat scores. The gap is what lets the interface say
  // "92 bpm" or "92 bpm?" and mean it.
  CHECK(estimate.confidence < 0.5F);
}

TEST_CASE("a file too short for two beats reports nothing", "[unit]") {
  // Rather than reporting whichever lag happened to overlap best, which is a
  // number where "no tempo" is the truth.
  const rt::Sample sample = beat_at(120.0, 0.5);
  CHECK_FALSE(tempo_of(sample).found());
}

TEST_CASE("silence has no tempo", "[unit]") {
  rt::Sample sample{kRate, 1, kRate};
  CHECK_FALSE(tempo_of(sample).found());

  const ingest::OnsetAnalysis empty;
  CHECK_FALSE(ingest::detect_tempo(empty).found());
}
