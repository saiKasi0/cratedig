// The metronome click table.
//
// It is built at compile time from two hard-coded angles, which is only safe
// because these tests pin those literals against the real trigonometric
// functions. Without them a typo in the tenth decimal would produce a click
// slightly off pitch and nothing would ever say so.

#include "rt/click.hpp"

#include "rt/sequencer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr double kTableRate = 48'000.0;

}  // namespace

TEST_CASE("the click angles match the trigonometry they stand in for", "[unit]") {
  // The reason hard-coding them is acceptable. Same discipline as the constexpr
  // cosine in window.hpp, which is checked against std::cos rather than trusted.
  const double accent_w = 2.0 * std::numbers::pi * 1'600.0 / kTableRate;
  const double beat_w = 2.0 * std::numbers::pi * 1'000.0 / kTableRate;

  // 1e-15 is about four ulps of a double near 1.0 -- tight enough that a
  // mistyped digit anywhere in the literal fails, loose enough to survive the
  // last-bit differences between one platform's libm and another's.
  CHECK(std::abs(rt::kAccentCos - std::cos(accent_w)) < 1e-15);
  CHECK(std::abs(rt::kAccentSin - std::sin(accent_w)) < 1e-15);
  CHECK(std::abs(rt::kBeatCos - std::cos(beat_w)) < 1e-15);
  CHECK(std::abs(rt::kBeatSin - std::sin(beat_w)) < 1e-15);
}

TEST_CASE("the click table is a decaying sine at the right pitch", "[unit]") {
  // The recurrence has to actually generate sin(n*w). A wrong sign or a swapped
  // seed still produces something oscillating, so this compares against the real
  // sine sample by sample rather than counting zero crossings.
  const double w = 2.0 * std::numbers::pi * 1'000.0 / kTableRate;

  double envelope = 1.0;
  double worst = 0.0;
  for (std::size_t n = 0; n < rt::kClickFrames; ++n) {
    const double taper =
        static_cast<double>(rt::kClickFrames - 1 - n) / static_cast<double>(rt::kClickFrames - 1);
    const double expected = std::sin(static_cast<double>(n) * w) * envelope * taper;
    worst = std::max(worst, std::abs(static_cast<double>(rt::kClickBeat.samples[n]) - expected));
    envelope *= rt::kClickDecay;
  }

  // A two-pole recurrence is marginally stable, so error accumulates over 1200
  // iterations. It is computed in double and stored as float, so the bound is
  // about float precision rather than about the recurrence.
  INFO("worst absolute deviation from sin(n*w): " << worst);
  CHECK(worst < 1e-6);
}

TEST_CASE("the click starts and ends at silence", "[unit]") {
  // Both ends matter and for different reasons. The start must be zero because
  // the click is ADDED to a buffer that may already have audio in it, and a
  // non-zero first sample is a step. The end must be EXACTLY zero, which is what
  // the linear taper is for -- an exponential alone leaves a small step, and a
  // step at the end of a click is a second click.
  CHECK(rt::kClickAccent.samples.front() == 0.0F);
  CHECK(rt::kClickBeat.samples.front() == 0.0F);
  CHECK(rt::kClickAccent.samples.back() == 0.0F);
  CHECK(rt::kClickBeat.samples.back() == 0.0F);
}

TEST_CASE("the click is audible and does not clip", "[unit]") {
  // The anti-vacuity guard: every assertion above holds for a table of zeros.
  const auto peak_of = [](const rt::ClickTable& table) {
    float peak = 0.0F;
    for (const float value : table.samples) {
      peak = std::max(peak, std::abs(value));
    }
    return peak;
  };

  CHECK(peak_of(rt::kClickAccent) > 0.5F);
  CHECK(peak_of(rt::kClickBeat) > 0.5F);

  // At full scale before the gain is applied, so accent and beat together with a
  // pattern underneath still have headroom.
  CHECK(peak_of(rt::kClickAccent) <= 1.0F);
  CHECK(peak_of(rt::kClickBeat) <= 1.0F);
}

TEST_CASE("the click decays monotonically in envelope", "[unit]") {
  // Not sample-by-sample -- it is a sine, so it goes up and down. What must
  // decrease is the peak of each successive cycle; a recurrence that was
  // accumulating energy instead of losing it would show up here as a click that
  // grows into a tone.
  const std::size_t window = 48;  // one cycle of 1000 Hz at 48 kHz
  float previous = 1.0F;
  for (std::size_t start = 0; start + window <= rt::kClickFrames; start += window) {
    float peak = 0.0F;
    for (std::size_t n = start; n < start + window; ++n) {
      peak = std::max(peak, std::abs(rt::kClickBeat.samples[n]));
    }
    INFO("cycle starting at frame " << start);
    REQUIRE(peak <= previous);
    previous = peak;
  }
}

TEST_CASE("a click is shorter than a beat at the fastest tempo", "[unit]") {
  // THE invariant the metronome mixer's simplicity rests on: at most one click
  // sounds at a time. Stated as a test rather than a comment, because the two
  // numbers it relates live in different headers and neither one's author would
  // think to check the other.
  //
  // 300 bpm is the clamped maximum; 8 kHz is far below any rate this will run
  // at and is here to show the margin is not marginal.
  for (const std::uint32_t rate : {8'000U, 44'100U, 48'000U, 96'000U}) {
    const std::uint64_t beat_frames = rt::step_frame(rt::kStepsPerBeat, rate, rt::kMaxBpmX100, 0);
    INFO("rate " << rate << ": beat is " << beat_frames << " frames, click is "
                 << rt::kClickFrames);
    REQUIRE(beat_frames > rt::kClickFrames);
  }
}
