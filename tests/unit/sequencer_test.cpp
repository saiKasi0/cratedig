// The sequencer's data model and the arithmetic that places a step in time.
//
// The arithmetic is the whole risk in M4. A sequencer that is a millisecond out
// sounds fine and fails the acceptance; one that drifts sounds fine for the
// first eight bars. So these check exact frame numbers against hand-computed
// values rather than asserting that steps are "roughly evenly spaced".

#include "rt/sequencer.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint32_t kRate = 48'000;

}  // namespace

TEST_CASE("SequencerState is plain data", "[unit]") {
  // Not a style preference. The audio thread reads it, so it must be trivially
  // copyable and free of indirection; and M6 serialises this exact struct, so a
  // std::string or a pointer sneaking in here breaks the project file as well as
  // RT-safety. STATIC_CHECK so it fails at compile time, where the mistake is.
  STATIC_CHECK(std::is_trivially_copyable_v<rt::SequencerState>);
  STATIC_CHECK(std::is_trivially_copyable_v<rt::Pattern>);
  STATIC_CHECK(std::is_trivially_copyable_v<rt::Song>);
  STATIC_CHECK(std::is_trivially_copyable_v<rt::Step>);
  STATIC_CHECK(std::is_trivially_copyable_v<rt::TransportCommand>);
}

TEST_CASE("step_frame places steps at the tempo", "[unit]") {
  // 120 bpm, sixteenths: a beat is 0.5 s = 24000 frames, a step is 6000.
  // Chosen because the arithmetic is exact, so a wrong answer here is a wrong
  // formula rather than a rounding.
  constexpr std::uint32_t kBpm = 12'000;  // 120.00

  CHECK(rt::step_frame(0, kRate, kBpm, 0) == 0);
  CHECK(rt::step_frame(1, kRate, kBpm, 0) == 6'000);
  CHECK(rt::step_frame(4, kRate, kBpm, 0) == 24'000);   // one beat
  CHECK(rt::step_frame(16, kRate, kBpm, 0) == 96'000);  // one bar, 2 s
}

TEST_CASE("step_frame does not drift over a long session", "[unit]") {
  // THE reason this is computed from the step index instead of accumulated.
  //
  // 137.00 bpm at 48 kHz gives 525.5474... frames per step -- deliberately not
  // an integer. An accumulator that rounded each step would be a whole frame out
  // within a couple of bars and seconds out by the end of a track; computing
  // from the index means the error never exceeds the final truncation.
  constexpr std::uint32_t kBpm = 13'700;    // 137.00
  constexpr std::uint64_t kStep = 100'000;  // ~14.6 hours in

  // The exact rational answer, worked out independently of the implementation:
  // step * rate * 60 * 100 / (bpm_x100 * 4).
  const std::uint64_t expected = (kStep * kRate * 60 * 100) / (std::uint64_t{kBpm} * 4);
  CHECK(rt::step_frame(kStep, kRate, kBpm, 0) == expected);

  // And the average step duration is still right at that distance: total frames
  // divided by steps must be within one frame of the ideal.
  const double ideal = (60.0 * kRate * 100.0) / (kBpm * 4.0);
  const double measured = static_cast<double>(rt::step_frame(kStep, kRate, kBpm, 0)) / kStep;
  CHECK(measured > ideal - 0.001);
  CHECK(measured < ideal + 0.001);
}

TEST_CASE("step positions are strictly increasing, with and without swing", "[unit]") {
  // The block scan in render() walks forward from an estimate and stops when it
  // passes the end of the block. That is only correct if positions increase --
  // a swing that pushed a step past its neighbour would make steps vanish.
  for (const std::uint8_t swing :
       {std::uint8_t{0}, std::uint8_t{25}, std::uint8_t{50}, std::uint8_t{75}}) {
    std::uint64_t previous = 0;
    for (std::uint64_t step = 1; step < 256; ++step) {
      const std::uint64_t now = rt::step_frame(step, kRate, 13'700, swing);
      INFO("swing " << static_cast<int>(swing) << ", step " << step);
      REQUIRE(now > previous);
      previous = now;
    }
  }
}

TEST_CASE("swing delays the odd steps only", "[unit]") {
  constexpr std::uint32_t kBpm = 12'000;  // 6000 frames per step
  constexpr std::uint8_t kSwing = 50;     // half a step late

  // Even steps are exactly where they were.
  CHECK(rt::step_frame(0, kRate, kBpm, kSwing) == 0);
  CHECK(rt::step_frame(2, kRate, kBpm, kSwing) == 12'000);
  CHECK(rt::step_frame(4, kRate, kBpm, kSwing) == 24'000);

  // Odd steps are pushed by half a step: 6000 + 3000.
  CHECK(rt::step_frame(1, kRate, kBpm, kSwing) == 9'000);
  CHECK(rt::step_frame(3, kRate, kBpm, kSwing) == 21'000);

  // Zero swing is exactly straight, not approximately.
  CHECK(rt::step_frame(1, kRate, kBpm, 0) == 6'000);
}

TEST_CASE("swing is capped rather than trusted", "[unit]") {
  // A swing of 100 would put an odd step on top of the next even one and break
  // monotonicity. It arrives from the control thread, so it is clamped here
  // rather than asserted -- the audio thread does not get to abort on bad input.
  constexpr std::uint32_t kBpm = 12'000;
  const std::uint64_t capped = rt::step_frame(1, kRate, kBpm, rt::kMaxSwingPercent);
  CHECK(rt::step_frame(1, kRate, kBpm, 200) == capped);
  CHECK(rt::step_frame(1, kRate, kBpm, 255) == capped);
  CHECK(capped < rt::step_frame(2, kRate, kBpm, 200));
}

TEST_CASE("tempo is clamped, so a zero bpm cannot divide by zero", "[unit]") {
  // Same reasoning as the swing cap: this crossed a thread boundary. A bpm of
  // zero reaching the division would be a crash in the audio callback.
  CHECK(rt::step_frame(4, kRate, 0, 0) == rt::step_frame(4, kRate, rt::kMinBpmX100, 0));
  CHECK(rt::step_frame(4, kRate, 1'000'000, 0) == rt::step_frame(4, kRate, rt::kMaxBpmX100, 0));
  CHECK(rt::clamp_bpm_x100(0) == rt::kMinBpmX100);
}

TEST_CASE("step_scan_start never overshoots", "[unit]") {
  // Its contract is that it under-estimates, because the caller walks forward
  // from it. An estimate that landed PAST a step would silently drop that step,
  // which is a missing note nobody could trace back to here.
  constexpr std::uint32_t kBpm = 13'700;
  for (const std::uint8_t swing : {std::uint8_t{0}, std::uint8_t{75}}) {
    for (std::uint64_t step = 0; step < 512; ++step) {
      const std::uint64_t frame = rt::step_frame(step, kRate, kBpm, swing);
      const std::uint64_t estimate = rt::step_scan_start(frame, kRate, kBpm);
      INFO("swing " << static_cast<int>(swing) << ", step " << step << " at frame " << frame);
      REQUIRE(estimate <= step);
      // And it is not uselessly far back -- a walk of two is what the caller
      // budgets for.
      REQUIRE(step - estimate <= 2);
    }
  }
}

TEST_CASE("pattern_length treats zero as the full grid", "[unit]") {
  // A length of zero would be a modulo by zero in the step wrap. Reading it as
  // the full grid is the benign interpretation and means a default-constructed
  // Pattern behaves.
  rt::Pattern pattern;
  pattern.length = 0;
  CHECK(rt::pattern_length(pattern) == rt::kMaxSteps);

  pattern.length = 16;
  CHECK(rt::pattern_length(pattern) == 16);

  // Longer than the grid is clamped to it rather than read past the end.
  pattern.length = 200;
  CHECK(rt::pattern_length(pattern) == rt::kMaxSteps);
}

TEST_CASE("a default sequencer state is silent and valid", "[unit]") {
  // The state a session starts in. Every step off, so publishing an untouched
  // state and pressing play produces silence rather than a wall of every pad.
  const rt::SequencerState state;
  CHECK(state.bpm_x100 == rt::kDefaultBpmX100);
  CHECK(state.song.length == 0);

  for (const rt::Pattern& pattern : state.patterns) {
    for (const auto& step : pattern.steps) {
      for (const rt::Step& cell : step) {
        REQUIRE_FALSE(cell.on);
      }
    }
  }
}
