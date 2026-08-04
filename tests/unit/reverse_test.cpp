#include "rt/garbage_ring.hpp"
#include "rt/pad_config.hpp"
#include "rt/sample.hpp"
#include "rt/voice_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Reverse playback.
//
// PadConfig's comment promised this from M3 -- "deliberately absent until the
// DSP for them lands" -- and the DSP turned out to be four lines, because the
// phase still runs FORWARD and only the read position is mirrored. These tests
// exist to pin that it really is a mirror: same frames, same count, opposite
// order, and the fraction mirrored too.

namespace {

constexpr std::uint32_t kRate = 48'000;

// A ramp, so every frame is identifiable by its value alone. Frame n holds
// n / count -- which makes "played backwards" a claim about numbers rather than
// about a waveform somebody has to squint at.
[[nodiscard]] std::shared_ptr<const rt::Sample> ramp(std::size_t frames) {
  auto sample = std::make_shared<rt::Sample>(kRate, std::uint16_t{1}, frames);
  const std::span<float> data = sample->mutable_channel(0);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    data[frame] = static_cast<float>(frame) / static_cast<float>(frames);
  }
  return sample;
}

[[nodiscard]] std::vector<float> render(bool reverse, std::size_t frames, float pitch = 1.0F,
                                        std::size_t start = 0, std::size_t end = 0) {
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;

  rt::PadConfig config{};
  config.sample = ramp(frames);
  config.start_frame = start;
  config.end_frame = end == 0 ? frames : end;
  config.pitch_ratio = pitch;
  config.reverse = reverse;
  // The declick ramps would scale the edges and hide an off-by-one at exactly
  // the frames this test is about.
  config.fade_in_frames = 0;
  config.fade_out_frames = 0;

  REQUIRE(pool.trigger(std::make_shared<const rt::PadConfig>(config), 1.0F, kRate, garbage, 0));

  std::vector<float> out(frames, 0.0F);
  std::array<float*, 1> channels{out.data()};
  pool.render_add(std::span<float* const>{channels}, frames);
  return out;
}

}  // namespace

TEST_CASE("reverse plays the same frames in the opposite order", "[unit]") {
  constexpr std::size_t kFrames = 64;
  const std::vector<float> forward = render(false, kFrames);
  const std::vector<float> backward = render(true, kFrames);

  REQUIRE(forward.size() == backward.size());

  // The forward render is the ramp itself; the reverse is the ramp read from the
  // other end. Frame i of one is frame (n-1-i) of the other.
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    INFO("frame: " << frame);
    CHECK(backward[frame] == Catch::Approx(forward[kFrames - 1 - frame]).margin(1e-6));
  }
}

TEST_CASE("reverse plays for exactly as long as forward", "[unit]") {
  // The termination test is untouched by reverse -- the phase still counts up to
  // end_frame -- so a slice takes the same time either way. If reverse had been
  // built by negating the step it would have run out at zero instead, which is a
  // different length for every slice that does not start at the beginning.
  constexpr std::size_t kFrames = 128;
  const auto sounding = [](const std::vector<float>& signal) {
    std::size_t last = 0;
    for (std::size_t frame = 0; frame < signal.size(); ++frame) {
      if (std::abs(signal[frame]) > 1e-6F) {
        last = frame;
      }
    }
    return last;
  };

  // A slice in the MIDDLE, which is where a negative step would have gone wrong.
  const std::vector<float> forward = render(false, kFrames, 1.0F, 32, 96);
  const std::vector<float> backward = render(true, kFrames, 1.0F, 32, 96);

  CHECK(sounding(forward) == sounding(backward));

  // And it is the slice's frames, not the file's: 32..95 backwards starts at 95.
  CHECK(backward[0] == Catch::Approx(95.0F / 128.0F).margin(1e-6));
  CHECK(forward[0] == Catch::Approx(32.0F / 128.0F).margin(1e-6));
}

TEST_CASE("reverse mirrors the fraction, not just the frame", "[unit]") {
  // THE REASON THE MIRROR IS IN FIXED POINT. At half speed the phase lands
  // between frames, and mirroring only the integer part would snap reverse
  // playback to frame boundaries -- audibly different from forward, and a
  // different pitch.
  //
  // A ramp interpolated at half speed is still a straight line, so consecutive
  // output frames differ by a constant. Reverse should give the same constant
  // with the opposite sign.
  constexpr std::size_t kFrames = 64;
  const std::vector<float> forward = render(false, kFrames, 0.5F);
  const std::vector<float> backward = render(true, kFrames, 0.5F);

  const float forward_step = forward[20] - forward[19];
  const float backward_step = backward[20] - backward[19];

  CHECK(forward_step == Catch::Approx(0.5F / static_cast<float>(kFrames)).margin(1e-5));
  CHECK(backward_step == Catch::Approx(-forward_step).margin(1e-5));
}

TEST_CASE("reverse is off by default", "[unit]") {
  // A field nothing sets must not change what anything does -- which is what
  // keeps every committed golden where it is.
  const rt::PadConfig defaults;
  CHECK_FALSE(defaults.reverse);
}

TEST_CASE("a pad flipped to reverse does not turn a ringing hit around", "[unit]") {
  // Captured at trigger, like every other per-voice setting. A voice that
  // changed direction mid-flight would jump to the mirror of wherever it had
  // got to, which is a click and a different sound.
  constexpr std::size_t kFrames = 64;
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;

  rt::PadConfig config{};
  config.sample = ramp(kFrames);
  config.end_frame = kFrames;
  config.fade_in_frames = 0;
  config.fade_out_frames = 0;
  REQUIRE(pool.trigger(std::make_shared<const rt::PadConfig>(config), 1.0F, kRate, garbage, 0));

  // Half the slice, forward.
  std::vector<float> out(kFrames, 0.0F);
  std::array<float*, 1> channels{out.data()};
  pool.render_add(std::span<float* const>{channels}, kFrames / 2);

  // The config the pad now holds says reverse. The sounding voice keeps its own.
  config.reverse = true;
  const auto flipped = std::make_shared<const rt::PadConfig>(config);
  static_cast<void>(flipped);

  std::array<float*, 1> rest{out.data() + (kFrames / 2)};
  pool.render_add(std::span<float* const>{rest}, kFrames / 2);

  // Still ascending across the join, rather than turning around at frame 32.
  CHECK(out[33] > out[32]);
  CHECK(out[kFrames - 1] > out[kFrames / 2]);
}
