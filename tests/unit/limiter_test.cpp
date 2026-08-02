#include "rt/limiter.hpp"

#include "engine/engine.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "rt/strip.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kBlock = 512;

engine::Engine::Config limiter_engine_config() {
  return engine::Engine::Config{
      .sample_rate = kRate, .num_channels = kChannels, .max_block_frames = 2'048, .seed = 0};
}

// Runs a signal straight through a bare limiter and returns the worst overshoot
// above the ceiling, as a ratio. 1.0 means it exactly reached the ceiling and
// never passed it.
[[nodiscard]] float worst_overshoot(const rt::LimiterConfig& config, std::span<const float> left,
                                    std::span<const float> right) {
  rt::Limiter limiter;
  limiter.prepare(kChannels);

  std::vector<float> a{left.begin(), left.end()};
  std::vector<float> b{right.begin(), right.end()};
  std::array<float*, kChannels> channels{a.data(), b.data()};

  // In several blocks, so the delay line has to survive block boundaries.
  std::size_t done = 0;
  const std::array<std::size_t, 4> sizes{97, 1, 256, 33};
  std::size_t next = 0;
  while (done < a.size()) {
    const std::size_t block = std::min(sizes[next % sizes.size()], a.size() - done);
    ++next;
    std::array<float*, kChannels> view{a.data() + done, b.data() + done};
    limiter.process(config, std::span<float* const>{view}, block);
    done += block;
    static_cast<void>(channels);
  }

  float worst = 0.0F;
  for (std::size_t frame = 0; frame < a.size(); ++frame) {
    worst = std::max(worst, std::max(std::abs(a[frame]), std::abs(b[frame])));
  }
  return worst / config.ceiling_linear;
}

std::shared_ptr<const rt::Sample> hot_sample() {
  auto sample = std::make_shared<rt::Sample>(kRate, kChannels, std::size_t{8'000});
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < data.size(); ++frame) {
      const auto mixed =
          static_cast<float>(((frame * 31) + (static_cast<std::size_t>(channel) * 733)) % 4'001);
      data[frame] = ((mixed / 2'000.5F) - 1.0F) * 0.98F;
    }
  }
  return sample;
}

std::vector<float> render_master(bool engage_limiter, float ceiling_db) {
  engine::Engine eng{limiter_engine_config()};
  REQUIRE(eng.set_pad_sample(0, hot_sample()));
  REQUIRE(eng.set_pad_sample(1, hot_sample()));
  if (engage_limiter) {
    REQUIRE(eng.set_limiter(rt::make_limiter(ceiling_db)));
  }
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 1, .velocity = 1.0F}));

  std::array<float, kBlock * kChannels> scratch{};
  scratch.fill(-1.5F);
  std::array<float*, kChannels> channels{};
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    channels[channel] = scratch.data() + (static_cast<std::size_t>(channel) * kBlock);
  }
  eng.render(std::span<float* const>{channels}, kBlock);
  return std::vector<float>{scratch.begin(), scratch.end()};
}

[[nodiscard]] bool identical(const std::vector<float>& left, const std::vector<float>& right) {
  return left.size() == right.size() &&
         std::memcmp(left.data(), right.data(), left.size() * sizeof(float)) == 0;
}

[[nodiscard]] float peak_of(const std::vector<float>& values) {
  float peak = 0.0F;
  for (const float value : values) {
    peak = std::max(peak, std::abs(value));
  }
  return peak;
}

// The residual the guarantee is allowed, and it is measured rather than
// budgeted. The gain is ceiling/peak and the output is gain*sample; both
// roundings are relative, so the algebraic bound |g*x| <= ceiling can be missed
// only in the last bit.
//
// Tightened to exactly 1.0 to find out: the worst overshoot across every case
// below is 1.000000119, which is ONE ULP at unity (2^-23 = 1.19e-7), and it
// appears only where the input is far above the ceiling. So this tolerance is
// about eight times the observed residual -- loose enough not to be brittle,
// far too tight to hide a real failure, which overshoots by percent rather than
// by parts per million.
constexpr float kOvershootTolerance = 1.0F + 1.0e-6F;

}  // namespace

TEST_CASE("the limiter is off by default and changes nothing", "[unit]") {
  // Load-bearing: the lookahead is a DELAY, and a delay that was always on would
  // shift every sample in the project by 64 frames and move every committed
  // hash.
  const rt::LimiterConfig fresh;
  CHECK_FALSE(fresh.enabled);
  CHECK(fresh.ceiling_db == rt::kDefaultCeilingDb);
  CHECK(fresh.lookahead_frames == rt::kDefaultLookaheadFrames);

  const std::vector<float> plain = render_master(false, rt::kDefaultCeilingDb);
  CHECK(peak_of(plain) > 0.1F);
}

TEST_CASE("no sample exceeds the ceiling, for any input", "[unit]") {
  // THE ACCEPTANCE: no output sample exceeds the ceiling, for any input.
  //
  // Five signals rather than one, because "any input" is the claim: a full-scale
  // step, a lone impulse in silence, material 32 dB above full scale, a burst
  // train that puts the release to work between hits, and noise that crosses the
  // ceiling constantly. Each is run in ragged blocks, so the delay line has to
  // carry state across block boundaries to pass.
  //
  // What carries the guarantee is the instant attack and the clamp on the
  // released gain -- NOT the lookahead. See the timing test below.
  for (const float ceiling_db : {-0.3F, -6.0F, -12.0F, -24.0F}) {
    const rt::LimiterConfig config = rt::make_limiter(ceiling_db);

    SECTION("a full-scale step") {
      std::vector<float> left(4'096, 0.0F);
      std::vector<float> right(4'096, 0.0F);
      for (std::size_t frame = 1'000; frame < 3'000; ++frame) {
        left[frame] = 1.0F;
        right[frame] = -1.0F;
      }
      INFO("ceiling " << ceiling_db << " dB, full-scale step");
      CHECK(worst_overshoot(config, left, right) <= kOvershootTolerance);
    }

    SECTION("a single full-scale impulse in silence") {
      std::vector<float> left(4'096, 0.0F);
      std::vector<float> right(4'096, 0.0F);
      left[2'000] = 1.0F;
      right[2'000] = 1.0F;
      INFO("ceiling " << ceiling_db << " dB, impulse");
      CHECK(worst_overshoot(config, left, right) <= kOvershootTolerance);
    }

    SECTION("far above full scale") {
      // Nothing in the mixer produces this, but the limiter is the last thing
      // between the graph and the converter and must survive being handed it.
      std::vector<float> left(4'096, 0.0F);
      std::vector<float> right(4'096, 0.0F);
      for (std::size_t frame = 0; frame < left.size(); ++frame) {
        left[frame] = 40.0F * static_cast<float>(std::sin(0.01 * static_cast<double>(frame)));
        right[frame] = -40.0F * static_cast<float>(std::cos(0.003 * static_cast<double>(frame)));
      }
      INFO("ceiling " << ceiling_db << " dB, +32 dBFS input");
      CHECK(worst_overshoot(config, left, right) <= kOvershootTolerance);
    }

    SECTION("a burst train, so the release is exercised between hits") {
      std::vector<float> left(20'000, 0.0F);
      std::vector<float> right(20'000, 0.0F);
      for (std::size_t frame = 0; frame < left.size(); ++frame) {
        const bool inside = (frame % 2'000) < 200;
        const auto phase = static_cast<float>(std::sin(0.2 * static_cast<double>(frame)));
        left[frame] = inside ? phase : (0.02F * phase);
        right[frame] = inside ? (0.8F * phase) : (0.02F * phase);
      }
      INFO("ceiling " << ceiling_db << " dB, burst train");
      CHECK(worst_overshoot(config, left, right) <= kOvershootTolerance);
    }

    SECTION("noise that crosses the ceiling constantly") {
      std::vector<float> left(20'000, 0.0F);
      std::vector<float> right(20'000, 0.0F);
      std::uint32_t state = 0x1234'5678U;
      for (std::size_t frame = 0; frame < left.size(); ++frame) {
        // A plain LCG: deterministic, and no dependence on a library's RNG.
        state = (state * 1'664'525U) + 1'013'904'223U;
        left[frame] = ((static_cast<float>(state >> 8U) / 8'388'608.0F) - 1.0F) * 1.4F;
        state = (state * 1'664'525U) + 1'013'904'223U;
        right[frame] = ((static_cast<float>(state >> 8U) / 8'388'608.0F) - 1.0F) * 1.4F;
      }
      INFO("ceiling " << ceiling_db << " dB, noise");
      CHECK(worst_overshoot(config, left, right) <= kOvershootTolerance);
    }
  }
}

TEST_CASE("gain reduction starts before the transient, not on it", "[unit]") {
  // WHAT LOOKAHEAD IS ACTUALLY FOR, and it is not the ceiling guarantee. With
  // instant attack on a detector that includes the current sample, the bound
  // holds at zero lookahead too -- deleting the lookahead and re-running the
  // acceptance above passes it, which is how the earlier claim to the contrary
  // was caught.
  //
  // What lookahead moves is WHERE THE GAIN STEP LANDS. Reduction is
  // instantaneous, so at L = 0 the step falls exactly on the transient that
  // caused it: a discontinuity applied to a loud sample, which is a step in the
  // waveform. At L > 0 the same step is applied L frames earlier, to the quiet
  // material in front of the peak.
  //
  // Measured as: how many frames before the peak does the output stop being the
  // input? That distance IS the lookahead.
  constexpr std::size_t kPeakAt = 500;

  for (const std::size_t lookahead :
       {std::size_t{0}, std::size_t{16}, std::size_t{64}, std::size_t{200}}) {
    const rt::LimiterConfig config = rt::make_limiter(-6.0F, 4'800, lookahead);

    rt::Limiter limiter;
    limiter.prepare(kChannels);

    // Quiet, well under the ceiling, with one loud sample.
    std::vector<float> left(2'048, 0.01F);
    std::vector<float> right(2'048, 0.01F);
    left[kPeakAt] = 1.0F;
    right[kPeakAt] = 1.0F;
    const std::vector<float> input = left;

    std::array<float*, kChannels> channels{left.data(), right.data()};
    limiter.process(config, std::span<float* const>{channels}, left.size());

    // The first frame whose output is not simply the delayed input at unity.
    std::size_t first_reduced = left.size();
    for (std::size_t frame = 0; frame < left.size(); ++frame) {
      const std::size_t source = frame < lookahead ? 0 : frame - lookahead;
      const float expected = frame < lookahead ? 0.0F : input[source];
      if (left[frame] != expected) {
        first_reduced = frame;
        break;
      }
    }

    // The peak leaves the delay line at kPeakAt + lookahead. Reduction must have
    // begun exactly `lookahead` frames before that -- which is to say, the
    // moment the peak ENTERED the window.
    INFO("lookahead " << lookahead << ", first reduced frame " << first_reduced);
    CHECK(first_reduced == kPeakAt);
    CHECK(kPeakAt + lookahead - first_reduced == lookahead);
  }
}

TEST_CASE("the limiter delays by exactly its lookahead", "[unit]") {
  // Which is the thing that makes the bound possible AND the reason it is off by
  // default, so it is worth pinning rather than assuming.
  for (const std::size_t lookahead :
       {std::size_t{0}, std::size_t{1}, std::size_t{64}, rt::kMaxLookaheadFrames}) {
    // A ceiling of 0 dB with quiet input: no reduction, so the only thing the
    // limiter does is delay.
    const rt::LimiterConfig config = rt::make_limiter(0.0F, 4'800, lookahead);

    rt::Limiter limiter;
    limiter.prepare(kChannels);

    std::vector<float> left(1'024, 0.0F);
    std::vector<float> right(1'024, 0.0F);
    left[10] = 0.5F;
    right[10] = 0.25F;

    std::array<float*, kChannels> channels{left.data(), right.data()};
    limiter.process(config, std::span<float* const>{channels}, left.size());

    INFO("lookahead " << lookahead);
    CHECK(left[10 + lookahead] == 0.5F);
    CHECK(right[10 + lookahead] == 0.25F);
    if (lookahead > 0) {
      CHECK(left[10] == 0.0F);
    }
  }
}

TEST_CASE("a disengaged limiter is bit-exact, and engaging it reduces the peak", "[unit]") {
  const std::vector<float> plain = render_master(false, -0.3F);
  CHECK(peak_of(plain) > 0.5F);

  // Two disengaged renders agree bit for bit, and nothing about publishing a
  // disabled config changes that.
  CHECK(identical(plain, render_master(false, -0.3F)));

  // Engaged at a low ceiling, the master peak comes down below it.
  const std::vector<float> limited = render_master(true, -12.0F);
  const float ceiling = rt::make_limiter(-12.0F).ceiling_linear;
  CHECK(peak_of(limited) <= ceiling * kOvershootTolerance);
  CHECK(peak_of(limited) < peak_of(plain));
}

TEST_CASE("the limiter gain is reported, and reads unity when disengaged", "[unit]") {
  engine::Engine idle{limiter_engine_config()};
  CHECK(idle.telemetry().limiter_gain == 1.0F);

  engine::Engine eng{limiter_engine_config()};
  REQUIRE(eng.set_pad_sample(0, hot_sample()));
  REQUIRE(eng.set_limiter(rt::make_limiter(-24.0F)));
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));

  std::array<float, kBlock * kChannels> scratch{};
  std::array<float*, kChannels> channels{};
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    channels[channel] = scratch.data() + (static_cast<std::size_t>(channel) * kBlock);
  }
  eng.render(std::span<float* const>{channels}, kBlock);
  CHECK(eng.telemetry().limiter_gain < 1.0F);

  // And a disengaged limiter reports unity rather than whatever gain it happened
  // to be holding, because "not limiting" is the truth about the signal.
  REQUIRE(eng.set_limiter(rt::LimiterConfig{}));
  eng.render(std::span<float* const>{channels}, kBlock);
  CHECK(eng.telemetry().limiter_gain == 1.0F);
}

TEST_CASE("limiter settings that crossed a thread boundary are clamped", "[unit]") {
  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

  CHECK(rt::make_limiter(kNan).ceiling_db == rt::kDefaultCeilingDb);
  CHECK(rt::make_limiter(12.0F).ceiling_db == rt::kMaxCeilingDb);
  CHECK(rt::make_limiter(-99.0F).ceiling_db == rt::kMinCeilingDb);

  // A lookahead longer than the delay line is clamped to it rather than reading
  // off the end -- the buffer is allocated once, on the control thread.
  CHECK(rt::make_limiter(-0.3F, 4'800, 100'000).lookahead_frames == rt::kMaxLookaheadFrames);
}

TEST_CASE("an engaged limiter is invariant to block size", "[unit]") {
  constexpr std::size_t kTotal = 2'048;
  auto render_in = [](std::span<const std::size_t> sizes) {
    engine::Engine eng{limiter_engine_config()};
    REQUIRE(eng.set_pad_sample(0, hot_sample()));
    REQUIRE(eng.set_pad_sample(1, hot_sample()));
    REQUIRE(eng.set_limiter(rt::make_limiter(-9.0F, 2'400, 64)));
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 1, .velocity = 0.8F}));

    std::vector<float> out(kTotal * kChannels, 0.0F);
    std::array<float, std::size_t{2'048} * kChannels> scratch{};
    std::size_t done = 0;
    std::size_t next = 0;
    while (done < kTotal) {
      const std::size_t block = std::min(sizes[next % sizes.size()], kTotal - done);
      ++next;
      scratch.fill(-5.5F);
      std::array<float*, kChannels> channels{};
      for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
        channels[channel] = scratch.data() + (static_cast<std::size_t>(channel) * 2'048);
      }
      eng.render(std::span<float* const>{channels}, block);
      for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
        std::copy_n(channels[channel], block,
                    out.data() + (static_cast<std::size_t>(channel) * kTotal) + done);
      }
      done += block;
    }
    return out;
  };

  const std::array<std::size_t, 1> one{kTotal};
  const std::array<std::size_t, 1> small{64};
  const std::array<std::size_t, 4> ragged{100, 37, 512, 1};

  const std::vector<float> single = render_in(one);
  CHECK(peak_of(single) > 0.1F);
  CHECK(identical(single, render_in(small)));
  CHECK(identical(single, render_in(ragged)));
}
