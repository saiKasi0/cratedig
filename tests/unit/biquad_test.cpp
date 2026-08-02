#include "rt/biquad.hpp"

#include "rt/eq.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint32_t kRate = 48'000;

// |H(e^jw)| in dB, evaluated DIRECTLY FROM THE SAME COEFFICIENTS the filter
// runs, per docs/MIXER.md:
//
//   H(e^jw) = (b0 + b1*e^-jw + b2*e^-2jw) / (1 + a1*e^-jw + a2*e^-2jw)
//
// No FFT anywhere, deliberately: an FFT brings its own windowing error to the
// measurement and the thing under test is the filter. double for the same reason
// tests/unit/interpolator_test.cpp uses it -- the reference must be better than
// what it is judging.
[[nodiscard]] double analytic_db(const rt::BiquadCoeffs& coeffs, double frequency) {
  const double omega = 2.0 * std::acos(-1.0) * frequency / static_cast<double>(kRate);
  const std::complex<double> z = std::polar(1.0, -omega);
  const std::complex<double> numerator = static_cast<double>(coeffs.b0) +
                                         (static_cast<double>(coeffs.b1) * z) +
                                         (static_cast<double>(coeffs.b2) * z * z);
  const std::complex<double> denominator =
      1.0 + (static_cast<double>(coeffs.a1) * z) + (static_cast<double>(coeffs.a2) * z * z);
  return 20.0 * std::log10(std::abs(numerator / denominator));
}

// The measurement window: 4800 frames at 48 kHz is a WHOLE NUMBER OF CYCLES at
// every probe frequency below (each is a multiple of 10 Hz), which makes the
// correlation exact. An off-integer window still lands within about 0.001 dB --
// measured -- so this is precision rather than a requirement.
constexpr std::size_t kMeasureFrames = 4'800;

// Backstop, and the only constant here that is a guess.
constexpr std::size_t kMaxSettleFrames = 400'000;

// How long THIS filter must run before its transient stops contributing.
//
// Derived rather than fixed, because a fixed settle has to be the worst case for
// every filter the suite measures and is still wrong for the next one added.
// This test began with a flat 2000 frames and failed on exactly the cases where
// that was not enough -- 120 Hz at Q = 8 needs 65363 frames, 1.36 seconds, more
// than thirty times as many. The filter was right; the measurement was not
// waiting.
//
// The transient decays as r^n where r is the magnitude of the SLOWEST pole, so
// reaching a floor of 1e-7 takes log(floor)/log(r) frames.
//
// The poles are the roots of z^2 + a1*z + a2. When they are a complex conjugate
// pair their common magnitude is sqrt(a2) -- and that shortcut is what this test
// used at first. It is wrong whenever the discriminant is non-negative and the
// poles are REAL AND SPLIT, because then only their product is a2 and one of
// them can sit far closer to the unit circle than the geometric mean. Measured
// on the case that exposed it, peaking at 120 Hz, -24 dB, Q = 0.5:
// sqrt(a2) = 0.9393 and asks for 257 frames, while the true dominant pole is
// 0.9980 and needs 8039 -- a factor of thirty-one. The filter was correct both
// times; the measurement was reading a transient and calling it the response.
[[nodiscard]] double dominant_pole(const rt::BiquadCoeffs& coeffs) {
  const auto a1 = static_cast<double>(coeffs.a1);
  const auto a2 = static_cast<double>(coeffs.a2);
  const double discriminant = (a1 * a1) - (4.0 * a2);
  if (discriminant < 0.0) {
    return std::sqrt(std::abs(a2));  // conjugate pair, equal magnitudes
  }
  const double root = std::sqrt(discriminant);
  return std::max(std::abs((-a1 + root) / 2.0), std::abs((-a1 - root) / 2.0));
}

[[nodiscard]] std::size_t settle_frames(const rt::BiquadCoeffs& coeffs) {
  const double radius = dominant_pole(coeffs);
  if (!(radius > 0.0)) {
    return 1;  // FIR: nothing to settle
  }
  if (radius >= 1.0) {
    return kMaxSettleFrames;  // not a stable filter; let the acceptance say so
  }
  const double frames = std::log(1.0e-7) / std::log(radius);
  return std::min(static_cast<std::size_t>(frames) + 1, kMaxSettleFrames);
}

struct Measurement {
  double by_correlation_db = 0.0;
  double by_peak_db = 0.0;
};

// Drives a sine through the real implementation and measures the steady-state
// amplitude two ways.
[[nodiscard]] Measurement measure(const rt::BiquadCoeffs& coeffs, double frequency) {
  rt::Biquad filter;
  const double omega = 2.0 * std::acos(-1.0) * frequency / static_cast<double>(kRate);
  const std::size_t settle = settle_frames(coeffs);

  for (std::size_t frame = 0; frame < settle; ++frame) {
    static_cast<void>(
        filter.process(coeffs, static_cast<float>(std::sin(omega * static_cast<double>(frame)))));
  }

  double cosine_sum = 0.0;
  double sine_sum = 0.0;
  double peak = 0.0;
  for (std::size_t frame = 0; frame < kMeasureFrames; ++frame) {
    const double phase = omega * static_cast<double>(settle + frame);
    const auto output =
        static_cast<double>(filter.process(coeffs, static_cast<float>(std::sin(phase))));
    cosine_sum += output * std::cos(phase);
    sine_sum += output * std::sin(phase);
    peak = std::max(peak, std::abs(output));
  }

  const double amplitude = 2.0 / static_cast<double>(kMeasureFrames) *
                           std::abs(std::complex<double>{cosine_sum, sine_sum});
  return Measurement{.by_correlation_db = 20.0 * std::log10(amplitude),
                     .by_peak_db = 20.0 * std::log10(peak)};
}

// THE ACCEPTANCE, in one place: measured magnitude within 0.1 dB of analytic.
void check_band(const rt::EqBand& band, std::span<const double> probes) {
  for (const double frequency : probes) {
    const double expected = analytic_db(band.coeffs, frequency);
    const double actual = measure(band.coeffs, frequency).by_correlation_db;
    const double error = std::abs(actual - expected);
    // INFO so a failure names the probe rather than just the number.
    INFO("f = " << frequency << " Hz, analytic " << expected << " dB, measured " << actual
                << " dB");
    CHECK(error < 0.1);
  }
}

constexpr std::array<double, 9> kProbes{50.0,    120.0,   400.0,    1'000.0, 2'500.0,
                                        6'000.0, 8'000.0, 12'000.0, 16'000.0};

}  // namespace

TEST_CASE("a default biquad is exact passthrough", "[unit]") {
  // b0 = 1 and nothing else, so every sample must come out as the bits it went
  // in as. Not "within a tolerance" -- a filter that is not filtering must not
  // touch the signal at all, which is what makes a bypassed band safe to leave
  // in the chain of a path with a committed hash.
  rt::Biquad filter;
  const rt::BiquadCoeffs passthrough;

  const std::array<float, 7> inputs{0.0F, 1.0F, -1.0F, 0.3333333F, -1.0e-8F, 12345.678F, -0.0F};
  for (const float input : inputs) {
    CHECK(filter.process(passthrough, input) == input);
  }
}

TEST_CASE("the biquad implements the Direct Form I difference equation", "[unit]") {
  // Checked against the equation written out by hand, so that the acceptance
  // below cannot pass by measuring a filter that is internally consistent but
  // is not the one docs/MIXER.md specifies.
  const rt::BiquadCoeffs coeffs{.b0 = 0.5F, .b1 = -0.25F, .b2 = 0.125F, .a1 = -0.3F, .a2 = 0.2F};
  rt::Biquad filter;

  const std::array<float, 6> inputs{1.0F, 0.5F, -0.75F, 0.25F, 0.0F, -1.0F};
  float x1 = 0.0F;
  float x2 = 0.0F;
  float y1 = 0.0F;
  float y2 = 0.0F;
  for (const float input : inputs) {
    const float expected = (coeffs.b0 * input) + (coeffs.b1 * x1) + (coeffs.b2 * x2) -
                           (coeffs.a1 * y1) - (coeffs.a2 * y2);
    CHECK(filter.process(coeffs, input) == expected);
    x2 = x1;
    x1 = input;
    y2 = y1;
    y1 = expected;
  }
}

TEST_CASE("reset clears the filter's memory", "[unit]") {
  const rt::EqBand band = rt::make_eq_band(rt::EqBandType::kPeaking, 1'000.0F, 12.0F, 4.0F, kRate);
  rt::Biquad filter;

  CHECK(filter.settled());
  static_cast<void>(filter.process(band.coeffs, 1.0F));
  CHECK_FALSE(filter.settled());

  filter.reset();
  CHECK(filter.settled());

  // And a reset filter answers exactly as a fresh one does, which is what makes
  // bypassing and re-enabling a band deterministic rather than dependent on how
  // long ago it was switched off.
  rt::Biquad fresh;
  for (int step = 0; step < 32; ++step) {
    const auto input = static_cast<float>(std::sin(0.1 * step));
    CHECK(filter.process(band.coeffs, input) == fresh.process(band.coeffs, input));
  }
}

TEST_CASE("a ringing filter fed silence decays instead of being cut", "[unit]") {
  // The property that makes "skip silent strips" a determinism bug rather than
  // an optimisation, asserted where the behaviour actually lives.
  const rt::EqBand band = rt::make_eq_band(rt::EqBandType::kPeaking, 800.0F, 18.0F, 12.0F, kRate);
  rt::Biquad filter;

  for (std::size_t frame = 0; frame < 200; ++frame) {
    static_cast<void>(filter.process(
        band.coeffs, static_cast<float>(std::sin(2.0 * std::acos(-1.0) * 800.0 *
                                                 static_cast<double>(frame) / kRate))));
  }

  // Silence in: the output must not fall to zero at once.
  const float first = filter.process(band.coeffs, 0.0F);
  CHECK(std::abs(first) > 0.01F);

  float tail = 0.0F;
  for (std::size_t frame = 0; frame < 64; ++frame) {
    tail = std::max(tail, std::abs(filter.process(band.coeffs, 0.0F)));
  }
  CHECK(tail > 0.0F);
}

TEST_CASE("the filter flushes its own denormals rather than trusting the FPU mode", "[unit]") {
  // rt::ScopedDenormalDisable sets FTZ/DAZ at audio-thread start, but the
  // offline renderer never opens a device and never sets them. If the flush were
  // left to the FPU, a live render and a bounce of the same material would
  // disagree in the far tail -- so it is done here, and this is what says so.
  const rt::EqBand band = rt::make_eq_band(rt::EqBandType::kPeaking, 1'000.0F, 6.0F, 2.0F, kRate);
  rt::Biquad filter;

  static_cast<void>(filter.process(band.coeffs, 1.0F));

  bool any_subnormal = false;
  for (std::size_t frame = 0; frame < 200'000; ++frame) {
    const float output = filter.process(band.coeffs, 0.0F);
    any_subnormal = any_subnormal || (std::fpclassify(output) == FP_SUBNORMAL);
  }
  CHECK_FALSE(any_subnormal);

  // And it reached exactly zero rather than grinding on in the denormal range
  // forever.
  CHECK(filter.settled());
}

TEST_CASE("EQ magnitude is within 0.1 dB of analytic: peaking", "[unit]") {
  // THE M5 ACCEPTANCE, for the peaking form.
  for (const float gain : {-24.0F, -12.0F, -3.0F, 3.0F, 12.0F, 24.0F}) {
    for (const float q : {0.5F, 0.707F, 2.0F, 8.0F}) {
      for (const float frequency : {120.0F, 1'000.0F, 6'000.0F}) {
        INFO("peaking " << frequency << " Hz, " << gain << " dB, Q = " << q);
        check_band(rt::make_eq_band(rt::EqBandType::kPeaking, frequency, gain, q, kRate), kProbes);
      }
    }
  }
}

TEST_CASE("EQ magnitude is within 0.1 dB of analytic: shelves", "[unit]") {
  for (const float gain : {-24.0F, -12.0F, -3.0F, 3.0F, 12.0F, 24.0F}) {
    for (const float slope : {0.3F, 0.7F, 1.0F}) {
      INFO("low shelf 200 Hz, " << gain << " dB, S = " << slope);
      check_band(rt::make_eq_band(rt::EqBandType::kLowShelf, 200.0F, gain, slope, kRate), kProbes);
      INFO("high shelf 8000 Hz, " << gain << " dB, S = " << slope);
      check_band(rt::make_eq_band(rt::EqBandType::kHighShelf, 8'000.0F, gain, slope, kRate),
                 kProbes);
    }
  }
}

TEST_CASE("the peak-of-samples measurement would fail a correct filter", "[unit]") {
  // WHY THE ACCEPTANCE ABOVE MEASURES BY CORRELATION, pinned so nobody
  // "simplifies" it back to the obvious thing.
  //
  // Peak-of-samples underestimates whenever there are few samples per cycle: the
  // sampling instants straddle the true peak rather than landing on it. Against
  // a filter that is exactly right it reads about -0.15 dB at 8 kHz and 18 kHz
  // -- outside the 0.1 dB budget, on nothing but the yardstick. The tempting
  // repair is to widen the tolerance, which is widening it to accommodate the
  // ruler; docs/TESTING.md already names that failure for the interpolator.
  const rt::EqBand band = rt::make_eq_band(rt::EqBandType::kHighShelf, 8'000.0F, 9.0F, 1.0F, kRate);

  double worst_peak_error = 0.0;
  double worst_correlation_error = 0.0;
  for (const double frequency : {1'000.0, 8'000.0, 15'000.0, 18'000.0, 21'000.0}) {
    const double expected = analytic_db(band.coeffs, frequency);
    const Measurement measured = measure(band.coeffs, frequency);
    worst_peak_error = std::max(worst_peak_error, std::abs(measured.by_peak_db - expected));
    worst_correlation_error =
        std::max(worst_correlation_error, std::abs(measured.by_correlation_db - expected));
  }

  // The correlation is inside the budget by two orders of magnitude...
  CHECK(worst_correlation_error < 0.001);

  // ...and the peak is outside it, on the same filter, from the same samples.
  CHECK(worst_peak_error > 0.1);
}
