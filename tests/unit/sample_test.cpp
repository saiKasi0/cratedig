#include "rt/sample.hpp"

#include "rt/interpolator.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

#include <catch2/catch_test_macros.hpp>

namespace {

// Fills every channel with a recognisable ramp so an off-by-one in the channel
// stride shows up as wrong values rather than as plausible ones.
rt::Sample make_ramp(std::uint16_t channels, std::size_t frames, std::uint32_t rate = 48'000) {
  rt::Sample sample{rate, channels, frames};
  for (std::uint16_t channel = 0; channel < channels; ++channel) {
    std::span<float> data = sample.mutable_channel(channel);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      data[frame] = static_cast<float>((static_cast<std::size_t>(channel) * 1'000) + frame + 1);
    }
  }
  return sample;
}

}  // namespace

TEST_CASE("Sample reports its own shape", "[unit]") {
  const rt::Sample sample = make_ramp(2, 128, 44'100);

  CHECK(sample.sample_rate() == 44'100);
  CHECK(sample.num_channels() == 2);
  CHECK(sample.num_frames() == 128);
  CHECK_FALSE(sample.empty());

  const rt::Sample nothing{};
  CHECK(nothing.empty());
  CHECK(nothing.num_frames() == 0);
}

TEST_CASE("Sample keeps channels separate", "[unit]") {
  const rt::Sample sample = make_ramp(3, 64);

  for (std::uint16_t channel = 0; channel < 3; ++channel) {
    const std::span<const float> data = sample.channel(channel);
    REQUIRE(data.size() == 64);
    CHECK(data[0] == static_cast<float>((channel * 1'000) + 1));
    CHECK(data[63] == static_cast<float>((channel * 1'000) + 64));
  }
}

TEST_CASE("Sample guard frames are readable and silent", "[unit]") {
  // The whole reason the guards exist: the interpolator reads x[i-1] .. x[i+2],
  // so at i == 0 and i == num_frames - 1 it reads outside the audible range.
  // Padding makes that defined; zeroing it makes the sample fade to silence at
  // its edges rather than holding its first/last value, which would click.
  const rt::Sample sample = make_ramp(2, 32);

  for (std::uint16_t channel = 0; channel < 2; ++channel) {
    const float* frame0 = sample.frame0(channel);

    for (std::size_t k = 1; k <= rt::Sample::kGuardBefore; ++k) {
      CHECK(frame0[-static_cast<std::ptrdiff_t>(k)] == 0.0F);
    }
    for (std::size_t k = 0; k < rt::Sample::kGuardAfter; ++k) {
      CHECK(frame0[static_cast<std::ptrdiff_t>(32 + k)] == 0.0F);
    }

    CHECK(frame0[0] == static_cast<float>((channel * 1'000) + 1));
    CHECK(frame0[31] == static_cast<float>((channel * 1'000) + 32));
  }
}

TEST_CASE("Sample guards cover exactly what the interpolator reads", "[unit]") {
  // Reading the first and last frame through the real kernel is the test that
  // matters; under asan an under-sized guard is a heap-buffer-overflow here
  // rather than a subtle wrong value.
  STATIC_CHECK(rt::Sample::kGuardBefore >= rt::kHermiteTapsBefore);
  STATIC_CHECK(rt::Sample::kGuardAfter >= rt::kHermiteTapsAfter);

  const rt::Sample sample = make_ramp(1, 16);
  const float* frame0 = sample.frame0(0);

  const float at_start = rt::hermite4(frame0[-1], frame0[0], frame0[1], frame0[2], 0.5F);
  const float at_end = rt::hermite4(frame0[14], frame0[15], frame0[16], frame0[17], 0.5F);

  CHECK(at_start != 0.0F);
  CHECK(at_end != 0.0F);
}

TEST_CASE("Sample is movable and leaves the source empty", "[unit]") {
  rt::Sample source = make_ramp(2, 16);
  const rt::Sample moved = std::move(source);

  CHECK(moved.num_frames() == 16);
  CHECK(moved.num_channels() == 2);
  CHECK(moved.channel(1)[0] == 1'001.0F);
}

TEST_CASE("Sample storage is zeroed before the worker writes it", "[unit]") {
  // A decoder that returns fewer frames than it promised must leave silence
  // behind, not whatever the allocator handed back.
  const rt::Sample sample{48'000, 2, 100};

  for (std::uint16_t channel = 0; channel < 2; ++channel) {
    for (const float value : sample.channel(channel)) {
      CHECK(value == 0.0F);
    }
  }
}
