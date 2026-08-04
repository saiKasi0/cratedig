// engine::place_hit / record_hit — what somebody played becoming what the
// pattern holds.
//
// The claims:
//
//   1. A hit exactly on a step records on that step, and one slightly early or
//      slightly late records on the SAME step. That is what quantising is.
//   2. Past halfway it records on the next one, and the tie is broken the same
//      way every time.
//   3. A coarser resolution snaps to fewer places, not to different ones.
//   4. Swing is respected: on a swung pattern a hit that lands where the
//      sequencer SOUNDS a step records on that step, rather than on the one
//      after it.
//   5. A take that runs past the end of a pattern wraps into it, and a take
//      across a song lands in the pattern that was playing.

#include "engine/take.hpp"

#include "rt/pad_event.hpp"
#include "rt/sequencer.hpp"

#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint32_t kRate = 48'000;

rt::SequencerState straight_state() {
  rt::SequencerState state;
  state.bpm_x100 = 12'000;  // 120 bpm: a sixteenth is exactly 6000 frames
  state.patterns[0].length = 16;
  return state;
}

// Frames per sixteenth at the fixture's tempo. Exact by construction, so the
// cases below can be arithmetic rather than approximation.
constexpr std::uint64_t kStepFrames = 6'000;

rt::PadHit hit_at(std::uint64_t frame, std::uint8_t pad = 3, std::uint8_t velocity = 100) {
  return rt::PadHit{.frame = frame, .pad = pad, .velocity = velocity};
}

}  // namespace

TEST_CASE("place_hit keeps a hit on the step it was aiming at", "[unit]") {
  const rt::SequencerState state = straight_state();

  // Dead on, a hair early, a hair late. All three are the same note played by a
  // person, and all three have to record in the same place or quantising is not
  // doing anything.
  const std::uint64_t target = 5 * kStepFrames;
  for (const std::int64_t offset : {std::int64_t{0}, std::int64_t{-800}, std::int64_t{900}}) {
    const auto frame = static_cast<std::uint64_t>(static_cast<std::int64_t>(target) + offset);
    const engine::HitPlacement where =
        engine::place_hit(state, kRate, engine::kQuantiseSixteenth, frame);
    INFO("offset " << offset << " frames");
    CHECK(where.absolute_step == 5);
    CHECK(where.step == 5);
    CHECK(where.pattern == 0);
  }
}

TEST_CASE("place_hit moves a hit past halfway onto the next step", "[unit]") {
  const rt::SequencerState state = straight_state();
  const std::uint64_t target = 5 * kStepFrames;

  // Just under half a step late stays; just over goes forward. Without both
  // halves this case would pass on a quantiser that always rounded down.
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseSixteenth, target + 2'999).absolute_step ==
        5);
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseSixteenth, target + 3'001).absolute_step ==
        6);

  // Exactly halfway is a tie, and it resolves the same way every time. Which way
  // matters less than that it is fixed: a take has to be reproducible.
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseSixteenth, target + 3'000).absolute_step ==
        5);
}

TEST_CASE("a coarser quantise snaps to fewer places", "[unit]") {
  const rt::SequencerState state = straight_state();

  // A hit on step 5 -- an off-beat sixteenth. At the native grid it stays; at
  // eighths it moves to the nearest even step; on the beat it moves to 4.
  const std::uint64_t frame = 5 * kStepFrames;
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseSixteenth, frame).absolute_step == 5);
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseEighth, frame).absolute_step == 4);
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseQuarter, frame).absolute_step == 4);
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseHalf, frame).absolute_step == 8);

  // And a hit already on a beat is left alone by every resolution, which is the
  // half that would fail if coarse quantising were shifting things rather than
  // snapping them.
  const std::uint64_t on_beat = 8 * kStepFrames;
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseSixteenth, on_beat).absolute_step == 8);
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseEighth, on_beat).absolute_step == 8);
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseQuarter, on_beat).absolute_step == 8);
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseHalf, on_beat).absolute_step == 8);
}

TEST_CASE("place_hit records a swung pattern where it sounds, not where the grid is", "[unit]") {
  rt::SequencerState state = straight_state();
  state.patterns[0].swing = 60;

  // Step 5 is odd, so the sequencer sounds it late. A player following what they
  // hear plays it there too.
  const std::uint64_t sounds_at = rt::step_frame(5, kRate, state.bpm_x100, state.patterns[0].swing);
  REQUIRE(sounds_at > 5 * kStepFrames);

  const engine::HitPlacement where =
      engine::place_hit(state, kRate, engine::kQuantiseSixteenth, sounds_at);
  CHECK(where.absolute_step == 5);

  // THE ANTI-VACUITY HALF. A quantiser that ignored swing would round this hit
  // to step 6, because it sits well past the straight grid's halfway line
  // between 5 and 6. Asserting that distance is what makes the case above a
  // claim about swing rather than about a small offset.
  const std::uint64_t straight_halfway = (5 * kStepFrames) + (kStepFrames / 2);
  REQUIRE(sounds_at > straight_halfway);
}

TEST_CASE("place_hit wraps a take back into the pattern", "[unit]") {
  const rt::SequencerState state = straight_state();

  // Step 18 of a 16-step pattern is step 2 of its second pass. The absolute
  // index is kept because it is what the arithmetic produced; the pattern index
  // is what gets written.
  const engine::HitPlacement where =
      engine::place_hit(state, kRate, engine::kQuantiseSixteenth, 18 * kStepFrames);
  CHECK(where.absolute_step == 18);
  CHECK(where.step == 2);
  CHECK(where.pattern == 0);
}

TEST_CASE("place_hit follows the song into whichever pattern is playing", "[unit]") {
  rt::SequencerState state = straight_state();
  state.patterns[0].length = 16;
  state.patterns[1].length = 16;
  state.song.order[0] = 0;
  state.song.order[1] = 1;
  state.song.length = 2;

  // Step 3 is in the first pattern; step 19 is step 3 of the second. Recording
  // over a chained song has to put the note where it was played, not always into
  // the selected pattern.
  CHECK(engine::place_hit(state, kRate, engine::kQuantiseSixteenth, 3 * kStepFrames).pattern == 0);

  const engine::HitPlacement second =
      engine::place_hit(state, kRate, engine::kQuantiseSixteenth, 19 * kStepFrames);
  CHECK(second.pattern == 1);
  CHECK(second.step == 3);
}

TEST_CASE("record_hit writes the step it placed, with the velocity played", "[unit]") {
  rt::SequencerState state = straight_state();

  REQUIRE(engine::record_hit(state, kRate, engine::kQuantiseSixteenth,
                             hit_at((7 * kStepFrames) + 400, 3, 92)));
  CHECK(state.patterns[0].steps[7][3].on);
  CHECK(state.patterns[0].steps[7][3].velocity == 92);

  // And nothing else moved. A recorder that set the right step and also
  // something else would pass any assertion that only looked at the right step.
  std::size_t on_count = 0;
  for (const auto& step : state.patterns[0].steps) {
    for (const rt::Step& cell : step) {
      on_count += cell.on ? 1 : 0;
    }
  }
  CHECK(on_count == 1);
}

TEST_CASE("record_hit never writes a step that is on and silent", "[unit]") {
  rt::SequencerState state = straight_state();

  // Velocity zero would otherwise be a note visible in the pattern lane that
  // makes no sound, which is a worse answer to "why can I not hear it" than the
  // one count this costs.
  REQUIRE(engine::record_hit(state, kRate, engine::kQuantiseSixteenth, hit_at(0, 2, 0)));
  CHECK(state.patterns[0].steps[0][2].on);
  CHECK(state.patterns[0].steps[0][2].velocity >= 1);
}

TEST_CASE("record_hit lets the last hit on a step win", "[unit]") {
  rt::SequencerState state = straight_state();

  REQUIRE(engine::record_hit(state, kRate, engine::kQuantiseSixteenth, hit_at(0, 1, 40)));
  REQUIRE(engine::record_hit(state, kRate, engine::kQuantiseSixteenth, hit_at(600, 1, 110)));

  // One cell per pad per step is the grid, so two hits that quantise together
  // are one note -- and it is the second one, which is what correcting yourself
  // mid-loop has to mean.
  CHECK(state.patterns[0].steps[0][1].velocity == 110);
}

TEST_CASE("record_hit refuses a pad that does not exist", "[unit]") {
  rt::SequencerState state = straight_state();
  CHECK_FALSE(
      engine::record_hit(state, kRate, engine::kQuantiseSixteenth, hit_at(0, rt::kNumPads, 100)));
}

TEST_CASE("place_hit survives a zero quantise rather than dividing by it", "[unit]") {
  const rt::SequencerState state = straight_state();
  // Not reachable through the UI, and this arrives from a config file in M7.
  CHECK(engine::place_hit(state, kRate, 0, 5 * kStepFrames).absolute_step == 5);
}
