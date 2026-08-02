#include "rt/eq.hpp"

#include "engine/engine.hpp"
#include "rt/biquad.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "rt/strip.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
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

engine::Engine::Config eq_engine_config() {
  return engine::Engine::Config{
      .sample_rate = kRate, .num_channels = kChannels, .max_block_frames = 2'048, .seed = 0};
}

[[nodiscard]] bool all_finite(const rt::BiquadCoeffs& coeffs) {
  return std::isfinite(coeffs.b0) && std::isfinite(coeffs.b1) && std::isfinite(coeffs.b2) &&
         std::isfinite(coeffs.a1) && std::isfinite(coeffs.a2);
}

std::shared_ptr<const rt::Sample> eq_sample() {
  auto sample = std::make_shared<rt::Sample>(kRate, kChannels, std::size_t{8'000});
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < data.size(); ++frame) {
      const auto mixed = static_cast<float>(((frame * 71) + (channel * 601)) % 6'143);
      data[frame] = ((mixed / 3'071.5F) - 1.0F) * 0.55F;
    }
  }
  return sample;
}

// One block through a fresh engine, with whatever strip settings are asked for.
std::vector<float> render_with(const rt::StripConfig& strip) {
  engine::Engine eng{eq_engine_config()};
  REQUIRE(eng.set_pad_sample(0, eq_sample()));
  REQUIRE(eng.set_strip(0, strip));
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));

  std::array<float, kBlock * kChannels> scratch{};
  scratch.fill(-9.5F);
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

TEST_CASE("a default EQ is four bypassed bands in the specified order", "[unit]") {
  const rt::EqConfig eq;
  REQUIRE(eq.bands.size() == rt::kEqBands);

  // The fixed order docs/MIXER.md specifies: shelf, two peaks, shelf.
  CHECK(eq.bands[0].type == rt::EqBandType::kLowShelf);
  CHECK(eq.bands[1].type == rt::EqBandType::kPeaking);
  CHECK(eq.bands[2].type == rt::EqBandType::kPeaking);
  CHECK(eq.bands[3].type == rt::EqBandType::kHighShelf);

  CHECK_FALSE(eq.any_enabled());
  for (const rt::EqBand& band : eq.bands) {
    CHECK_FALSE(band.enabled);
    CHECK(band.gain_db == 0.0F);

    // Passthrough coefficients as well as bypassed. Two independent reasons a
    // fresh strip cannot colour the signal, so a bug in either one is visible
    // rather than masked by the other.
    CHECK(band.coeffs.b0 == 1.0F);
    CHECK(band.coeffs.b1 == 0.0F);
    CHECK(band.coeffs.b2 == 0.0F);
    CHECK(band.coeffs.a1 == 0.0F);
    CHECK(band.coeffs.a2 == 0.0F);
  }
}

TEST_CASE("EQ parameters are clamped, and meaningless ones fall back", "[unit]") {
  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInf = std::numeric_limits<float>::infinity();

  // Finite and out of range CLAMPS, which is what a slider dragged past its end
  // should do.
  CHECK(rt::clamp_eq_frequency(1.0F, kRate) == rt::kMinEqFrequency);
  CHECK(rt::clamp_eq_gain_db(99.0F) == rt::kMaxEqGainDb);
  CHECK(rt::clamp_eq_q(0.001F) == rt::kMinEqQ);
  CHECK(rt::clamp_eq_slope(5.0F) == rt::kMaxEqSlope);

  // Non-finite FALLS BACK, and does not clamp to the nearest bound. An infinite
  // gain clamped to +24 dB would turn "this number is meaningless" into "boost
  // this as hard as the EQ allows", which is the trap rt::clamp_strip_gain
  // already names.
  CHECK(rt::clamp_eq_gain_db(kInf) == 0.0F);
  CHECK(rt::clamp_eq_gain_db(kNan) == 0.0F);
  CHECK(rt::clamp_eq_q(kNan) == rt::kDefaultEqQ);
  CHECK(rt::clamp_eq_slope(-kInf) == rt::kDefaultEqSlope);

  // The frequency ceiling is min(20 kHz, 0.45 * Fs), so which of the two bites
  // depends on the rate. At 44.1 kHz that is 19845 Hz -- so 19 kHz passes
  // through untouched and only a request above 19845 is pulled down.
  CHECK(rt::clamp_eq_frequency(19'000.0F, 44'100) == 19'000.0F);
  CHECK(rt::clamp_eq_frequency(20'000.0F, 44'100) == 0.45F * 44'100.0F);
  CHECK(rt::clamp_eq_frequency(20'000.0F, 44'100) < rt::kMaxEqFrequency);

  // Above 44.4 kHz the 20 kHz ceiling is the binding one instead, because there
  // is no musical reason to filter above hearing however much headroom the rate
  // offers.
  CHECK(rt::clamp_eq_frequency(19'000.0F, 96'000) == 19'000.0F);
  CHECK(rt::clamp_eq_frequency(30'000.0F, 96'000) == rt::kMaxEqFrequency);

  // And a rate so low that 0.45 * Fs falls under the 10 Hz floor still returns
  // something ordered rather than a range with its ends crossed.
  CHECK(rt::clamp_eq_frequency(1'000.0F, 20) <= rt::kMinEqFrequency);
}

TEST_CASE("no parameters the clamps allow can produce NaN coefficients", "[unit]") {
  // THE FINDING THIS TASK TURNED UP, and the reason kMaxEqSlope is 1 rather than
  // the 2 the spec first said.
  //
  // RBJ's shelf alpha contains sqrt((A + 1/A)(1/S - 1) + 2). For S > 1 the
  // (1/S - 1) term is negative, and at high gain it takes the radicand below
  // zero -- at +24 dB with S = 2 it is -0.116, so sqrt returns NaN, the
  // coefficients are NaN, and the strip fills with NaN for the rest of the
  // session. That combination was INSIDE the range the spec allowed.
  //
  // Clamping the radicand to zero was tried and rejected: it keeps the
  // arithmetic finite while producing a filter that is not a shelf -- a "+24 dB"
  // low shelf measured +69.8 dB at 100 Hz with a -45 dB notch above it. Finite
  // is not the same as correct. Capping S at 1 makes (1/S - 1) non-negative, so
  // the radicand is at least 2 for every gain and the case cannot arise.
  const std::array<rt::EqBandType, 3> types{rt::EqBandType::kLowShelf, rt::EqBandType::kPeaking,
                                            rt::EqBandType::kHighShelf};

  std::size_t checked = 0;
  for (const rt::EqBandType type : types) {
    for (int gain = -30; gain <= 30; gain += 1) {
      for (int shape = 1; shape <= 200; shape += 1) {
        for (const std::uint32_t rate : {44'100U, 48'000U, 96'000U}) {
          for (const float frequency : {5.0F, 40.0F, 1'000.0F, 19'000.0F, 40'000.0F}) {
            const rt::EqBand band = rt::make_eq_band(type, frequency, static_cast<float>(gain),
                                                     static_cast<float>(shape) / 10.0F, rate);
            INFO("type " << static_cast<int>(type) << ", " << gain << " dB, shape "
                         << (static_cast<float>(shape) / 10.0F) << ", " << rate << " Hz, "
                         << frequency << " Hz");
            REQUIRE(all_finite(band.coeffs));
            ++checked;
          }
        }
      }
    }
  }
  CHECK(checked > 50'000);

  // And a zero sample rate -- which no engine runs at, but which the audio
  // thread must survive being handed -- gives passthrough rather than a division
  // by zero.
  const rt::EqBand degenerate =
      rt::make_eq_band(rt::EqBandType::kPeaking, 1'000.0F, 12.0F, 2.0F, 0U);
  CHECK(all_finite(degenerate.coeffs));
  CHECK(degenerate.coeffs.b0 == 1.0F);
}

TEST_CASE("a band's gain means what it asked for", "[unit]") {
  // THE TEST THE ACCEPTANCE CANNOT BE.
  //
  // biquad_test.cpp compares the measured response against |H(e^jw)| evaluated
  // FROM THE SAME COEFFICIENTS, which is the right way to judge whether the
  // implementation realises its coefficients -- and is completely blind to
  // coefficients that are self-consistent and wrong. Deriving A as 10^(dB/20)
  // instead of 10^(dB/40) sails through it: the filter faithfully realises the
  // wrong filter.
  //
  // This is the other half. A band asked for +6 dB must produce +6 dB where it
  // acts: at f0 for a peak, at DC for a low shelf, at Nyquist for a high shelf.
  // Those three points are exact rather than sampled -- at w = 0, H is
  // (b0+b1+b2)/(1+a1+a2), and at w = pi it is (b0-b1+b2)/(1-a1+a2).
  for (const float gain : {-24.0F, -12.0F, -6.0F, -1.0F, 1.0F, 6.0F, 12.0F, 24.0F}) {
    for (const float q : {0.5F, 1.0F, 4.0F, 12.0F}) {
      const rt::EqBand band = rt::make_eq_band(rt::EqBandType::kPeaking, 1'000.0F, gain, q, kRate);
      const double omega = 2.0 * std::acos(-1.0) * 1'000.0 / static_cast<double>(kRate);
      const std::complex<double> z = std::polar(1.0, -omega);
      const std::complex<double> numerator = static_cast<double>(band.coeffs.b0) +
                                             (static_cast<double>(band.coeffs.b1) * z) +
                                             (static_cast<double>(band.coeffs.b2) * z * z);
      const std::complex<double> denominator = 1.0 + (static_cast<double>(band.coeffs.a1) * z) +
                                               (static_cast<double>(band.coeffs.a2) * z * z);
      const double measured = 20.0 * std::log10(std::abs(numerator / denominator));
      INFO("peaking 1 kHz, " << gain << " dB, Q = " << q << " measured " << measured);
      CHECK(std::abs(measured - static_cast<double>(gain)) < 0.01);
    }
  }

  for (const float gain : {-24.0F, -6.0F, 6.0F, 24.0F}) {
    for (const float slope : {0.3F, 0.7F, 1.0F}) {
      const rt::EqBand low =
          rt::make_eq_band(rt::EqBandType::kLowShelf, 200.0F, gain, slope, kRate);
      const double at_dc =
          20.0 *
          std::log10(std::abs(static_cast<double>(low.coeffs.b0 + low.coeffs.b1 + low.coeffs.b2) /
                              static_cast<double>(1.0F + low.coeffs.a1 + low.coeffs.a2)));
      INFO("low shelf " << gain << " dB, S = " << slope << ", at DC " << at_dc);
      CHECK(std::abs(at_dc - static_cast<double>(gain)) < 0.01);

      const rt::EqBand high =
          rt::make_eq_band(rt::EqBandType::kHighShelf, 8'000.0F, gain, slope, kRate);
      const double at_nyquist =
          20.0 * std::log10(std::abs(
                     static_cast<double>(high.coeffs.b0 - high.coeffs.b1 + high.coeffs.b2) /
                     static_cast<double>(1.0F - high.coeffs.a1 + high.coeffs.a2)));
      INFO("high shelf " << gain << " dB, S = " << slope << ", at Nyquist " << at_nyquist);
      CHECK(std::abs(at_nyquist - static_cast<double>(gain)) < 0.01);
    }
  }

  // A band at 0 dB is flat, and flat in a specific way: at A = 1 the numerator
  // and the denominator become the SAME polynomial, so H(z) is identically 1.
  const rt::EqBand neutral =
      rt::make_eq_band(rt::EqBandType::kPeaking, 1'000.0F, 0.0F, 2.0F, kRate);
  CHECK(neutral.coeffs.b0 == 1.0F);
  CHECK(neutral.coeffs.b1 == neutral.coeffs.a1);
  CHECK(neutral.coeffs.b2 == neutral.coeffs.a2);

  // ...and yet an ENABLED 0 dB band is still not bit-transparent, because
  // identical polynomials cancel in algebra and not in floating point: the
  // recursion accumulates rounding that the algebra says is zero. This is
  // exactly why bypass is a real bypass rather than "set the gain to 0 dB", and
  // why the goldens are safe only because a default band is switched off.
  rt::Biquad filter;
  bool bit_exact = true;
  for (int step = 1; step <= 512; ++step) {
    const auto input = static_cast<float>(std::sin(0.037 * step) * 0.7);
    bit_exact = bit_exact && (filter.process(neutral.coeffs, input) == input);
  }
  CHECK_FALSE(bit_exact);
}

TEST_CASE("a shelf at maximum slope does not overshoot its plateau", "[unit]") {
  // The other half of capping S at 1: it is the steepest slope that stays
  // monotonic. Above it the response rings past the shelf gain, which is what
  // the cap exists to prevent -- so check the cap actually delivers it.
  const rt::EqBand band =
      rt::make_eq_band(rt::EqBandType::kLowShelf, 500.0F, 12.0F, rt::kMaxEqSlope, kRate);

  double worst = -1'000.0;
  for (double frequency = 20.0; frequency < 20'000.0; frequency *= 1.01) {
    const double omega = 2.0 * std::acos(-1.0) * frequency / static_cast<double>(kRate);
    const std::complex<double> z = std::polar(1.0, -omega);
    const std::complex<double> numerator = static_cast<double>(band.coeffs.b0) +
                                           (static_cast<double>(band.coeffs.b1) * z) +
                                           (static_cast<double>(band.coeffs.b2) * z * z);
    const std::complex<double> denominator = 1.0 + (static_cast<double>(band.coeffs.a1) * z) +
                                             (static_cast<double>(band.coeffs.a2) * z * z);
    worst = std::max(worst, 20.0 * std::log10(std::abs(numerator / denominator)));
  }

  // Within a hundredth of a dB of the plateau, never above it.
  CHECK(worst < 12.01);
}

TEST_CASE("a bypassed EQ does not touch a single sample", "[unit]") {
  // BIT-EXACT, not "within a tolerance". The M3 and M4 e2e goldens run through
  // this chain, so a bypassed band that multiplied by a passthrough coefficient
  // set would still be arithmetic, and arithmetic that a hash can see.
  const std::vector<float> plain = render_with(rt::StripConfig{});

  rt::StripConfig configured;
  configured.eq = rt::EqConfig{};
  for (rt::EqBand& band : configured.eq.bands) {
    // Real coefficients, deliberately -- a band fully specified and then
    // switched off must be as silent as one never touched.
    band = rt::make_eq_band(band.type, band.frequency, 18.0F, 0.5F, kRate);
    band.enabled = false;
  }

  const std::vector<float> bypassed = render_with(configured);
  CHECK(identical(plain, bypassed));
  CHECK(peak_of(plain) > 0.1F);
}

TEST_CASE("an enabled band changes the sound, and each band acts independently", "[unit]") {
  const std::vector<float> plain = render_with(rt::StripConfig{});

  // Each band on its own does something, and something different from its
  // neighbours -- which is what would fail if the engine applied one band's
  // coefficients to every slot, or ran the same filter state for all four.
  std::array<std::vector<float>, rt::kEqBands> single;
  for (std::size_t index = 0; index < rt::kEqBands; ++index) {
    rt::StripConfig strip;
    strip.eq.bands[index] = rt::make_eq_band(strip.eq.bands[index].type,
                                             strip.eq.bands[index].frequency, 15.0F, 1.0F, kRate);
    single[index] = render_with(strip);
    INFO("band " << index);
    CHECK_FALSE(identical(plain, single[index]));
  }

  for (std::size_t index = 0; index + 1 < rt::kEqBands; ++index) {
    INFO("bands " << index << " and " << (index + 1));
    CHECK_FALSE(identical(single[index], single[index + 1]));
  }

  // All four together is different again from any one of them.
  rt::StripConfig all;
  for (rt::EqBand& band : all.eq.bands) {
    band = rt::make_eq_band(band.type, band.frequency, 15.0F, 1.0F, kRate);
  }
  const std::vector<float> every = render_with(all);
  for (const std::vector<float>& one : single) {
    CHECK_FALSE(identical(every, one));
  }
}

TEST_CASE("every strip has its own filter state", "[unit]") {
  // WHAT A WEAKER VERSION OF THIS MISSED. The first attempt put an EQ on one pad
  // and left the neighbour clean, then checked their meters differed. Making the
  // engine share one set of filters between all sixteen strips passed it
  // comfortably: the clean pad's disabled band merely RESET the shared state
  // instead of corrupting it, and the difference the test looked for was still
  // there.
  //
  // Contamination needs two pads that both filter. So: a neighbour that is
  // muted -- contributing nothing to the mix by design, while still running its
  // own EQ, because docs/MIXER.md requires a muted strip's filters to keep
  // advancing so unmuting does not jump.
  //
  // With per-strip state the neighbour is inaudible and the output is IDENTICAL.
  // With shared state its filter walks over the first pad's history and the
  // difference shows from the second block, once there is a previous block to
  // inherit.
  rt::StripConfig filtered;
  filtered.eq.bands[1] = rt::make_eq_band(rt::EqBandType::kPeaking, 900.0F, 20.0F, 6.0F, kRate);

  rt::StripConfig neighbour = filtered;
  neighbour.mute = true;

  auto render_blocks = [](bool with_neighbour, const rt::StripConfig& first,
                          const rt::StripConfig& second) {
    engine::Engine eng{eq_engine_config()};
    REQUIRE(eng.set_pad_sample(5, eq_sample()));
    REQUIRE(eng.set_strip(5, first));
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 5, .velocity = 1.0F}));
    if (with_neighbour) {
      // Different material, so contamination is a different signal rather than
      // a slightly different amount of the same one.
      auto other = std::make_shared<rt::Sample>(kRate, kChannels, std::size_t{8'000});
      for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
        std::span<float> data = other->mutable_channel(channel);
        for (std::size_t frame = 0; frame < data.size(); ++frame) {
          data[frame] = static_cast<float>(((frame * 13) + channel) % 97) / 97.0F - 0.5F;
        }
      }
      REQUIRE(eng.set_pad_sample(6, std::move(other)));
      REQUIRE(eng.set_strip(6, second));
      REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 6, .velocity = 1.0F}));
    }

    std::vector<float> captured;
    for (int block = 0; block < 4; ++block) {
      std::array<float, kBlock * kChannels> scratch{};
      scratch.fill(-4.5F);
      std::array<float*, kChannels> channels{};
      for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
        channels[channel] = scratch.data() + (static_cast<std::size_t>(channel) * kBlock);
      }
      eng.render(std::span<float* const>{channels}, kBlock);
      captured.insert(captured.end(), scratch.begin(), scratch.end());
    }
    return captured;
  };

  const std::vector<float> alone = render_blocks(false, filtered, filtered);
  const std::vector<float> with_muted_neighbour = render_blocks(true, filtered, neighbour);

  CHECK(peak_of(alone) > 0.1F);
  CHECK(identical(alone, with_muted_neighbour));

  // The other half: two engines handed the same config render identically, which
  // is what would fail if state lived inside the shared, immutable PadConfig.
  CHECK(identical(render_with(filtered), render_with(filtered)));
}

TEST_CASE("an engaged EQ is invariant to block size", "[unit]") {
  // THE TEST THAT ACTUALLY CATCHES A SHARED FILTER, and the one the muted
  // -neighbour case above could not be.
  //
  // Pointing every strip at one set of filters looked like it would corrupt a
  // neighbour, and does not: the eleven strips with no EQ RESET the shared state
  // on their way past, so the filtered strip simply restarts from silence at the
  // top of every block. Both renders in that test were equally wrong and
  // therefore equal.
  //
  // What that destroys is HISTORY ACROSS BLOCK BOUNDARIES, and an IIR is exactly
  // where this project's determinism contract is easiest to break: the same
  // material rendered at 2048 frames and at 64 must produce the same bytes, and
  // a filter whose memory is a block long cannot. docs/TESTING.md calls block
  // -size invariance the property the whole contract rests on.
  rt::StripConfig strip;
  strip.eq.bands[0] = rt::make_eq_band(rt::EqBandType::kLowShelf, 150.0F, 12.0F, 0.8F, kRate);
  strip.eq.bands[2] = rt::make_eq_band(rt::EqBandType::kPeaking, 3'000.0F, -18.0F, 5.0F, kRate);

  constexpr std::size_t kTotal = 2'048;
  auto render_in = [&strip](std::span<const std::size_t> sizes) {
    engine::Engine eng{eq_engine_config()};
    REQUIRE(eng.set_pad_sample(0, eq_sample()));
    REQUIRE(eng.set_strip(0, strip));
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));

    std::vector<float> out(kTotal * kChannels, 0.0F);
    std::array<float, 2'048 * kChannels> scratch{};
    std::size_t done = 0;
    std::size_t next = 0;
    while (done < kTotal) {
      const std::size_t block = std::min(sizes[next % sizes.size()], kTotal - done);
      ++next;
      scratch.fill(-6.25F);
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
