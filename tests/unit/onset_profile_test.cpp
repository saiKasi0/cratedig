#include "ingest/onset.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Named transient profiles, and the band cut that makes one of them worth having.
//
// The profiles are parameter sets over one detector, so what has to be tested is
// not "does melodic use a different algorithm" -- it does not -- but "does any of
// this change the answer". A profile that produced the same chop as the default
// would be a name with nothing behind it.

namespace {

constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kTolerance = kRate / 40;  // 25 ms, as onset_test.cpp uses

[[nodiscard]] rt::Sample sample_from(const std::vector<float>& mono) {
  rt::Sample sample{kRate, 1, mono.size()};
  std::copy(mono.begin(), mono.end(), sample.mutable_channel(0).begin());
  return sample;
}

// Sustained tones stepping pitch, with a 20 ms attack -- an order slower than a
// drum, so there is no level transient anywhere. A new note is a change in the
// upper partials and nothing else, which is exactly the material the percussive
// defaults are not for.
struct Melodic {
  std::vector<float> signal;
  std::vector<std::size_t> truth;
};

[[nodiscard]] Melodic melodic_material() {
  constexpr std::size_t kNotes = 8;
  constexpr std::size_t kSpacing = kRate / 2;

  Melodic out;
  out.signal.assign(kSpacing * kNotes, 0.0F);

  float phase = 0.0F;
  for (std::size_t frame = 0; frame < out.signal.size(); ++frame) {
    const std::size_t note = frame / kSpacing;
    const float hz = 220.0F * std::pow(1.05946309F, static_cast<float>(note * 2));
    phase += 6.2831853F * hz / static_cast<float>(kRate);

    const std::size_t into = frame % kSpacing;
    const float attack = std::min(1.0F, static_cast<float>(into) / (0.020F * kRate));

    float value = 0.0F;
    for (int partial = 1; partial <= 6; ++partial) {
      value += std::sin(phase * static_cast<float>(partial)) / static_cast<float>(partial);
    }
    out.signal[frame] = 0.4F * attack * value;
  }
  for (std::size_t note = 1; note < kNotes; ++note) {
    out.truth.push_back(kSpacing * note);
  }
  return out;
}

// True onsets found, and detections that match none.
struct Score {
  std::size_t matched = 0;
  std::size_t spurious = 0;
};

[[nodiscard]] Score score(const std::vector<std::size_t>& detected,
                          const std::vector<std::size_t>& truth) {
  Score out;
  for (const std::size_t at : truth) {
    for (const std::size_t found : detected) {
      const auto delta = static_cast<std::ptrdiff_t>(found) - static_cast<std::ptrdiff_t>(at);
      if (static_cast<std::size_t>(std::abs(delta)) <= kTolerance) {
        ++out.matched;
        break;
      }
    }
  }
  out.spurious = detected.size() > out.matched ? detected.size() - out.matched : 0;
  return out;
}

}  // namespace

TEST_CASE("the finest density is exactly the detector's defaults", "[unit]") {
  // THE PROPERTY THAT KEEPS EVERY COMMITTED CHOP WHERE IT IS. `chop transient`
  // with nothing after it is `chop transient strum`, and if that were not the
  // struct's own defaults every golden in the project would have moved the day
  // densities landed.
  const ingest::OnsetParams defaults;
  for (const std::uint32_t bpm :
       {std::uint32_t{9'200}, std::uint32_t{12'000}, std::uint32_t{17'200}}) {
    const ingest::OnsetParams strum = ingest::params_for(ingest::ChopDensity::kStrum, bpm);
    INFO("bpm_x100: " << bpm);
    CHECK(strum.hop == defaults.hop);
    CHECK(strum.threshold_lambda == defaults.threshold_lambda);
    CHECK(strum.threshold_delta == defaults.threshold_delta);
    CHECK(strum.median_radius == defaults.median_radius);
    // The tempo does not reach it: a strum is a property of the sound.
    CHECK(strum.min_gap_seconds == defaults.min_gap_seconds);
    CHECK(strum.hf_emphasis == 0.0F);
    CHECK(strum.backtrack == defaults.backtrack);
  }
}

TEST_CASE("beat and bar densities follow the tempo", "[unit]") {
  // The whole reason params_for() takes a tempo: "a slice per beat" is not a
  // number of seconds until you know how long a beat is.
  const ingest::OnsetParams slow = ingest::params_for(ingest::ChopDensity::kBeat, 6'000);
  const ingest::OnsetParams fast = ingest::params_for(ingest::ChopDensity::kBeat, 18'000);

  // 60 bpm is a one-second beat; 180 is a third of that.
  CHECK(slow.min_gap_seconds > fast.min_gap_seconds);
  CHECK(slow.min_gap_seconds == Catch::Approx(0.5));
  CHECK(fast.min_gap_seconds == Catch::Approx(1.0 / 6.0));

  // A bar is four beats, so it is four times the gap at the same tempo.
  const ingest::OnsetParams bar = ingest::params_for(ingest::ChopDensity::kBar, 6'000);
  CHECK(bar.min_gap_seconds == Catch::Approx(slow.min_gap_seconds * ingest::kBeatsPerBar));

  // And coarser still in what it will fire on.
  CHECK(bar.threshold_lambda > slow.threshold_lambda);
}

TEST_CASE("a nonsense tempo falls back rather than dividing by it", "[unit]") {
  // bpm_x100 zero would make a beat infinitely long and min_gap infinite, which
  // is one slice covering the whole file -- the exact failure the roadmap names.
  const ingest::OnsetParams defaults;
  const ingest::OnsetParams beat = ingest::params_for(ingest::ChopDensity::kBeat, 0);
  CHECK(beat.min_gap_seconds == defaults.min_gap_seconds);
}

TEST_CASE("cutting the low bins is what the band control actually does", "[unit]") {
  // MEASURED, AND IT OVERTURNED THE FIRST IMPLEMENTATION. `hf_emphasis` began as
  // a weight ramp -- `1 + emphasis * (bin / last)` -- and on this material it did
  // nothing at all: 0, 0.4, 0.8, 2.0 and 6.0 all found the same seven of seven
  // true onsets and varied the total detection count by one. A smooth monotone
  // reweighting does not move where the flux peaks, and peak position is all
  // that survives normalisation and the adaptive threshold.
  //
  // Cutting the band does move it, and this is the number that says so.
  const Melodic material = melodic_material();
  const rt::Sample sample = sample_from(material.signal);

  ingest::OnsetParams flat;
  const Score without = score(ingest::detect_onsets(sample, flat).frames, material.truth);

  ingest::OnsetParams cut;
  cut.hf_emphasis = 0.4F;
  const Score with = score(ingest::detect_onsets(sample, cut).frames, material.truth);

  // Every real note still found...
  CHECK(without.matched == material.truth.size());
  CHECK(with.matched == material.truth.size());

  // ...and fewer inventions. This is the whole claim of the melodic profile.
  INFO("spurious without the cut: " << without.spurious << ", with: " << with.spurious);
  CHECK(with.spurious < without.spurious);
}

TEST_CASE("too much of the spectrum cut finds nothing, and cannot be asked for", "[unit]") {
  // The failure mode this clamp exists to make unreachable. Above 1 there are no
  // bins left, the detector returns nothing, and `:chop transient` falls back to
  // one slice covering the whole file -- which is exactly the symptom
  // docs/ROADMAP.md describes as a chop gone wrong. A typed number must not be
  // able to produce it.
  const Melodic material = melodic_material();
  const rt::Sample sample = sample_from(material.signal);

  ingest::OnsetParams absurd;
  absurd.hf_emphasis = 6.0F;
  const ingest::OnsetResult result = ingest::detect_onsets(sample, absurd);

  // Clamped, so it still detects rather than returning an empty set.
  CHECK_FALSE(result.frames.empty());

  // And the clamp is where the header says it is.
  ingest::OnsetParams at_limit;
  at_limit.hf_emphasis = ingest::kMaxHfEmphasis;
  CHECK(ingest::detect_onsets(sample, at_limit).frames == result.frames);
}

TEST_CASE("each density names itself and round-trips", "[unit]") {
  // The `:` grammar reads these names, so they live beside the parameters rather
  // than in the parser -- a name and a setting that can drift apart will.
  for (const ingest::ChopDensity density :
       {ingest::ChopDensity::kStrum, ingest::ChopDensity::kBeat, ingest::ChopDensity::kBar}) {
    ingest::ChopDensity parsed{};
    INFO("density: " << ingest::name_of(density));
    REQUIRE(ingest::density_from_name(ingest::name_of(density), parsed));
    CHECK(parsed == density);
  }

  ingest::ChopDensity ignored{};
  CHECK_FALSE(ingest::density_from_name("phrase", ignored));
  CHECK_FALSE(ingest::density_from_name("", ignored));
  CHECK_FALSE(ingest::density_from_name("BEAT", ignored));  // the parser is case-sensitive
}

TEST_CASE("no shipped density cuts the spectrum at all", "[unit]") {
  // MEASURED ON A REAL RECORD, not a preference. A band cut of 0.2 pushed the
  // first detection on distorted guitar from 0.05 s to 1.45 s, and 0.4 to
  // 2.64 s -- the opening riff was simply gone, because the discarded band is
  // the one a low-E riff lives in. The parameter stays reachable; no default
  // sets it.
  for (const ingest::ChopDensity density :
       {ingest::ChopDensity::kStrum, ingest::ChopDensity::kBeat, ingest::ChopDensity::kBar}) {
    const ingest::OnsetParams params = ingest::params_for(density, 12'000);
    INFO("density: " << ingest::name_of(density));
    CHECK(params.hf_emphasis == 0.0F);
    CHECK(params.threshold_lambda > 0.0F);
    CHECK(params.min_gap_seconds > 0.0);
  }
}
