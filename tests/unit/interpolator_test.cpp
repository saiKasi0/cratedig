#include "rt/interpolator.hpp"

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace {

// Signal-to-noise ratio of interpolating a pure sine at a given playback ratio,
// measured against the analytically evaluated sine at the same instants.
//
// No FFT is involved on purpose: comparing against the closed-form signal in the
// time domain measures exactly the quantity we care about (total error power,
// aliasing and all) and keeps PFFFT staged for M3 where onset detection actually
// needs it.
//
// Reference and accumulators are double so that what is measured is the kernel's
// error, not the rounding of the yardstick.
struct SnrDb {
  double hermite;
  double linear;
};

SnrDb measure_snr(double freq_hz, double sample_rate, double ratio, std::size_t out_frames) {
  constexpr double kTwoPi = 2.0 * std::numbers::pi;

  // The kernel reads x[i-1] .. x[i+2]; storage[0] holds index -1.
  const auto span_needed = static_cast<std::size_t>(static_cast<double>(out_frames) * ratio) + 8;
  std::vector<float> storage(span_needed + 2, 0.0F);
  for (std::size_t k = 0; k < storage.size(); ++k) {
    const double index = static_cast<double>(k) - 1.0;
    storage[k] = static_cast<float>(std::sin(kTwoPi * freq_hz * index / sample_rate));
  }
  const float* frame0 = storage.data() + 1;

  double signal_power = 0.0;
  double hermite_error = 0.0;
  double linear_error = 0.0;

  for (std::size_t m = 0; m < out_frames; ++m) {
    const double position = static_cast<double>(m) * ratio;
    const auto index = static_cast<std::ptrdiff_t>(std::floor(position));
    const auto frac = static_cast<float>(position - static_cast<double>(index));

    // Explicit widening: -Wdouble-promotion is an error here, and silencing it
    // by accident would hide a real float/double mix-up elsewhere.
    const auto hermite = static_cast<double>(
        rt::hermite4(frame0[index - 1], frame0[index], frame0[index + 1], frame0[index + 2], frac));
    const auto linear = static_cast<double>(rt::linear2(frame0[index], frame0[index + 1], frac));
    const double reference = std::sin(kTwoPi * freq_hz * position / sample_rate);

    signal_power += reference * reference;
    hermite_error += (hermite - reference) * (hermite - reference);
    linear_error += (linear - reference) * (linear - reference);
  }

  return SnrDb{10.0 * std::log10(signal_power / hermite_error),
               10.0 * std::log10(signal_power / linear_error)};
}

// The resampler-adjacent ratio: 44.1 kHz material played at 48 kHz. Unlike 0.5,
// 1.0 or 2.0 it visits every fractional phase, so it is the honest worst case.
// At ratio 1.0 or 2.0 the fraction is always exactly zero and BOTH kernels
// degenerate to a passthrough, which would make an SNR assertion there a
// measurement of nothing.
constexpr double kAwkwardRatio = 44'100.0 / 48'000.0;
constexpr double kSampleRate = 44'100.0;
constexpr std::size_t kMeasureFrames = 20'000;

}  // namespace

TEST_CASE("Hermite interpolation is bit-exact at t == 0", "[unit]") {
  // c0 is x0 and every other coefficient is multiplied by t, so t == 0 must
  // return x0 with no rounding at all. This is not a nicety: it is what makes
  // playback at ratio 1.0 a bit-exact passthrough, and therefore what makes the
  // engine's determinism goldens mean anything.
  const float values[] = {0.0F, 1.0F, -1.0F, 0.5F, -0.333F, 1e-20F, 0.987654321F};

  for (const float xm1 : values) {
    for (const float x0 : values) {
      for (const float x1 : values) {
        for (const float x2 : values) {
          CHECK(rt::hermite4(xm1, x0, x1, x2, 0.0F) == x0);
        }
      }
    }
  }
}

TEST_CASE("Hermite interpolation is constexpr", "[unit]") {
  // Every operand below is an exact binary fraction, so the expected value is
  // exact rather than approximate.
  STATIC_CHECK(rt::hermite4(0.0F, 1.0F, 0.0F, 0.0F, 0.5F) == 0.5625F);
  STATIC_CHECK(rt::hermite4(1.0F, 1.0F, 1.0F, 1.0F, 0.0F) == 1.0F);
  STATIC_CHECK(rt::linear2(0.0F, 1.0F, 0.5F) == 0.5F);
}

TEST_CASE("Hermite interpolation reproduces polynomials up to degree 2", "[unit]") {
  using Catch::Matchers::WithinAbs;

  // Stepped with an integer counter rather than `for (float t = ...; t += 0.05F)`:
  // accumulating a non-representable step drifts, and clang-tidy's
  // security.FloatLoopCounter rejects it outright.
  constexpr int kSteps = 20;
  auto step = [](int index) { return static_cast<float>(index) / static_cast<float>(kSteps); };

  SECTION("constant") {
    for (int index = 0; index < kSteps; ++index) {
      CHECK_THAT(rt::hermite4(3.5F, 3.5F, 3.5F, 3.5F, step(index)), WithinAbs(3.5, 1e-6));
    }
  }

  SECTION("linear") {
    // f(x) = 2x + 1 sampled at x = -1, 0, 1, 2
    for (int index = 0; index < kSteps; ++index) {
      const float t = step(index);
      const double expected = (2.0 * static_cast<double>(t)) + 1.0;
      CHECK_THAT(rt::hermite4(-1.0F, 1.0F, 3.0F, 5.0F, t), WithinAbs(expected, 1e-5));
    }
  }

  SECTION("quadratic") {
    // f(x) = x^2 sampled at x = -1, 0, 1, 2
    for (int index = 0; index < kSteps; ++index) {
      const float t = step(index);
      const double expected = static_cast<double>(t) * static_cast<double>(t);
      CHECK_THAT(rt::hermite4(1.0F, 0.0F, 1.0F, 4.0F, t), WithinAbs(expected, 1e-5));
    }
  }
}

TEST_CASE("Hermite interpolation does NOT reproduce cubics", "[unit]") {
  // Documented deliberately. The slope at each end is estimated by a centred
  // difference, (x1 - xm1)/2, which is not the derivative of a cubic -- so this
  // kernel is exact to degree 2 and no further.
  //
  // The trap this guards: f(x) = x^3 happens to come out exact at t = 0.5, so
  // anyone spot-checking there concludes cubics are reproduced, "strengthens"
  // the test above to degree 3, and ships something that fails everywhere else.
  const float xm1 = -1.0F;
  const float x0 = 0.0F;
  const float x1 = 1.0F;
  const float x2 = 8.0F;

  CHECK_THAT(rt::hermite4(xm1, x0, x1, x2, 0.5F),
             Catch::Matchers::WithinAbs(0.125, 1e-6));  // coincidentally exact

  const auto actual = static_cast<double>(rt::hermite4(xm1, x0, x1, x2, 0.25F));
  const double true_cubic = 0.25 * 0.25 * 0.25;  // 0.015625
  CHECK(std::abs(actual - true_cubic) > 0.05);   // ~0.109 in practice
}

TEST_CASE("Hermite interpolation meets the M1 SNR budget", "[unit]") {
  // The ROADMAP M1 acceptance criterion: "unit tests for interpolator SNR pass".
  //
  // Thresholds are measured values rounded down with roughly 5 dB of margin, not
  // aspirations -- see docs/TESTING.md for the measured table and the reasoning.
  // They fall steeply with frequency because a 4-point kernel has little to work
  // with once a partial approaches Nyquist; that is a property of the kernel, not
  // a defect, and pretending otherwise would mean a threshold nobody can meet.
  struct Case {
    double freq_hz;
    double min_snr_db;
  };
  const Case cases[] = {
      {100.0, 140.0},    // measured 146.8
      {440.0, 105.0},    // measured 110.9
      {1'000.0, 85.0},   // measured  89.4
      {4'000.0, 48.0},   // measured  51.5
      {10'000.0, 22.0},  // measured  24.1
  };

  for (const Case& item : cases) {
    const SnrDb snr = measure_snr(item.freq_hz, kSampleRate, kAwkwardRatio, kMeasureFrames);
    CAPTURE(item.freq_hz, snr.hermite, item.min_snr_db);
    CHECK(snr.hermite > item.min_snr_db);
  }
}

TEST_CASE("Hermite interpolation beats linear by a wide margin", "[unit]") {
  // An absolute SNR threshold alone is a weak test: a kernel that silently
  // degraded to something cruder could still clear it at low frequencies. The
  // margin over linear is what actually pins down that the extra two taps are
  // being used.
  struct Case {
    double freq_hz;
    double min_gain_db;
  };
  const Case cases[] = {
      {440.0, 35.0},    // measured 42.0
      {1'000.0, 28.0},  // measured 34.8
      {4'000.0, 15.0},  // measured 20.9
      {10'000.0, 6.0},  // measured  9.0
  };

  for (const Case& item : cases) {
    const SnrDb snr = measure_snr(item.freq_hz, kSampleRate, kAwkwardRatio, kMeasureFrames);
    const double gain = snr.hermite - snr.linear;
    CAPTURE(item.freq_hz, snr.hermite, snr.linear, gain);
    CHECK(gain > item.min_gain_db);
  }
}

TEST_CASE("Playback at an integer ratio is a passthrough", "[unit]") {
  // At ratio 1.0 and 2.0 the fractional part is always exactly zero, so the
  // kernel must return input frames untouched. This is why a sample already at
  // the engine's rate survives playback bit-exactly.
  std::vector<float> storage(1'000, 0.0F);
  for (std::size_t k = 0; k < storage.size(); ++k) {
    storage[k] = static_cast<float>(std::sin(0.01 * static_cast<double>(k)));
  }
  const float* frame0 = storage.data() + 1;

  for (std::ptrdiff_t index = 0; index < 900; ++index) {
    CHECK(rt::hermite4(frame0[index - 1], frame0[index], frame0[index + 1], frame0[index + 2],
                       0.0F) == frame0[index]);
  }
}
