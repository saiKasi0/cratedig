// The peak pyramid: what the waveform is drawn from.
//
// The property that matters is not "the numbers look about right" -- it is that
// a transient the user can hear is a transient the user can see, at every zoom
// level. Most of what follows is that one statement, made checkable.

#include "ingest/peak_pyramid.hpp"

#include "rt/sample.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

using ingest::PeakBin;
using ingest::PeakPyramid;

// A deterministic, non-repeating signal. Not white noise: a smooth carrier with
// a slow envelope, so brute-force expectations stay easy to reason about while
// still varying enough that a channel- or offset-confusion shows up.
[[nodiscard]] float wobble(std::size_t frame, std::uint16_t channel) {
  const auto position = static_cast<double>(frame);
  const double carrier = std::sin(position * 0.00131 + (static_cast<double>(channel) * 1.7));
  const double envelope = 0.5 + (0.5 * std::sin(position * 0.000017));
  return static_cast<float>(carrier * envelope);
}

[[nodiscard]] rt::Sample make_sample(std::size_t frames, std::uint16_t channels) {
  rt::Sample sample{48'000, channels, frames};
  for (std::uint16_t channel = 0; channel < channels; ++channel) {
    const std::span<float> data = sample.mutable_channel(channel);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      data[frame] = wobble(frame, channel);
    }
  }
  return sample;
}

// The truth, computed the slow obvious way. Every expectation below is stated
// against this rather than against another pyramid.
[[nodiscard]] PeakBin brute_force(const rt::Sample& sample, std::uint16_t channel,
                                  std::size_t first, std::size_t last) {
  const std::span<const float> data = sample.channel(channel);
  first = std::min(first, data.size());
  last = std::min(last, data.size());
  if (first >= last) {
    return PeakBin{};
  }
  PeakBin bin{data[first], data[first]};
  for (std::size_t index = first + 1; index < last; ++index) {
    bin.min = std::min(bin.min, data[index]);
    bin.max = std::max(bin.max, data[index]);
  }
  return bin;
}

}  // namespace

TEST_CASE("an empty pyramid summarises to silence rather than crashing", "[unit]") {
  const rt::Sample empty;
  const PeakPyramid pyramid = PeakPyramid::build(empty);

  CHECK(pyramid.empty());
  CHECK(pyramid.num_levels() == 0);
  CHECK(pyramid.bins(0, 0).empty());

  std::vector<PeakBin> out(16, PeakBin{-5.0F, 5.0F});
  pyramid.summarize(empty, 0, 0, 0, out);
  for (const PeakBin& bin : out) {
    CHECK(bin.min == 0.0F);
    CHECK(bin.max == 0.0F);
  }
}

TEST_CASE("a one-frame sample still builds a usable pyramid", "[unit]") {
  rt::Sample sample{48'000, 1, 1};
  sample.mutable_channel(0)[0] = 0.75F;
  const PeakPyramid pyramid = PeakPyramid::build(sample);

  REQUIRE(pyramid.num_levels() == 1);
  REQUIRE(pyramid.num_bins(0) == 1);
  CHECK(pyramid.bins(0, 0)[0].max == 0.75F);

  // Eight columns over one frame: past one frame per column the display holds
  // the frame rather than leaving seven columns empty and drawing the sample at
  // the right-hand edge, which is what plain integer boundaries would do.
  std::vector<PeakBin> out(8);
  pyramid.summarize(sample, 0, 0, 1, out);
  for (std::size_t column = 0; column < out.size(); ++column) {
    INFO("column " << column);
    CHECK(out[column].max == 0.75F);
  }
}

TEST_CASE("zoomed past one frame per column, frames are held not skipped", "[unit]") {
  // Ten frames across sixty columns: every frame must appear somewhere, and the
  // columns must stay in order. A renderer that skipped the collapsed ranges
  // would show a comb of gaps at maximum zoom.
  constexpr std::size_t kFrames = 10;
  rt::Sample sample{48'000, 1, kFrames};
  const std::span<float> data = sample.mutable_channel(0);
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    data[frame] = static_cast<float>(frame + 1) / 10.0F;
  }

  const PeakPyramid pyramid = PeakPyramid::build(sample);
  std::vector<PeakBin> out(60);
  pyramid.summarize(sample, 0, 0, kFrames, out);

  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    const float expected = static_cast<float>(frame + 1) / 10.0F;
    const bool present = std::any_of(
        out.begin(), out.end(), [expected](const PeakBin& bin) { return bin.max == expected; });
    INFO("frame " << frame << " value " << expected);
    CHECK(present);
  }
  // Monotonic ramp in, monotonic ramp out: ordering is preserved.
  CHECK(out.front().max < out.back().max);
}

TEST_CASE("every level is the exact min/max of the frames it covers", "[unit]") {
  constexpr std::size_t kFrames = 200'000;
  const rt::Sample sample = make_sample(kFrames, 2);
  const PeakPyramid pyramid = PeakPyramid::build(sample);

  REQUIRE(pyramid.num_levels() > 3);
  CHECK(pyramid.bin_frames(0) == PeakPyramid::kBaseBinFrames);

  for (std::size_t level = 0; level < pyramid.num_levels(); ++level) {
    // Bin width grows by exactly the ratio, and the top level is a single bin.
    CHECK(pyramid.bin_frames(level) ==
          PeakPyramid::kBaseBinFrames *
              static_cast<std::size_t>(std::pow(PeakPyramid::kLevelRatio, level)));

    for (std::uint16_t channel = 0; channel < 2; ++channel) {
      const std::span<const PeakBin> bins = pyramid.bins(level, channel);
      REQUIRE(bins.size() == pyramid.num_bins(level));

      for (std::size_t bin = 0; bin < bins.size(); ++bin) {
        const std::size_t first = bin * pyramid.bin_frames(level);
        const PeakBin expected =
            brute_force(sample, channel, first, first + pyramid.bin_frames(level));
        // Exact equality, not a tolerance: min/max of a set of floats involves no
        // arithmetic at all, so anything but the identical value is a bug.
        CHECK(bins[bin].min == expected.min);
        CHECK(bins[bin].max == expected.max);
      }
    }
  }
  CHECK(pyramid.num_bins(pyramid.num_levels() - 1) == 1);
}

TEST_CASE("zoomed in past one base bin, summarize is exact", "[unit]") {
  constexpr std::size_t kFrames = 100'000;
  const rt::Sample sample = make_sample(kFrames, 1);
  const PeakPyramid pyramid = PeakPyramid::build(sample);

  // 96 columns over 4 800 frames is 50 frames per column -- well under
  // kBaseBinFrames, so this must take the raw-frame path.
  constexpr std::size_t kColumns = 96;
  constexpr std::size_t kFirst = 12'345;
  constexpr std::size_t kSpan = 4'800;

  std::vector<PeakBin> out(kColumns);
  pyramid.summarize(sample, 0, kFirst, kSpan, out);

  for (std::size_t column = 0; column < kColumns; ++column) {
    const std::size_t first = kFirst + ((column * kSpan) / kColumns);
    const std::size_t last = kFirst + (((column + 1) * kSpan) / kColumns);
    const PeakBin expected = brute_force(sample, 0, first, last);
    CHECK(out[column].min == expected.min);
    CHECK(out[column].max == expected.max);
  }
}

TEST_CASE("at every zoom a column contains the truth and little else", "[unit]") {
  constexpr std::size_t kFrames = 500'000;
  const rt::Sample sample = make_sample(kFrames, 1);
  const PeakPyramid pyramid = PeakPyramid::build(sample);

  constexpr std::size_t kColumns = 100;
  std::vector<PeakBin> out(kColumns);

  // Spans from "the whole file" down to "a few hundred frames", so every level
  // of the pyramid AND the raw path get exercised.
  for (const std::size_t span : {kFrames, kFrames / 3, std::size_t{131'072}, std::size_t{40'000},
                                 std::size_t{9'000}, std::size_t{600}}) {
    pyramid.summarize(sample, 0, 0, span, out);

    const std::size_t column_frames = span / kColumns;
    for (std::size_t column = 0; column < kColumns; ++column) {
      const std::size_t first = (column * span) / kColumns;
      const std::size_t last = ((column + 1) * span) / kColumns;

      const PeakBin truth = brute_force(sample, 0, first, last);
      // Superset: the peak is never hidden. This is the direction that matters.
      CHECK(out[column].min <= truth.min);
      CHECK(out[column].max >= truth.max);

      // ...and bounded on the other side, so "report +/-1 everywhere" would not
      // pass. One column of slop each way is the level-selection guarantee.
      const std::size_t wide_first = first > column_frames ? first - column_frames : 0;
      const PeakBin generous = brute_force(sample, 0, wide_first, last + column_frames);
      CHECK(out[column].min >= generous.min);
      CHECK(out[column].max <= generous.max);
    }
  }
}

TEST_CASE("a single-sample transient survives every zoom level", "[unit]") {
  // THE test. A pyramid built on averages, or one that reads only the bins fully
  // contained by a column, passes everything above and fails here.
  constexpr std::size_t kFrames = 1'000'000;
  constexpr float kSpike = 0.93F;

  // Deliberately awkward positions: the very first frame, exactly on a base-bin
  // boundary, one frame before one, deep in the middle, and the last frame.
  const std::vector<std::size_t> spikes{0, 65'536, 262'143, 517'371, kFrames - 1};

  rt::Sample sample{48'000, 1, kFrames};
  const std::span<float> data = sample.mutable_channel(0);
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    data[frame] = 0.01F * wobble(frame, 0);  // a quiet bed the spike towers over
  }
  for (const std::size_t position : spikes) {
    data[position] = kSpike;
  }

  const PeakPyramid pyramid = PeakPyramid::build(sample);

  for (const std::size_t columns :
       {std::size_t{8}, std::size_t{37}, std::size_t{100}, std::size_t{192}, std::size_t{1'000}}) {
    std::vector<PeakBin> out(columns);
    pyramid.summarize(sample, 0, 0, kFrames, out);

    for (const std::size_t position : spikes) {
      const std::size_t column = (position * columns) / kFrames;
      INFO("columns=" << columns << " spike at frame " << position << " expected in column "
                      << column);
      // Not merely "some column is loud" -- the RIGHT column must be loud, so a
      // renderer cannot pass by smearing the peak across the whole display.
      REQUIRE(column < out.size());
      CHECK(out[column].max >= kSpike);
    }
  }
}

TEST_CASE("channels are summarised independently", "[unit]") {
  // Catches the classic channel-major indexing slip, where channel 1 reads
  // channel 0's bins and stereo files quietly render as mono.
  constexpr std::size_t kFrames = 80'000;
  rt::Sample sample{48'000, 2, kFrames};
  const std::span<float> left = sample.mutable_channel(0);
  const std::span<float> right = sample.mutable_channel(1);
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    left[frame] = 0.5F;
    right[frame] = -0.25F;
  }
  left[40'000] = 0.99F;
  right[40'000] = -0.99F;

  const PeakPyramid pyramid = PeakPyramid::build(sample);

  std::vector<PeakBin> out(32);
  pyramid.summarize(sample, 0, 0, kFrames, out);
  CHECK(out[16].max == 0.99F);
  CHECK(out[0].min == 0.5F);

  pyramid.summarize(sample, 1, 0, kFrames, out);
  CHECK(out[16].min == -0.99F);
  CHECK(out[0].max == -0.25F);
}

TEST_CASE("views past the end of the sample are silent, not wrapped", "[unit]") {
  constexpr std::size_t kFrames = 50'000;
  const rt::Sample sample = make_sample(kFrames, 1);
  const PeakPyramid pyramid = PeakPyramid::build(sample);

  std::vector<PeakBin> out(20);

  SECTION("entirely past the end") {
    pyramid.summarize(sample, 0, kFrames + 1'000, 10'000, out);
    for (const PeakBin& bin : out) {
      CHECK(bin.min == 0.0F);
      CHECK(bin.max == 0.0F);
    }
  }

  SECTION("straddling the end") {
    // Half the view is real audio, half is past the end. The tail must be
    // silence -- wrapping or clamping to the last frame would draw a solid bar
    // where there is no audio at all.
    pyramid.summarize(sample, 0, kFrames - 5'000, 10'000, out);
    CHECK(out[0].max != 0.0F);
    for (std::size_t column = 10; column < out.size(); ++column) {
      INFO("column " << column);
      CHECK(out[column].min == 0.0F);
      CHECK(out[column].max == 0.0F);
    }
  }

  SECTION("an out-of-range channel is silence, not a read out of bounds") {
    pyramid.summarize(sample, 7, 0, kFrames, out);
    for (const PeakBin& bin : out) {
      CHECK(bin.max == 0.0F);
    }
  }
}
