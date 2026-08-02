#ifndef CRATEDIG_RT_COMPRESSOR_HPP
#define CRATEDIG_RT_COMPRESSOR_HPP

#include "rt/arch.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rt {

// Ranges. docs/MIXER.md, "The compressor".
inline constexpr float kMinCompressorRatio = 1.0F;
inline constexpr float kMaxCompressorRatio = 20.0F;
inline constexpr float kMinCompressorThresholdDb = -60.0F;
inline constexpr float kMaxCompressorThresholdDb = 0.0F;
inline constexpr float kMaxCompressorKneeDb = 24.0F;
inline constexpr float kMaxCompressorMakeupDb = 24.0F;

// One feed-forward, peak-detecting compressor's settings, with the two values
// that are expensive to compute already derived.
//
// PEAK, NOT RMS. This is a drum machine; an RMS detector on a kick transient is
// late by design. RMS is not offered rather than offered and wrong.
//
// The detector reads the MAXIMUM OF THE CHANNELS and one gain is applied to all
// of them, so gain reduction cannot pull the stereo image sideways -- which is
// what a per-channel compressor does to a snare panned off centre.
struct CompressorConfig {
  // exp(-1 / time_frames), derived on the control thread. The consequence worth
  // remembering is that the envelope reaches 1 - 1/e of a step after exactly
  // `time_frames` frames, and "attack time" means that here rather than "time to
  // full gain reduction" -- a definition that varies between manufacturers and
  // cannot be tested against.
  float attack_coeff = 0.0F;
  float release_coeff = 0.0F;

  // Linear, derived from makeup_db.
  float makeup_gain = 1.0F;

  // The level below which no reduction is possible, linear. Precomputed so the
  // quiet case -- which is most of the time on most strips -- costs one compare
  // instead of a log and a pow.
  float knee_low_linear = 1.0F;

  float threshold_db = 0.0F;
  float ratio = kMinCompressorRatio;
  float knee_db = 0.0F;

  // Kept for the interface to display; the audio thread reads the coefficients.
  std::size_t attack_frames = 0;
  std::size_t release_frames = 0;
  float makeup_db = 0.0F;

  // Off. R = 1 is already unity at every level -- verified, exactly, for every
  // knee width -- so this is an optimisation rather than a second code path, and
  // switching it on with the default ratio must change nothing.
  bool enabled = false;
};

[[nodiscard]] inline float clamp_compressor_finite(float value, float low, float high,
                                                   float fallback) noexcept {
  return std::isfinite(value) ? std::min(std::max(value, low), high) : fallback;
}

// CONTROL THREAD. Derives the coefficients from the settings.
//
// exp() lives here and not in the callback, for the reason docs/MIXER.md gives
// about EQ coefficients: this runs when somebody turns a knob.
[[nodiscard]] inline CompressorConfig make_compressor(float threshold_db, float ratio,
                                                      float knee_db, float makeup_db,
                                                      std::size_t attack_frames,
                                                      std::size_t release_frames) noexcept {
  CompressorConfig config;
  config.threshold_db = clamp_compressor_finite(threshold_db, kMinCompressorThresholdDb,
                                                kMaxCompressorThresholdDb, 0.0F);
  config.ratio =
      clamp_compressor_finite(ratio, kMinCompressorRatio, kMaxCompressorRatio, kMinCompressorRatio);
  config.knee_db = clamp_compressor_finite(knee_db, 0.0F, kMaxCompressorKneeDb, 0.0F);
  config.makeup_db = clamp_compressor_finite(makeup_db, 0.0F, kMaxCompressorMakeupDb, 0.0F);
  config.attack_frames = attack_frames;
  config.release_frames = release_frames;
  config.enabled = true;

  // A zero time constant means "instant", and gets there honestly: -1/0 is
  // -infinity and exp(-infinity) is 0, which is the correct limit. Written out
  // rather than left to the FPU so that it reads as a decision.
  config.attack_coeff =
      attack_frames == 0 ? 0.0F
                         : static_cast<float>(std::exp(-1.0 / static_cast<double>(attack_frames)));
  config.release_coeff =
      release_frames == 0
          ? 0.0F
          : static_cast<float>(std::exp(-1.0 / static_cast<double>(release_frames)));

  config.makeup_gain =
      static_cast<float>(std::pow(10.0, static_cast<double>(config.makeup_db) / 20.0));
  config.knee_low_linear =
      static_cast<float>(std::pow(10.0, (static_cast<double>(config.threshold_db) -
                                         (0.5 * static_cast<double>(config.knee_db))) /
                                            20.0));
  return config;
}

// GAIN REDUCTION in dB -- the amount the curve subtracts, not the output level.
// Zero or negative; zero means no reduction.
//
// This is the primitive rather than the output level, and the difference is not
// cosmetic. docs/MIXER.md writes the curve as `y = T + (x - T)/R` above the
// knee, and the gain applied is `10^((y - x)/20)`. Computing y and then
// subtracting x has two problems in float that the algebra does not have:
//
//  - AT R = 1 IT IS NOT UNITY. `T + (x - T)` does not round back to `x`, so a
//    ratio of 1 -- which the spec calls "unity gain at every level", and which
//    is the reason the enabled flag can be an optimisation rather than a second
//    code path -- came out at 0.99999976. Measured, not feared. Written as a
//    reduction the `(1 - 1/R)` factor is exactly zero and the whole expression
//    vanishes, so R = 1 is exact for every threshold and every knee width.
//  - It differences two numbers that are nearly equal whenever reduction is
//    small, which is precisely when the answer needs its precision.
//
// Algebraically identical: T + (x-T)/R - x == -(x-T)(1 - 1/R).
[[nodiscard]] inline float compressor_reduction_db(float input_db, float threshold_db, float ratio,
                                                   float knee_db) noexcept {
  const float over = input_db - threshold_db;
  const float half = 0.5F * knee_db;

  if (knee_db > 0.0F && over > -half && over <= half) {
    const float into = over + half;
    return ((1.0F / ratio) - 1.0F) * into * into / (2.0F * knee_db);
  }
  if (over > 0.0F) {
    return -over * (1.0F - (1.0F / ratio));
  }
  return 0.0F;
}

// The static gain curve as docs/MIXER.md writes it: the OUTPUT level in dB.
//
// One line, so there is one source of truth and the two forms cannot drift.
// Kept because the spec is written this way and the acceptance is stated against
// it; the audio path uses the reduction above.
//
// The quadratic knee is continuous in VALUE AND FIRST DERIVATIVE at both
// boundaries, with the slope interpolating linearly from 1 to 1/R across it.
// Verified: a knee that is merely continuous still produces an audible edge, and
// the edge lives in the derivative.
[[nodiscard]] inline float compressor_curve_db(float input_db, float threshold_db, float ratio,
                                               float knee_db) noexcept {
  return input_db + compressor_reduction_db(input_db, threshold_db, ratio, knee_db);
}

// One strip's compressor: the envelope follower, and nothing else.
//
// ONE PER STRIP, not one per channel, because the detector is the maximum across
// channels. Two envelopes would be two different gains, which is exactly the
// stereo-image problem the shared detector exists to avoid.
class Compressor {
 public:
  // AUDIO THREAD. Advances the envelope by one frame and returns the linear gain
  // to apply to every channel of that frame.
  [[nodiscard]] float process(const CompressorConfig& config, float detector) noexcept {
    // Attack when the signal is rising, release when it is falling. The compare
    // is against the PREVIOUS envelope, which is what makes a one-pole
    // asymmetric rather than just slow.
    const float coeff = detector > m_envelope ? config.attack_coeff : config.release_coeff;
    m_envelope = flush_denormal((coeff * (m_envelope - detector)) + detector);
    return gain_for(config, m_envelope);
  }

  void reset() noexcept { m_envelope = 0.0F; }

  [[nodiscard]] float envelope() const noexcept { return m_envelope; }

  // The linear gain this configuration applies to a settled level. Static: it
  // depends only on the envelope, which is what lets the curve be tested apart
  // from the detector.
  [[nodiscard]] static float gain_for(const CompressorConfig& config, float envelope) noexcept {
    // The quiet path. Below the knee the curve is y = x, so the gain is exactly
    // the makeup and no transcendental is needed -- which matters because this
    // is the branch nearly every frame of nearly every strip takes.
    if (envelope <= config.knee_low_linear) {
      return config.makeup_gain;
    }

    const float input_db = 20.0F * std::log10(envelope);
    const float reduction_db =
        compressor_reduction_db(input_db, config.threshold_db, config.ratio, config.knee_db);

    // Exactly zero reduction is exactly unity, without going near pow(). Not
    // only an optimisation: it is what makes a ratio of 1 bit-transparent rather
    // than nearly so, and pow(10, -0.0f/20) is a call whose result nobody should
    // have to reason about.
    if (reduction_db == 0.0F) {
      return config.makeup_gain;
    }

    // log10 and pow in the callback. Legal -- they allocate nothing, lock
    // nothing and cannot throw -- and correct, which is what this milestone is
    // for. If M8's perf pass replaces them with a table, the acceptance in
    // tests/unit/compressor_test.cpp is the budget that table has to fit.
    return config.makeup_gain * std::pow(10.0F, reduction_db / 20.0F);
  }

 private:
  float m_envelope = 0.0F;
};

}  // namespace rt

#endif  // CRATEDIG_RT_COMPRESSOR_HPP
