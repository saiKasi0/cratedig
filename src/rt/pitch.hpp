#ifndef CRATEDIG_RT_PITCH_HPP
#define CRATEDIG_RT_PITCH_HPP

#include <cmath>

namespace rt {

// Playback speed, in the two units people think in.
//
// A MUSICIAN THINKS IN SEMITONES AND A TAPE THINKS IN RATIO, and
// `PadConfig::pitch_ratio` is a ratio. Converting in one place beats arguing
// about it in three: the parser reads either, the interface shows both, and
// neither has its own idea of what an octave is.
//
// NOT IN THE AUDIO CALLBACK. These call `std::pow` and `std::log2`, which the
// RT rules keep out of `render()` -- the ratio is computed on the control thread
// and published inside a PadConfig like every other setting.

// What VoicePool::step_for() will actually accept. Outside this range it
// substitutes 1.0 rather than letting the phase run backwards or stop, so a
// value that reaches it out of range is silently ignored -- which is why the
// control side clamps to the same bounds and says what it did.
//
// The bottom is not zero: at 1/64 a one-second slice takes over a minute, and
// below that the interpolator is reading the same frame for long enough that it
// is a drone rather than a sample.
inline constexpr float kMinPitchRatio = 1.0F / 64.0F;
inline constexpr float kMaxPitchRatio = 64.0F;

// Twelve-tone equal temperament, because that is what "+7" means to the person
// typing it. Six octaves either way, which is the ratio range expressed in the
// other unit.
inline constexpr float kSemitonesPerOctave = 12.0F;
inline constexpr float kMinSemitones = -72.0F;
inline constexpr float kMaxSemitones = 72.0F;

[[nodiscard]] inline float clamp_pitch_ratio(float ratio) noexcept {
  if (!(ratio > 0.0F)) {
    return 1.0F;  // NaN and non-positive both land here, and both mean "unset"
  }
  if (ratio < kMinPitchRatio) {
    return kMinPitchRatio;
  }
  if (ratio > kMaxPitchRatio) {
    return kMaxPitchRatio;
  }
  return ratio;
}

[[nodiscard]] inline float ratio_from_semitones(float semitones) noexcept {
  return clamp_pitch_ratio(std::pow(2.0F, semitones / kSemitonesPerOctave));
}

[[nodiscard]] inline float semitones_from_ratio(float ratio) noexcept {
  return kSemitonesPerOctave * std::log2(clamp_pitch_ratio(ratio));
}

}  // namespace rt

#endif  // CRATEDIG_RT_PITCH_HPP
