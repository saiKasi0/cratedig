#ifndef CRATEDIG_RT_EQ_HPP
#define CRATEDIG_RT_EQ_HPP

#include "rt/biquad.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rt {

// Four bands per strip, in fixed order: low shelf, two peaking, high shelf.
//
// Fixed rather than a list the user builds, for the reason docs/MIXER.md gives
// about the whole chain: a fixed shape needs no allocation, no reordering
// protocol and no null node to check for. Arbitrary processors in arbitrary
// order is M8's plugin chain, which has a swap protocol this does not need.
inline constexpr std::size_t kEqBands = 4;

enum class EqBandType : std::uint8_t {
  kLowShelf = 0,
  kPeaking,
  kHighShelf,
};

// Ranges. docs/MIXER.md, "Ranges".
inline constexpr float kMinEqFrequency = 10.0F;
inline constexpr float kMaxEqFrequency = 20'000.0F;

// The ceiling is also held below Nyquist by this fraction of the sample rate.
// RBJ coefficients degenerate as w0 approaches pi, so a UI offering 20 kHz at a
// 44.1 kHz rate is a UI offering a filter that is no longer the filter it says
// it is.
inline constexpr float kMaxEqFrequencyFraction = 0.45F;

inline constexpr float kMinEqQ = 0.1F;
inline constexpr float kMaxEqQ = 18.0F;

// SHELF SLOPE IS CAPPED AT 1, and that is a correctness bound rather than taste.
//
// S = 1 is the steepest shelf that stays monotonic; above it the response
// overshoots the plateau (measured: +0.116 dB at S = 1.2, +1.383 dB at S = 2 on
// a +12 dB shelf). Worse, RBJ's shelf alpha contains
// sqrt((A + 1/A)(1/S - 1) + 2), whose radicand goes NEGATIVE for S > 1 at high
// gain -- at +24 dB and S = 2 it is -0.116, so sqrt returns NaN and the whole
// strip fills with NaN. For S <= 1 the term (1/S - 1) is non-negative, so the
// radicand is at least 2 for every gain, and the degenerate case cannot arise
// at all.
//
// Clamping the RADICAND instead was tried and rejected: it keeps the arithmetic
// finite and produces a filter that is not a shelf -- a "+24 dB" low shelf came
// out +69.8 dB at 100 Hz with a -45 dB notch above it. Finite is not the same as
// correct.
inline constexpr float kMinEqSlope = 0.1F;
inline constexpr float kMaxEqSlope = 1.0F;

inline constexpr float kMaxEqGainDb = 24.0F;

// Default shapes, and what a non-finite parameter falls back to.
inline constexpr float kDefaultEqQ = 0.707F;
inline constexpr float kDefaultEqSlope = 1.0F;
inline constexpr float kDefaultEqFrequency = 1'000.0F;

// Finite values are clamped into range; non-finite ones fall back.
//
// The split matters. Clamping is what a slider dragged past its end should do.
// But NaN compares false against everything and an infinity clamps to the
// nearest bound, which for a gain would turn "this number is meaningless" into
// "play this as loud as the mixer allows" -- the trap rt::clamp_strip_gain
// already names.
[[nodiscard]] inline float clamp_eq_finite(float value, float low, float high,
                                           float fallback) noexcept {
  return std::isfinite(value) ? std::min(std::max(value, low), high) : fallback;
}

[[nodiscard]] inline float clamp_eq_frequency(float frequency, std::uint32_t sample_rate) noexcept {
  const float nyquist_limit = kMaxEqFrequencyFraction * static_cast<float>(sample_rate);
  const float high = std::min(kMaxEqFrequency, nyquist_limit);
  // A sample rate low enough to put the ceiling under the floor is not a rate
  // this engine runs at, but the audio thread still has to be handed something
  // ordered.
  const float low = std::min(kMinEqFrequency, high);
  return clamp_eq_finite(frequency, low, high, low);
}

[[nodiscard]] inline float clamp_eq_q(float q) noexcept {
  return clamp_eq_finite(q, kMinEqQ, kMaxEqQ, kDefaultEqQ);
}

[[nodiscard]] inline float clamp_eq_slope(float slope) noexcept {
  return clamp_eq_finite(slope, kMinEqSlope, kMaxEqSlope, kDefaultEqSlope);
}

[[nodiscard]] inline float clamp_eq_gain_db(float gain_db) noexcept {
  return clamp_eq_finite(gain_db, -kMaxEqGainDb, kMaxEqGainDb, 0.0F);
}

// One band: what it is set to, and the coefficients that follow from it.
//
// BOTH, in one struct, built by one function. The audio thread reads only
// `coeffs`; the interface reads only the parameters. Keeping them apart would be
// two things to publish and two chances for a display to disagree with a sound,
// so make_eq_band() is the only supported way to produce a band whose
// coefficients mean anything.
struct EqBand {
  BiquadCoeffs coeffs{};

  float frequency = kDefaultEqFrequency;
  float gain_db = 0.0F;

  // Q for a peaking band, S for a shelf. One field because a band has exactly
  // one shape control and which one it is follows from `type` -- two fields
  // would mean one of them is always meaningless and occasionally believed.
  float shape = kDefaultEqQ;

  EqBandType type = EqBandType::kPeaking;

  // Off. A strip that nobody has touched must not filter anything, and
  // `coeffs` defaults to passthrough as well so this is belt and braces.
  bool enabled = false;
};

struct EqConfig {
  // Fixed order, and the default frequencies spread across the band so that
  // enabling a band does something plausible before anything is tuned.
  std::array<EqBand, kEqBands> bands{
      EqBand{.frequency = 120.0F, .shape = kDefaultEqSlope, .type = EqBandType::kLowShelf},
      EqBand{.frequency = 500.0F, .shape = kDefaultEqQ, .type = EqBandType::kPeaking},
      EqBand{.frequency = 2'500.0F, .shape = kDefaultEqQ, .type = EqBandType::kPeaking},
      EqBand{.frequency = 8'000.0F, .shape = kDefaultEqSlope, .type = EqBandType::kHighShelf},
  };

  [[nodiscard]] bool any_enabled() const noexcept {
    for (const EqBand& band : bands) {
      if (band.enabled) {
        return true;
      }
    }
    return false;
  }
};

namespace detail {

// Divides through by a0 and narrows to float.
//
// a0 cannot be zero for any parameters the clamps above allow: for the peaking
// form it is 1 + alpha/A with alpha and A both positive, and for the shelves it
// is a sum whose (A+1) term dominates (A-1)cos(w0) for every A > 0.
[[nodiscard]] inline BiquadCoeffs normalise(double b0, double b1, double b2, double a0, double a1,
                                            double a2) noexcept {
  const double inverse = 1.0 / a0;
  return BiquadCoeffs{
      .b0 = static_cast<float>(b0 * inverse),
      .b1 = static_cast<float>(b1 * inverse),
      .b2 = static_cast<float>(b2 * inverse),
      .a1 = static_cast<float>(a1 * inverse),
      .a2 = static_cast<float>(a2 * inverse),
  };
}

}  // namespace detail

// CONTROL THREAD. Derives one band's coefficients from its settings.
//
// RBJ Audio EQ Cookbook, transcribed in docs/MIXER.md, computed in double and
// narrowed once. Not constexpr, and it cannot be: sin, cos and sqrt are not
// constexpr in C++20. It does not need to be either -- this runs when somebody
// turns a knob, and MIXER.md's requirement is that it never runs per block.
//
// Returns an ENABLED band. A band you went to the trouble of specifying is one
// you want; switch it off afterwards if not.
[[nodiscard]] inline EqBand make_eq_band(EqBandType type, float frequency, float gain_db,
                                         float shape, std::uint32_t sample_rate) noexcept {
  EqBand band;
  band.type = type;
  band.frequency = clamp_eq_frequency(frequency, sample_rate);
  band.gain_db = clamp_eq_gain_db(gain_db);
  band.shape = type == EqBandType::kPeaking ? clamp_eq_q(shape) : clamp_eq_slope(shape);
  band.enabled = true;

  if (sample_rate == 0) {
    return band;  // passthrough coefficients; there is no filter to derive
  }

  // A = 10^(dB/40), not 10^(dB/20): the amplitude response reaches A^2 at the
  // peak or on the shelf plateau, so this is what makes "6 dB" mean 6 dB there.
  const double amplitude = std::pow(10.0, static_cast<double>(band.gain_db) / 40.0);
  const double omega = 2.0 * std::acos(-1.0) * static_cast<double>(band.frequency) /
                       static_cast<double>(sample_rate);
  const double cosine = std::cos(omega);
  const double sine = std::sin(omega);

  if (type == EqBandType::kPeaking) {
    const double alpha = sine / (2.0 * static_cast<double>(band.shape));
    band.coeffs =
        detail::normalise(1.0 + (alpha * amplitude), -2.0 * cosine, 1.0 - (alpha * amplitude),
                          1.0 + (alpha / amplitude), -2.0 * cosine, 1.0 - (alpha / amplitude));
    return band;
  }

  // Shelf alpha. The radicand is at least 2 for every gain because kMaxEqSlope
  // is 1, so (1/S - 1) is never negative -- see the note there.
  const double slope = static_cast<double>(band.shape);
  const double radicand = ((amplitude + (1.0 / amplitude)) * ((1.0 / slope) - 1.0)) + 2.0;
  const double alpha = sine / 2.0 * std::sqrt(radicand);
  const double shelf = 2.0 * std::sqrt(amplitude) * alpha;

  const double plus = amplitude + 1.0;
  const double minus = amplitude - 1.0;

  if (type == EqBandType::kLowShelf) {
    band.coeffs = detail::normalise(
        amplitude * (plus - (minus * cosine) + shelf), 2.0 * amplitude * (minus - (plus * cosine)),
        amplitude * (plus - (minus * cosine) - shelf), plus + (minus * cosine) + shelf,
        -2.0 * (minus + (plus * cosine)), plus + (minus * cosine) - shelf);
    return band;
  }

  band.coeffs = detail::normalise(
      amplitude * (plus + (minus * cosine) + shelf), -2.0 * amplitude * (minus + (plus * cosine)),
      amplitude * (plus + (minus * cosine) - shelf), plus - (minus * cosine) + shelf,
      2.0 * (minus - (plus * cosine)), plus - (minus * cosine) - shelf);
  return band;
}

}  // namespace rt

#endif  // CRATEDIG_RT_EQ_HPP
