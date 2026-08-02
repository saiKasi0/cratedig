#ifndef CRATEDIG_RT_LIMITER_HPP
#define CRATEDIG_RT_LIMITER_HPP

#include "rt/arch.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace rt {

// The longest lookahead the delay line is sized for. 256 frames is 5.3 ms at
// 48 kHz, four times the default and well past the point where the delay stops
// being inaudible.
//
// A ceiling rather than a setting: the delay line is allocated once, on the
// control thread, and the audio thread clamps to what was allocated. A limiter
// that could grow its own buffer would be a limiter that allocates in the
// callback.
inline constexpr std::size_t kMaxLookaheadFrames = 256;

// ~1.3 ms at 48 kHz. Long enough that gain reduction is fully applied before a
// transient arrives, short enough that the delay does not smear the transient
// the limiter exists to catch.
inline constexpr std::size_t kDefaultLookaheadFrames = 64;

// 50 ms at 48 kHz. Slow enough not to modulate bass into distortion, fast
// enough that one loud hit does not duck the following bar.
inline constexpr std::size_t kDefaultLimiterReleaseFrames = 2'400;

// -0.3 dBFS. Below full scale because an inter-sample peak can exceed the
// sample peak, and because some converters clip at exactly 0.
inline constexpr float kDefaultCeilingDb = -0.3F;

inline constexpr float kMinCeilingDb = -24.0F;
inline constexpr float kMaxCeilingDb = 0.0F;

struct LimiterConfig {
  // Derived on the control thread.
  float ceiling_linear = 1.0F;
  float release_coeff = 0.0F;

  float ceiling_db = kDefaultCeilingDb;
  std::size_t release_frames = kDefaultLimiterReleaseFrames;
  std::size_t lookahead_frames = kDefaultLookaheadFrames;

  // OFF, and that is a decision rather than an oversight (docs/MIXER.md).
  // Lookahead is a delay, and a delay that is always on would move every
  // committed hash in the project for a feature nobody asked to enable.
  bool enabled = false;
};

// CONTROL THREAD.
[[nodiscard]] inline LimiterConfig make_limiter(
    float ceiling_db, std::size_t release_frames = kDefaultLimiterReleaseFrames,
    std::size_t lookahead_frames = kDefaultLookaheadFrames) noexcept {
  LimiterConfig config;
  config.ceiling_db = std::isfinite(ceiling_db)
                          ? std::min(std::max(ceiling_db, kMinCeilingDb), kMaxCeilingDb)
                          : kDefaultCeilingDb;
  config.release_frames = release_frames;
  config.lookahead_frames = std::min(lookahead_frames, kMaxLookaheadFrames);
  config.enabled = true;
  config.ceiling_linear =
      static_cast<float>(std::pow(10.0, static_cast<double>(config.ceiling_db) / 20.0));
  config.release_coeff =
      release_frames == 0
          ? 0.0F
          : static_cast<float>(std::exp(-1.0 / static_cast<double>(release_frames)));
  return config;
}

// The master limiter: a brick wall with lookahead.
//
// WHY THE BOUND HOLDS:
//
//   output[n] = g[n] * x[n - L]
//   g[n]     <= ceiling / max(|x[n - L]| .. |x[n]|)
//
// The detector's window is the SAME window the delay line holds, and it includes
// the sample about to be emitted. So the sample leaving the delay line is one
// the detector has already accounted for, and the gain that meets it is already
// low enough. Attack is instant -- the gain jumps straight to the target -- and
// the released gain is clamped to the target every frame, so no amount of
// rounding on the way up can let it drift above what the bound allows. Both of
// those are load-bearing: removing either one lets samples through.
//
// WHAT LOOKAHEAD IS AND IS NOT FOR. It is NOT what makes the bound possible.
// This comment used to say a feed-forward limiter without lookahead "cannot make
// the guarantee at all", and that is simply false: with instant attack on a
// detector that includes the current sample, |g*x| <= ceiling holds at L = 0
// too. Deleting the lookahead and re-running the acceptance passes it, which is
// how the claim was caught.
//
// What lookahead buys is WHERE THE GAIN STEP LANDS. Reduction is instantaneous,
// so without lookahead the step coincides exactly with the transient that caused
// it -- a discontinuity applied to a loud sample, which is a step in the
// waveform and audible as distortion. With L frames of lookahead the same step
// is applied L frames earlier, to the near-silence in front of the transient,
// and by the time the peak emerges the gain has been settled for L frames.
//
// Release is the same one-pole the compressor uses.
class Limiter {
 public:
  // CONTROL THREAD, once. Allocates the delay line.
  void prepare(std::size_t channels) {
    m_channels = channels;
    m_stride = kMaxLookaheadFrames + 1;
    m_delay.assign(channels * m_stride, 0.0F);
    reset();
  }

  void reset() noexcept {
    std::fill(m_delay.begin(), m_delay.end(), 0.0F);
    m_write = 0;
    m_gain = 1.0F;
  }

  [[nodiscard]] float gain() const noexcept { return m_gain; }

  // AUDIO THREAD. In place, at master.
  void process(const LimiterConfig& config, std::span<float* const> channels,
               std::size_t num_frames) noexcept {
    if (m_delay.empty() || channels.empty()) {
      return;
    }
    const std::size_t count = std::min(channels.size(), m_channels);
    const std::size_t lookahead = std::min(config.lookahead_frames, kMaxLookaheadFrames);
    const std::size_t window = lookahead + 1;

    for (std::size_t frame = 0; frame < num_frames; ++frame) {
      for (std::size_t channel = 0; channel < count; ++channel) {
        m_delay[(channel * m_stride) + m_write] = channels[channel][frame];
      }

      // The peak over the window, across every channel: one gain for all of
      // them, so a limiter cannot pull the stereo image sideways any more than
      // the compressor can.
      //
      // Scanned rather than maintained incrementally. A monotonic deque would be
      // O(1) instead of O(lookahead), and at 64 frames of window this is a few
      // hundred thousand comparisons a second on ONE node in the graph. M8 is
      // the perf pass; measure there rather than adding a second data structure
      // to get wrong now.
      const std::size_t read = (m_write + m_stride - lookahead) % m_stride;
      float peak = 0.0F;
      for (std::size_t channel = 0; channel < count; ++channel) {
        const float* line = m_delay.data() + (channel * m_stride);
        std::size_t index = read;
        for (std::size_t step = 0; step < window; ++step) {
          const float value = line[index];
          peak = std::max(peak, value < 0.0F ? -value : value);
          index = (index + 1 == m_stride) ? 0 : index + 1;
        }
      }

      const float target = peak > config.ceiling_linear ? config.ceiling_linear / peak : 1.0F;
      if (target < m_gain) {
        m_gain = target;  // instant attack: the bound is not negotiable
      } else {
        // One-pole release toward the target, then clamped to it. The clamp is
        // arithmetic paranoia rather than logic -- a one-pole approaching from
        // below cannot mathematically overshoot -- and it is what makes the
        // guarantee hold in float rather than only in algebra.
        m_gain = std::min((config.release_coeff * (m_gain - target)) + target, target);
      }

      for (std::size_t channel = 0; channel < count; ++channel) {
        channels[channel][frame] = m_gain * m_delay[(channel * m_stride) + read];
      }

      m_write = (m_write + 1 == m_stride) ? 0 : m_write + 1;
    }
  }

 private:
  std::vector<float> m_delay;
  std::size_t m_channels = 0;
  std::size_t m_stride = 0;
  std::size_t m_write = 0;
  float m_gain = 1.0F;
};

}  // namespace rt

#endif  // CRATEDIG_RT_LIMITER_HPP
