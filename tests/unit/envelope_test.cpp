// rt::Envelope characterisation.
//
// No golden file, deliberately. CLAUDE.md asks for "known input -> known output"
// vectors for DSP units, and for a piecewise-linear envelope the closed form IS
// the specification: a committed table of numbers would be a weaker restatement
// of the same arithmetic, and one that could only ever be regenerated from the
// implementation it is supposed to check. So the expectations below are computed
// from the definition instead, the way interpolator_test compares against an
// analytically evaluated sine rather than against a recording of one.
//
// What that buys: nearly every assertion here is exact (==, not Approx). Linear
// interpolation between exactly-representable endpoints over a POWER-OF-TWO
// frame count is exact in binary floating point, so the segment lengths below
// are chosen to be powers of two -- a test that has to allow slop is a test that
// has stopped pinning anything down.
//
// The one exception is the long-segment case at the bottom, whose whole subject
// is rounding, and which says so.

#include "rt/envelope.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

// Runs the envelope for `frames` and collects what it produced.
[[nodiscard]] std::vector<float> run(rt::Envelope& env, std::size_t frames) {
  std::vector<float> out;
  out.reserve(frames);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    out.push_back(env.next());
  }
  return out;
}

constexpr rt::AdsrFrames kAdsr{.attack = 8, .decay = 4, .sustain = 0.5F, .release = 4};

}  // namespace

TEST_CASE("Envelope starts idle and silent", "[unit]") {
  rt::Envelope env;
  CHECK(env.idle());
  CHECK(env.level() == 0.0F);
  CHECK(env.next() == 0.0F);
  CHECK(env.idle());  // an idle envelope stays idle however often it is asked
}

TEST_CASE("Envelope walks attack, decay and sustain exactly", "[unit]") {
  rt::Envelope env;
  env.trigger(kAdsr);

  const std::vector<float> values = run(env, 20);

  // Attack: 8 frames from 0 towards 1, so the segment emits 0/8 .. 7/8 and the
  // endpoint 1.0 belongs to the first frame of the decay. That is what keeps the
  // two segments continuous with no repeated value at the join.
  for (std::size_t frame = 0; frame < 8; ++frame) {
    CHECK(values[frame] == static_cast<float>(frame) / 8.0F);
  }

  // Decay: 4 frames from 1.0 towards 0.5.
  CHECK(values[8] == 1.0F);
  CHECK(values[9] == 0.875F);
  CHECK(values[10] == 0.75F);
  CHECK(values[11] == 0.625F);

  // Sustain holds indefinitely; nothing here advances.
  for (std::size_t frame = 12; frame < 20; ++frame) {
    CHECK(values[frame] == 0.5F);
  }
  CHECK(env.stage() == rt::EnvStage::kSustain);
}

// No declick floor: these cases are about what the SPEC's release does, and the
// floor is the caller's policy on top of it (see PadConfig::release_floor_frames).
// The floored behaviour has its own case at the bottom of this file.
constexpr std::size_t kNoFloor = 0;

TEST_CASE("Envelope releases from sustain to silence and goes idle", "[unit]") {
  rt::Envelope env;
  env.trigger(kAdsr);
  static_cast<void>(run(env, 12));  // through attack and decay, into sustain
  REQUIRE(env.stage() == rt::EnvStage::kSustain);

  env.release(kNoFloor);
  const std::vector<float> values = run(env, 6);

  CHECK(values[0] == 0.5F);
  CHECK(values[1] == 0.375F);
  CHECK(values[2] == 0.25F);
  CHECK(values[3] == 0.125F);
  CHECK(values[4] == 0.0F);
  CHECK(env.idle());
}

TEST_CASE("Envelope releases from wherever the level actually is", "[unit]") {
  // The reason release() reads level() rather than assuming sustain. A pad
  // choked part-way up a long attack must fall from the quiet level it reached;
  // jumping to sustain first and then falling is an audible click, and it is the
  // classic way to get one.
  //
  // 128 and 32 rather than 100 and 25, so the level is a power-of-two fraction
  // and every expectation below is exactly representable. The same test with
  // round numbers passes here by luck -- 25 * fl(1/100) happens to round to
  // exactly 0.25 -- and luck is not something to depend on across libm versions.
  rt::Envelope env;
  env.trigger(rt::AdsrFrames{.attack = 128, .decay = 0, .sustain = 1.0F, .release = 4});

  static_cast<void>(run(env, 32));  // a quarter of the way up
  REQUIRE(env.level() == 0.25F);

  env.release(kNoFloor);
  const std::vector<float> values = run(env, 5);

  CHECK(values[0] == 0.25F);
  CHECK(values[1] == 0.1875F);
  CHECK(values[2] == 0.125F);
  CHECK(values[3] == 0.0625F);
  CHECK(values[4] == 0.0F);
  CHECK(env.idle());
}

TEST_CASE("Envelope handles zero-length segments", "[unit]") {
  SECTION("zero attack arrives at full scale immediately") {
    rt::Envelope env;
    env.trigger(rt::AdsrFrames{.attack = 0, .decay = 4, .sustain = 0.0F, .release = 0});
    const std::vector<float> values = run(env, 4);
    CHECK(values[0] == 1.0F);
    CHECK(values[1] == 0.75F);
  }

  SECTION("zero attack and decay land straight on sustain") {
    rt::Envelope env;
    env.trigger(rt::AdsrFrames{.attack = 0, .decay = 0, .sustain = 0.75F, .release = 0});
    CHECK(env.stage() == rt::EnvStage::kSustain);
    CHECK(env.next() == 0.75F);
  }

  SECTION("zero release silences on the spot, when nothing floors it") {
    rt::Envelope env;
    env.trigger(rt::AdsrFrames{.attack = 0, .decay = 0, .sustain = 1.0F, .release = 0});
    env.release(kNoFloor);
    CHECK(env.idle());
    CHECK(env.next() == 0.0F);
  }

  SECTION("an all-zero envelope is transparent, not silent") {
    // The default. A pad with no envelope configured must play at full level --
    // if this were silence, every unconfigured pad would be mute.
    rt::Envelope env;
    env.trigger(rt::AdsrFrames{});
    CHECK(env.next() == 1.0F);
    CHECK(env.next() == 1.0F);
  }
}

TEST_CASE("Envelope release on an idle envelope does nothing", "[unit]") {
  rt::Envelope env;
  env.release(kNoFloor);
  CHECK(env.idle());
  CHECK(env.next() == 0.0F);
}

TEST_CASE("Envelope stop silences immediately from any stage", "[unit]") {
  rt::Envelope env;
  env.trigger(rt::AdsrFrames{.attack = 100, .decay = 0, .sustain = 1.0F, .release = 100});
  static_cast<void>(run(env, 50));
  REQUIRE(env.level() == 0.5F);

  env.stop();
  CHECK(env.idle());
  CHECK(env.level() == 0.0F);
}

TEST_CASE("Envelope error does not grow with segment length", "[unit]") {
  // The reason the level is computed as start + position * step rather than
  // accumulated. A 400 ms attack at 48 kHz is 19 200 frames, and adding a step
  // that many times compounds the rounding of every one of them.
  //
  // Not asserted as exact: 1/19200 is not representable in binary floating
  // point, so `position * step` differs from `position / 19200` by a rounding
  // even when nothing has drifted -- the first version of this test compared the
  // two and failed by one ULP. What is asserted is the property that actually
  // distinguishes the two implementations: the error is bounded by a SINGLE
  // rounding wherever you look, rather than growing with position.
  constexpr std::size_t kFrames = 19'200;
  rt::Envelope env;
  env.trigger(rt::AdsrFrames{.attack = kFrames, .decay = 0, .sustain = 1.0F, .release = 0});

  const std::vector<float> values = run(env, kFrames);

  CHECK(values[0] == 0.0F);
  CHECK(env.stage() == rt::EnvStage::kSustain);

  // Reference in double, so what is measured is the envelope's error and not the
  // yardstick's -- the same discipline interpolator_test uses.
  double worst_early = 0.0;
  double worst_late = 0.0;
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    const double reference = static_cast<double>(frame) / static_cast<double>(kFrames);
    const double error = std::abs(static_cast<double>(values[frame]) - reference);
    if (frame < kFrames / 100) {
      worst_early = std::max(worst_early, error);
    } else {
      worst_late = std::max(worst_late, error);
    }
  }
  INFO("worst error: " << worst_early << " over the first 1%, " << worst_late << " over the rest");

  // Two ULP of 1.0f. An accumulating implementation would be orders of magnitude
  // outside this by the end of the segment.
  constexpr double kTolerance = 2.4e-7;
  CHECK(worst_late < kTolerance);

  // Monotone throughout: a drifting accumulator can wobble, and a wobble in an
  // amplitude envelope is a sound nobody asked for.
  for (std::size_t frame = 1; frame < kFrames; ++frame) {
    REQUIRE(values[frame] >= values[frame - 1]);
  }
}

TEST_CASE("Envelope retrigger restarts from the beginning", "[unit]") {
  rt::Envelope env;
  env.trigger(kAdsr);
  static_cast<void>(run(env, 12));
  REQUIRE(env.level() == 0.5F);

  env.trigger(kAdsr);
  CHECK(env.stage() == rt::EnvStage::kAttack);
  CHECK(env.next() == 0.0F);
}

TEST_CASE("Envelope release takes at least the floor, however short the spec", "[unit]") {
  // THE CLICK THIS FIXES. AdsrFrames::release defaults to zero, so a default pad
  // released from full scale fell to silence in a single frame -- a step
  // discontinuity, which is audible as a click and which choke groups and gate
  // note-offs both did from M3 until M4.5.
  rt::Envelope env;
  env.trigger(rt::AdsrFrames{});  // the default: no attack, no release, full sustain
  REQUIRE(env.next() == 1.0F);

  constexpr std::size_t kFloor = 4;
  env.release(kFloor);

  // Four frames of ramp, arriving at silence, and MONOTONIC -- a fall that
  // overshot or wobbled would be its own artefact.
  const std::vector<float> values = run(env, kFloor + 1);
  CHECK(values[0] == 1.0F);
  CHECK(values[1] == 0.75F);
  CHECK(values[2] == 0.5F);
  CHECK(values[3] == 0.25F);
  CHECK(values[4] == 0.0F);
  CHECK(env.idle());
}

TEST_CASE("Envelope release keeps the longer of the spec and the floor", "[unit]") {
  // The floor is a MINIMUM, not an override: a pad with a deliberate 200 ms
  // release must still get 200 ms. Getting this backwards would silently turn
  // every musical release into a declick, which is a much worse bug than the
  // click it was meant to fix -- and one nobody would look for here.
  rt::Envelope slow;
  slow.trigger(rt::AdsrFrames{.attack = 0, .decay = 0, .sustain = 1.0F, .release = 8});
  static_cast<void>(slow.next());
  slow.release(/*floor_frames=*/2);
  const std::vector<float> values = run(slow, 9);
  CHECK(values[4] == 0.5F);  // halfway down after four of eight frames
  CHECK(values[8] == 0.0F);
  CHECK(slow.idle());

  // And releasing from part-way up still starts where the level actually is,
  // which is the property the floor must not disturb.
  rt::Envelope rising;
  rising.trigger(rt::AdsrFrames{.attack = 128, .decay = 0, .sustain = 1.0F, .release = 0});
  static_cast<void>(run(rising, 32));
  REQUIRE(rising.level() == 0.25F);
  rising.release(/*floor_frames=*/4);
  CHECK(rising.next() == 0.25F);
}
