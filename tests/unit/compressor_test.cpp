#include "rt/compressor.hpp"

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

engine::Engine::Config compressor_engine_config() {
  return engine::Engine::Config{
      .sample_rate = kRate, .num_channels = kChannels, .max_block_frames = 2'048, .seed = 0};
}

// The curve from docs/MIXER.md, written out independently in double.
//
// A second implementation rather than a call into the one under test, which
// would only prove the function equals itself. Both come from the spec; if they
// disagree, one of the two transcriptions is wrong and the test says so.
[[nodiscard]] double reference_curve_db(double input_db, double threshold_db, double ratio,
                                        double knee_db) {
  const double over = input_db - threshold_db;
  if (knee_db > 0.0 && std::abs(over) <= knee_db / 2.0) {
    const double into = over + (knee_db / 2.0);
    return input_db + (((1.0 / ratio) - 1.0) * into * into / (2.0 * knee_db));
  }
  if (over > 0.0) {
    return threshold_db + (over / ratio);
  }
  return input_db;
}

// Numerical slope of the curve at a point, for the derivative-continuity check.
[[nodiscard]] double slope_at(double input_db, double threshold_db, double ratio, double knee_db) {
  constexpr double kStep = 1.0e-5;
  return (reference_curve_db(input_db + kStep, threshold_db, ratio, knee_db) -
          reference_curve_db(input_db - kStep, threshold_db, ratio, knee_db)) /
         (2.0 * kStep);
}

std::shared_ptr<const rt::Sample> loud_sample() {
  auto sample = std::make_shared<rt::Sample>(kRate, kChannels, std::size_t{8'000});
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < data.size(); ++frame) {
      const auto mixed = static_cast<float>(((frame * 47) + (channel * 409)) % 5'003);
      data[frame] = ((mixed / 2'501.5F) - 1.0F) * 0.9F;
    }
  }
  return sample;
}

std::vector<float> render_with(const rt::StripConfig& strip) {
  engine::Engine eng{compressor_engine_config()};
  REQUIRE(eng.set_pad_sample(0, loud_sample()));
  REQUIRE(eng.set_strip(0, strip));
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));

  std::array<float, kBlock * kChannels> scratch{};
  scratch.fill(-2.75F);
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

}  // namespace

TEST_CASE("the compressor curve matches the specified curve", "[unit]") {
  // THE ACCEPTANCE. Steady-state gain reduction against the analytic curve,
  // across threshold and ratio, measured on the implementation rather than on a
  // restatement of it.
  for (const float threshold : {-60.0F, -40.0F, -24.0F, -12.0F, -3.0F, 0.0F}) {
    for (const float ratio : {1.0F, 1.5F, 2.0F, 4.0F, 8.0F, 20.0F}) {
      for (const float knee : {0.0F, 3.0F, 12.0F, 24.0F}) {
        const rt::CompressorConfig config = rt::make_compressor(threshold, ratio, knee, 0.0F, 0, 0);

        for (double input_db = -90.0; input_db <= 6.0; input_db += 0.5) {
          const double expected =
              reference_curve_db(input_db, static_cast<double>(threshold),
                                 static_cast<double>(ratio), static_cast<double>(knee));
          // Gain reduction is what the compressor applies; convert it back to an
          // output level so the comparison is against the curve itself.
          const auto envelope = static_cast<float>(std::pow(10.0, input_db / 20.0));
          const float gain = rt::Compressor::gain_for(config, envelope);
          const double actual = input_db + (20.0 * std::log10(static_cast<double>(gain)));

          INFO("T = " << threshold << ", R = " << ratio << ", W = " << knee << ", input "
                      << input_db << " dB");
          CHECK(std::abs(actual - expected) < 0.01);
        }
      }
    }
  }
}

TEST_CASE("the knee is continuous in value and in slope", "[unit]") {
  // A knee that is merely continuous still produces an audible edge, and the
  // edge lives in the DERIVATIVE -- so both are checked. The slope must also
  // interpolate 1 -> 1/R across the knee rather than jumping at either end.
  for (const double threshold : {-40.0, -20.0, -6.0}) {
    for (const double ratio : {1.5, 2.0, 4.0, 20.0}) {
      for (const double knee : {1.0, 6.0, 12.0, 24.0}) {
        constexpr double kStep = 1.0e-5;
        for (const double edge : {threshold - (knee / 2.0), threshold + (knee / 2.0)}) {
          const double below = reference_curve_db(edge - kStep, threshold, ratio, knee);
          const double above = reference_curve_db(edge + kStep, threshold, ratio, knee);
          INFO("T = " << threshold << ", R = " << ratio << ", W = " << knee << ", edge " << edge);
          CHECK(std::abs(above - below) < 1.0e-4);

          const double slope_below = slope_at(edge - (10.0 * kStep), threshold, ratio, knee);
          const double slope_above = slope_at(edge + (10.0 * kStep), threshold, ratio, knee);
          CHECK(std::abs(slope_above - slope_below) < 1.0e-3);
        }

        // 1 at the bottom of the knee, 1/R at the top, linear between.
        CHECK(std::abs(slope_at(threshold - (knee / 2.0) + 1.0e-4, threshold, ratio, knee) - 1.0) <
              1.0e-3);
        CHECK(std::abs(slope_at(threshold + (knee / 2.0) - 1.0e-4, threshold, ratio, knee) -
                       (1.0 / ratio)) < 1.0e-3);
        const double middle = slope_at(threshold, threshold, ratio, knee);
        CHECK(std::abs(middle - ((1.0 + (1.0 / ratio)) / 2.0)) < 1.0e-3);
      }
    }
  }

  // And the implementation agrees with that reference at the boundaries, which
  // is where a transcription error would hide.
  for (const float knee : {1.0F, 6.0F, 24.0F}) {
    for (const float offset : {-0.5F * knee, 0.0F, 0.5F * knee}) {
      const float input_db = -20.0F + offset;
      const double expected =
          reference_curve_db(static_cast<double>(input_db), -20.0, 4.0, static_cast<double>(knee));
      INFO("knee " << knee << ", input " << input_db << " dB");
      CHECK(std::abs(static_cast<double>(rt::compressor_curve_db(input_db, -20.0F, 4.0F, knee)) -
                     expected) < 1.0e-3);
    }
  }
}

TEST_CASE("the envelope reaches 1 - 1/e after exactly the time constant", "[unit]") {
  // What "attack time" MEANS here, per docs/MIXER.md: not "time to full gain
  // reduction", a definition that varies between manufacturers and cannot be
  // tested against.
  constexpr double kTarget = 1.0 - (1.0 / 2.718'281'828'459'045);

  for (const std::size_t frames : {1U, 10U, 48U, 240U, 4'800U}) {
    const rt::CompressorConfig config = rt::make_compressor(0.0F, 1.0F, 0.0F, 0.0F, frames, frames);

    // Attack: a step from silence to full scale.
    rt::Compressor attacking;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      static_cast<void>(attacking.process(config, 1.0F));
    }
    INFO("attack over " << frames << " frames");
    CHECK(std::abs(static_cast<double>(attacking.envelope()) - kTarget) < 0.001);

    // Release: a step back to silence, which should FALL by the same fraction.
    //
    // Measured against the level the envelope actually reached, not against 1.0.
    // A one-pole in float32 cannot converge all the way to its target: the
    // iteration stalls once the per-step change falls below half an ULP, at a
    // residual of roughly eps/(1 - coeff), which grows with the time constant.
    // Measured: 3.0e-7 at tau = 10, 1.4e-6 at 48, 1.4e-4 at 4800, 1.4e-3 at
    // 48000 -- tracking the prediction across five orders of magnitude. That is
    // arithmetic, not a defect, and a test that demanded 1e-5 of a 4800-frame
    // attack was asserting something float cannot do.
    rt::Compressor releasing;
    for (std::size_t frame = 0; frame < 200'000; ++frame) {
      static_cast<void>(releasing.process(config, 1.0F));
    }
    const double from = static_cast<double>(releasing.envelope());
    REQUIRE(from > 0.99);

    for (std::size_t frame = 0; frame < frames; ++frame) {
      static_cast<void>(releasing.process(config, 0.0F));
    }
    INFO("release over " << frames << " frames, from " << from);
    CHECK(std::abs((static_cast<double>(releasing.envelope()) / from) - (1.0 - kTarget)) < 0.001);
  }

  // A zero time constant is instant, and gets there without a NaN: -1/0 is
  // -infinity and exp of that is 0.
  const rt::CompressorConfig instant = rt::make_compressor(0.0F, 1.0F, 0.0F, 0.0F, 0, 0);
  CHECK(instant.attack_coeff == 0.0F);
  rt::Compressor snappy;
  CHECK(snappy.process(instant, 0.5F) == rt::Compressor::gain_for(instant, 0.5F));
  CHECK(snappy.envelope() == 0.5F);
}

TEST_CASE("the attack is faster than the release, and both are used", "[unit]") {
  // Asymmetry is the whole point of a one-pole detector; a bug that used one
  // coefficient for both would still settle to the right place and would still
  // pass a curve test.
  const rt::CompressorConfig config = rt::make_compressor(-20.0F, 4.0F, 6.0F, 0.0F, 10, 4'800);
  CHECK(config.attack_coeff < config.release_coeff);

  rt::Compressor compressor;
  for (std::size_t frame = 0; frame < 10; ++frame) {
    static_cast<void>(compressor.process(config, 1.0F));
  }
  const float after_attack = compressor.envelope();
  CHECK(after_attack > 0.6F);  // fast attack: most of the way in ten frames

  for (std::size_t frame = 0; frame < 10; ++frame) {
    static_cast<void>(compressor.process(config, 0.0F));
  }
  // Slow release: barely moved over the same ten frames.
  CHECK(compressor.envelope() > 0.99F * after_attack);
}

TEST_CASE("ratio 1 is unity at every level, for every knee", "[unit]") {
  // Which is what makes the enabled flag an optimisation rather than a second
  // code path: bypassed and R = 1 must be the same arithmetic.
  for (const float knee : {0.0F, 6.0F, 24.0F}) {
    for (const float threshold : {-60.0F, -20.0F, 0.0F}) {
      const rt::CompressorConfig config = rt::make_compressor(threshold, 1.0F, knee, 0.0F, 48, 480);
      for (double input_db = -90.0; input_db <= 6.0; input_db += 0.25) {
        const auto envelope = static_cast<float>(std::pow(10.0, input_db / 20.0));
        INFO("T = " << threshold << ", W = " << knee << ", input " << input_db << " dB");
        CHECK(rt::Compressor::gain_for(config, envelope) == 1.0F);
      }
    }
  }
}

TEST_CASE("compressor settings that crossed a thread boundary are clamped", "[unit]") {
  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInf = std::numeric_limits<float>::infinity();

  const rt::CompressorConfig nonsense = rt::make_compressor(kNan, kInf, -5.0F, kNan, 0, 0);
  CHECK(nonsense.threshold_db == 0.0F);
  CHECK(nonsense.ratio == rt::kMinCompressorRatio);
  CHECK(nonsense.knee_db == 0.0F);
  CHECK(nonsense.makeup_db == 0.0F);
  CHECK(std::isfinite(nonsense.makeup_gain));
  CHECK(std::isfinite(nonsense.knee_low_linear));

  // A ratio of infinity falling back to 1 rather than clamping to 20 is the same
  // rule the EQ and the strip fader follow: a meaningless number must not become
  // the most extreme legal setting.
  CHECK(rt::Compressor::gain_for(nonsense, 1.0F) == 1.0F);

  const rt::CompressorConfig clamped = rt::make_compressor(-200.0F, 1'000.0F, 100.0F, 100.0F, 1, 1);
  CHECK(clamped.threshold_db == rt::kMinCompressorThresholdDb);
  CHECK(clamped.ratio == rt::kMaxCompressorRatio);
  CHECK(clamped.knee_db == rt::kMaxCompressorKneeDb);
  CHECK(clamped.makeup_db == rt::kMaxCompressorMakeupDb);
}

TEST_CASE("a disabled compressor does not touch a single sample", "[unit]") {
  const std::vector<float> plain = render_with(rt::StripConfig{});

  rt::StripConfig configured;
  configured.compressor = rt::make_compressor(-40.0F, 8.0F, 6.0F, 12.0F, 48, 4'800);
  configured.compressor.enabled = false;

  CHECK(identical(plain, render_with(configured)));
  CHECK(peak_of(plain) > 0.1F);
}

TEST_CASE("an engaged compressor pulls the level down, and makeup puts it back", "[unit]") {
  rt::StripConfig squashed;
  squashed.compressor = rt::make_compressor(-30.0F, 20.0F, 3.0F, 0.0F, 1, 480);

  const std::vector<float> plain = render_with(rt::StripConfig{});
  const std::vector<float> compressed = render_with(squashed);
  CHECK(peak_of(compressed) < peak_of(plain));

  // Makeup is applied after the curve, so the same settings with makeup are
  // louder than without -- and the reduction meter reports the curve alone.
  rt::StripConfig with_makeup = squashed;
  with_makeup.compressor =
      rt::make_compressor(-30.0F, 20.0F, 3.0F, rt::kMaxCompressorMakeupDb, 1, 480);
  CHECK(peak_of(render_with(with_makeup)) > peak_of(compressed));
}

TEST_CASE("gain reduction is metered without the makeup folded in", "[unit]") {
  // A compressor pulling 6 dB down with 6 dB of makeup is doing something, and a
  // meter that reported the net would say it was not.
  auto reduction_for = [](float makeup_db) {
    engine::Engine eng{compressor_engine_config()};
    REQUIRE(eng.set_pad_sample(0, loud_sample()));
    rt::StripConfig strip;
    strip.compressor = rt::make_compressor(-40.0F, 20.0F, 3.0F, makeup_db, 1, 4'800);
    REQUIRE(eng.set_strip(0, strip));
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));

    std::array<float, kBlock * kChannels> scratch{};
    std::array<float*, kChannels> channels{};
    for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
      channels[channel] = scratch.data() + (static_cast<std::size_t>(channel) * kBlock);
    }
    eng.render(std::span<float* const>{channels}, kBlock);
    return eng.telemetry().strip_reduction[0];
  };

  const float without = reduction_for(0.0F);
  const float with = reduction_for(12.0F);
  CHECK(without < 1.0F);  // it really is reducing
  CHECK(std::abs(without - with) < 1.0e-5F);

  // And a strip with no compressor reports unity rather than zero.
  engine::Engine idle{compressor_engine_config()};
  CHECK(idle.telemetry().strip_reduction[3] == 1.0F);
}

TEST_CASE("an engaged compressor is invariant to block size", "[unit]") {
  // The envelope is recursive, so it is exactly as able to break the
  // determinism contract as an IIR filter is -- and the same detector catches
  // it. This is what fails if the envelope is ever reset per block, or if the
  // detector is computed from a block maximum rather than per frame.
  rt::StripConfig strip;
  strip.compressor = rt::make_compressor(-35.0F, 6.0F, 9.0F, 6.0F, 24, 2'400);

  constexpr std::size_t kTotal = 2'048;
  auto render_in = [&strip](std::span<const std::size_t> sizes) {
    engine::Engine eng{compressor_engine_config()};
    REQUIRE(eng.set_pad_sample(0, loud_sample()));
    REQUIRE(eng.set_strip(0, strip));
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));

    std::vector<float> out(kTotal * kChannels, 0.0F);
    std::array<float, 2'048 * kChannels> scratch{};
    std::size_t done = 0;
    std::size_t next = 0;
    while (done < kTotal) {
      const std::size_t block = std::min(sizes[next % sizes.size()], kTotal - done);
      ++next;
      scratch.fill(-8.125F);
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

TEST_CASE("the detector is the maximum across channels, not one of them", "[unit]") {
  // The detector reads the loudest channel precisely so gain reduction cannot
  // pull the stereo image sideways: a per-channel compressor moves a hard-panned
  // hit toward the middle every time it fires.
  //
  // BOTH ORDERINGS, and that is the point. An earlier version of this test put
  // the loud content on channel 0 only -- against which "the maximum of the
  // channels" and "channel 0" are the same number, so replacing the detector
  // with `buffer[0]` passed the entire suite. Whichever channel a broken
  // implementation happens to pick, one of these two cases has the loud material
  // on the other one.
  for (const std::uint16_t loud_channel : {std::uint16_t{0}, std::uint16_t{1}}) {
    const std::uint16_t quiet_channel = loud_channel == 0 ? 1 : 0;
    constexpr float kLoud = 0.9F;
    constexpr float kQuiet = 0.05F;

    auto sample = std::make_shared<rt::Sample>(kRate, kChannels, std::size_t{8'000});
    for (std::size_t frame = 0; frame < 8'000; ++frame) {
      const auto phase = static_cast<float>(std::sin(0.05 * static_cast<double>(frame)));
      sample->mutable_channel(loud_channel)[frame] = phase * kLoud;
      sample->mutable_channel(quiet_channel)[frame] = phase * kQuiet;
    }

    engine::Engine eng{compressor_engine_config()};
    REQUIRE(eng.set_pad_sample(0, sample));
    rt::StripConfig strip;
    strip.compressor = rt::make_compressor(-20.0F, 20.0F, 0.0F, 0.0F, 1, 48'000);
    REQUIRE(eng.set_strip(0, strip));
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));

    std::array<float, kBlock * kChannels> scratch{};
    std::array<float*, kChannels> channels{};
    for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
      channels[channel] = scratch.data() + (static_cast<std::size_t>(channel) * kBlock);
    }
    eng.render(std::span<float* const>{channels}, kBlock);

    float loud = 0.0F;
    float quiet = 0.0F;
    for (std::size_t frame = 0; frame < kBlock; ++frame) {
      loud = std::max(loud, std::abs(channels[loud_channel][frame]));
      quiet = std::max(quiet, std::abs(channels[quiet_channel][frame]));
    }
    INFO("loud material on channel " << loud_channel);
    REQUIRE(loud > 0.0F);
    REQUIRE(quiet > 0.0F);

    // The ratio between the channels going in is preserved exactly by one shared
    // gain. It is NOT preserved by two independent detectors: the quiet channel
    // never crosses the threshold, so it would come out untouched while the loud
    // one was pulled down -- the image moving.
    CHECK(std::abs((quiet / loud) - (kQuiet / kLoud)) < 1.0e-3F);

    // And the loud channel really was reduced, so the ratio above is not
    // preserved merely because nothing happened.
    CHECK(loud < kLoud * 0.9F);
  }
}
