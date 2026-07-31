// The slice model, zero-crossing snap, and the two chop algorithms.

#include "ingest/slices.hpp"

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

[[nodiscard]] rt::Sample sine_sample(std::size_t frames, double hertz, float amplitude = 1.0F,
                                     std::uint16_t channels = 1) {
  rt::Sample sample{kRate, channels, frames};
  for (std::uint16_t channel = 0; channel < channels; ++channel) {
    const std::span<float> data = sample.mutable_channel(channel);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const double phase =
          2.0 * std::numbers::pi * hertz * static_cast<double>(frame) / static_cast<double>(kRate);
      data[frame] = amplitude * static_cast<float>(std::sin(phase));
    }
  }
  return sample;
}

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
  std::copy(mono.begin(), mono.end(), sample.mutable_channel(0).begin());
  return sample;
}

// A drum pattern: eight hits, evenly spaced, over silence.
[[nodiscard]] rt::Sample drum_pattern(std::size_t hits, std::size_t spacing) {
  std::vector<float> signal(spacing * (hits + 1), 0.0F);
  for (std::size_t hit = 0; hit < hits; ++hit) {
    add_hit(signal, spacing * (hit + 1), 0.8F, kRate / 25, 21U + static_cast<std::uint32_t>(hit));
  }
  return sample_from(signal);
}

}  // namespace

// --- the snap ----------------------------------------------------------------

TEST_CASE("the snap finds the nearest zero crossing", "[unit]") {
  // 100 Hz at 48 kHz is a 480-frame period, so crossings are 240 frames apart
  // and a boundary in the middle of a half-cycle has one comfortably in range.
  const rt::Sample sample = sine_sample(4'800, 100.0);

  // Frame 120 is the positive peak; the crossings either side are at 0 and 240.
  //
  // Within a frame of one of them, not exactly on it: sin(pi) evaluates to about
  // 1.2e-16 rather than to zero, so the sign change at the half-period lands one
  // frame later than the arithmetic says. That is a fact about floating point,
  // not about the snap, and asserting the exact frame would be asserting the
  // rounding.
  const std::size_t snapped = ingest::snap_to_zero_crossing(sample, 120, 240);
  INFO("snapped 120 -> " << snapped);
  const auto distance = std::min(static_cast<std::ptrdiff_t>(snapped),
                                 std::abs(static_cast<std::ptrdiff_t>(snapped) - 240));
  CHECK(distance <= 1);

  // From just after a crossing it snaps BACKWARD onto it, a couple of frames.
  //
  // The backward half of the outward search is what makes this work, and the
  // assertion has to say "it moved back", not merely "it did not move forward":
  // with a forward-only search there is no crossing within the radius at all,
  // so it would return 243 untouched and a `<=` would sail through. Verified by
  // deleting the backward half, which fails exactly here.
  const std::size_t near = ingest::snap_to_zero_crossing(sample, 243, 64);
  INFO("snapped 243 -> " << near);
  CHECK(near < 243);
  CHECK(243 - near <= 4);
}

TEST_CASE("the snap moves a boundary as little as possible", "[unit]") {
  // Searching outward means the first crossing found is the nearest one. A
  // search that scanned forward only would move a boundary up to a full radius
  // when a crossing sat one frame behind it.
  const rt::Sample sample = sine_sample(4'800, 100.0);
  for (std::size_t at = 100; at < 400; at += 7) {
    const std::size_t snapped = ingest::snap_to_zero_crossing(sample, at, 240);
    const auto moved =
        std::abs(static_cast<std::ptrdiff_t>(snapped) - static_cast<std::ptrdiff_t>(at));
    REQUIRE(moved <= 240);
  }
}

TEST_CASE("the snap gives up rather than moving a boundary too far", "[unit]") {
  // A boundary inside a sustained low tone has no crossing within the radius.
  // Returning the original position is the honest answer -- PadConfig's declick
  // fade exists for exactly this case, and moving the hit to fix a click would
  // trade one artefact for a worse one.
  const rt::Sample sample = sine_sample(48'000, 20.0);  // 2400-frame period
  const std::size_t at = 600;                           // the positive peak
  CHECK(ingest::snap_to_zero_crossing(sample, at, 64) == at);
}

TEST_CASE("the snap handles degenerate input", "[unit]") {
  const rt::Sample empty{kRate, 1, 0};
  CHECK(ingest::snap_to_zero_crossing(empty, 0, 64) == 0);

  const rt::Sample sample = sine_sample(1'000, 100.0);
  CHECK(ingest::snap_to_zero_crossing(sample, 500, 0) == 500);       // zero radius
  CHECK(ingest::snap_to_zero_crossing(sample, 5'000, 64) == 1'000);  // past the end
}

TEST_CASE("the snap reduces the step at a slice boundary", "[unit]") {
  // What it is for, measured. A boundary at a peak steps the full amplitude;
  // snapped to a crossing it steps by almost nothing.
  const rt::Sample sample = sine_sample(4'800, 100.0);
  constexpr std::size_t kPeak = 120;

  const float unsnapped = std::abs(sample.channel(0)[kPeak]);
  const std::size_t snapped = ingest::snap_to_zero_crossing(sample, kPeak, 240);
  const float after = std::abs(sample.channel(0)[snapped]);

  INFO("value at the boundary: " << unsnapped << " unsnapped, " << after << " snapped");
  CHECK(unsnapped > 0.99F);
  CHECK(after < 0.02F);
}

// --- chop grid ---------------------------------------------------------------

TEST_CASE("chop grid divides a sample evenly", "[unit]") {
  const rt::Sample sample = sine_sample(16'000, 100.0);
  ingest::SnapParams no_snap;
  no_snap.enabled = false;

  const ingest::SliceSet set = ingest::chop_grid(sample, 16, no_snap);
  REQUIRE(set.size() == 16);
  CHECK(set.algorithm == ingest::ChopAlgorithm::kGrid);

  CHECK(set.slices.front().start_frame == 0);
  CHECK(set.slices.back().end_frame == 16'000);
  for (const ingest::Slice& slice : set.slices) {
    CHECK(slice.length() == 1'000);
  }
}

TEST_CASE("chop grid leaves no gaps and no overlaps", "[unit]") {
  // Including when the length does not divide evenly: 1000 frames into 7 parts.
  // Computing each boundary from its index rather than accumulating is what
  // makes the last one land exactly on the end.
  const rt::Sample sample = sine_sample(1'000, 100.0);
  ingest::SnapParams no_snap;
  no_snap.enabled = false;

  const ingest::SliceSet set = ingest::chop_grid(sample, 7, no_snap);
  REQUIRE(set.size() == 7);
  CHECK(set.slices.front().start_frame == 0);
  CHECK(set.slices.back().end_frame == 1'000);
  for (std::size_t index = 1; index < set.size(); ++index) {
    REQUIRE(set.slices[index].start_frame == set.slices[index - 1].end_frame);
  }
}

TEST_CASE("chop grid snaps its boundaries when asked", "[unit]") {
  const rt::Sample sample = sine_sample(16'000, 100.0);
  const ingest::SliceSet snapped = ingest::chop_grid(sample, 16);

  REQUIRE(snapped.size() == 16);
  // Adjacent slices must still share a boundary exactly: the snap runs on the
  // boundary list, not on each slice's ends independently.
  for (std::size_t index = 1; index < snapped.size(); ++index) {
    REQUIRE(snapped.slices[index].start_frame == snapped.slices[index - 1].end_frame);
  }

  // ...and at least one boundary actually moved, or this proves nothing.
  const bool any_moved = std::any_of(snapped.slices.begin(), snapped.slices.end(),
                                     [](const ingest::Slice& s) { return s.start_snap != 0; });
  CHECK(any_moved);
}

TEST_CASE("chop grid refuses nonsense", "[unit]") {
  const rt::Sample sample = sine_sample(1'000, 100.0);
  CHECK(ingest::chop_grid(sample, 0).empty());

  const rt::Sample empty{kRate, 1, 0};
  CHECK(ingest::chop_grid(empty, 8).empty());
}

// --- chop transient ----------------------------------------------------------

TEST_CASE("chop transient makes one slice per hit", "[unit]") {
  constexpr std::size_t kHits = 8;
  constexpr std::size_t kSpacing = kRate / 4;
  const rt::Sample sample = drum_pattern(kHits, kSpacing);

  const ingest::SliceSet set = ingest::chop_transient(sample);
  INFO("sliced into " << set.size());

  CHECK(set.algorithm == ingest::ChopAlgorithm::kTransient);
  CHECK(set.size() == kHits);

  // Each slice starts on its hit.
  for (std::size_t index = 0; index < set.size(); ++index) {
    const auto expected = static_cast<std::ptrdiff_t>(kSpacing * (index + 1));
    const auto actual = static_cast<std::ptrdiff_t>(set.slices[index].start_frame);
    REQUIRE(std::abs(actual - expected) < static_cast<std::ptrdiff_t>(kRate / 40));
  }
}

TEST_CASE("chop transient ignores material before the first hit", "[unit]") {
  // A hardware sampler does not spend a pad on the count-in, and neither does
  // this: slices run from one onset to the next, so leading silence is in none
  // of them.
  const rt::Sample sample = drum_pattern(4, kRate / 4);
  const ingest::SliceSet set = ingest::chop_transient(sample);

  REQUIRE_FALSE(set.empty());
  CHECK(set.slices.front().start_frame > kRate / 8);
}

TEST_CASE("chop transient covers everything from the first hit to the end", "[unit]") {
  const rt::Sample sample = drum_pattern(6, kRate / 5);
  const ingest::SliceSet set = ingest::chop_transient(sample);
  REQUIRE_FALSE(set.empty());

  CHECK(set.slices.back().end_frame == sample.num_frames());
  for (std::size_t index = 1; index < set.size(); ++index) {
    REQUIRE(set.slices[index].start_frame == set.slices[index - 1].end_frame);
  }
}

TEST_CASE("chop transient on material with no transients gives one slice", "[unit]") {
  // A pad, a drone, a field recording. One slice covering everything makes
  // `:chop transient` on unsuitable material behave like a no-op rather than
  // like a failure -- and leaves the pad playable.
  //
  // Faded in over half a second, deliberately. A tone that simply STARTS at
  // frame 0 has a transient there -- the signal appearing out of nothing is a
  // real onset, and the detector is right to find it -- which would exercise
  // the ordinary one-onset path rather than the no-onset path this is for.
  rt::Sample sample = sine_sample(48'000, 220.0, 0.5F);
  const std::span<float> data = sample.mutable_channel(0);
  const std::size_t fade = kRate / 2;
  for (std::size_t frame = 0; frame < fade; ++frame) {
    data[frame] *= static_cast<float>(frame) / static_cast<float>(fade);
  }

  const ingest::SliceSet set = ingest::chop_transient(sample);

  REQUIRE(set.size() == 1);
  CHECK(set.slices[0].start_frame == 0);
  CHECK(set.slices[0].end_frame == sample.num_frames());
}

TEST_CASE("chop transient produces no empty or inverted slices", "[unit]") {
  // Every slice has to be triggerable: VoicePool refuses a slice whose end is
  // not past its start, which would present as a pad that silently does nothing.
  const rt::Sample sample = drum_pattern(12, kRate / 8);
  const ingest::SliceSet set = ingest::chop_transient(sample);

  REQUIRE_FALSE(set.empty());
  for (const ingest::Slice& slice : set.slices) {
    REQUIRE(slice.end_frame > slice.start_frame);
    REQUIRE(slice.end_frame <= sample.num_frames());
  }
}

TEST_CASE("chop transient is deterministic", "[unit]") {
  const rt::Sample sample = drum_pattern(8, kRate / 4);
  const ingest::SliceSet first = ingest::chop_transient(sample);
  const ingest::SliceSet second = ingest::chop_transient(sample);

  REQUIRE(first.size() == second.size());
  for (std::size_t index = 0; index < first.size(); ++index) {
    REQUIRE(first.slices[index].start_frame == second.slices[index].start_frame);
    REQUIRE(first.slices[index].end_frame == second.slices[index].end_frame);
  }
}

TEST_CASE("chop transient records how far the snap moved each boundary", "[unit]") {
  // The EDIT screen shows "start snapped -3 smp", so the number has to survive
  // the chop rather than being recomputed later from a boundary that has
  // already moved.
  const rt::Sample sample = drum_pattern(8, kRate / 4);

  ingest::SnapParams no_snap;
  no_snap.enabled = false;
  const ingest::SliceSet unsnapped = ingest::chop_transient(sample, {}, no_snap);
  const ingest::SliceSet snapped = ingest::chop_transient(sample);

  REQUIRE(unsnapped.size() == snapped.size());
  for (const ingest::Slice& slice : unsnapped.slices) {
    CHECK(slice.start_snap == 0);
  }

  // Each recorded offset must actually reconstruct the unsnapped position.
  for (std::size_t index = 0; index < snapped.size(); ++index) {
    const auto original = static_cast<std::ptrdiff_t>(snapped.slices[index].start_frame) -
                          snapped.slices[index].start_snap;
    REQUIRE(original == static_cast<std::ptrdiff_t>(unsnapped.slices[index].start_frame));
  }
}

TEST_CASE("the default snap radius keeps a low tone's boundaries in place", "[unit]") {
  // What the default radius is actually protecting, and the case the drum
  // material above cannot show: broadband noise has zero crossings everywhere,
  // so any radius finds one within a frame or two. A sustained low tone does
  // not -- its next crossing can be a whole half-period away.
  //
  // At 40 Hz that half-period is 600 frames, 12.5 ms: no longer an inaudible
  // correction but a moved hit. The radius has to give up before then.
  const rt::Sample sample = sine_sample(48'000, 40.0, 0.8F);

  const ingest::SnapParams defaults;
  std::ptrdiff_t worst = 0;
  for (std::size_t at = 100; at < 40'000; at += 137) {
    const std::size_t snapped = ingest::snap_to_zero_crossing(sample, at, defaults.radius);
    worst = std::max(
        worst, std::abs(static_cast<std::ptrdiff_t>(snapped) - static_cast<std::ptrdiff_t>(at)));
  }
  INFO("largest move on a 40 Hz tone: " << worst << " frames, radius " << defaults.radius);

  // 96 frames -- 2 ms -- as a LITERAL, not as defaults.radius. Asserting against
  // the parameter under test would make this pass for any radius at all:
  // measured, a 4000-frame radius moves a boundary 501 frames here and would
  // have sailed through.
  CHECK(worst <= 96);
}

TEST_CASE("snapping does not move a hit far enough to matter", "[unit]") {
  // The snap must not undo the detector's work. Its radius is 1.3 ms and the
  // detector places onsets within a sample or two of the attack, so a snapped
  // boundary is still on the transient.
  const rt::Sample sample = drum_pattern(8, kRate / 4);

  ingest::SnapParams no_snap;
  no_snap.enabled = false;
  const ingest::SliceSet unsnapped = ingest::chop_transient(sample, {}, no_snap);
  const ingest::SliceSet snapped = ingest::chop_transient(sample);
  REQUIRE(unsnapped.size() == snapped.size());

  std::ptrdiff_t worst = 0;
  for (std::size_t index = 0; index < snapped.size(); ++index) {
    worst = std::max(worst, std::abs(snapped.slices[index].start_snap));
  }
  INFO("largest snap: " << worst << " frames");
  CHECK(worst <= 64);
}
