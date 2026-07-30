#ifndef CRATEDIG_RT_SAMPLE_HPP
#define CRATEDIG_RT_SAMPLE_HPP

#include "rt/interpolator.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rt {

// A decoded piece of audio, ready to play.
//
// Built by a worker (src/ingest/), published as std::shared_ptr<const Sample>,
// and thereafter read — never written — by the audio thread. Construction
// allocates; that is fine and expected, because it never happens on the audio
// thread. Nothing in the read path allocates, locks, or branches on anything but
// its arguments.
//
// Layout is planar (channel 0's frames, then channel 1's, ...), not interleaved:
// a voice reads one channel at a time, and the mixer graph downstream is
// per-channel too, so interleaving would only add a stride to every access.
//
// GUARD PADDING
// -------------
// Each channel is stored with kGuardBefore zeroed frames in front and
// kGuardAfter behind. The interpolator reads x[i-1] .. x[i+2], so for i at
// either end of the sample it would otherwise read out of bounds. The
// alternatives are a bounds check in the innermost loop of the audio callback,
// or clamping index arithmetic on every tap; padding costs three frames per
// channel, once, at load time, and removes the question entirely.
//
// The guards are zero rather than edge-clamped so that a sample fades to silence
// at its boundaries instead of holding its first/last value, which would be an
// audible click.
class Sample {
 public:
  // Sized from what the interpolator actually reads, so changing the kernel is a
  // compile error here rather than a silent out-of-bounds read.
  static constexpr std::size_t kGuardBefore = kHermiteTapsBefore;
  static constexpr std::size_t kGuardAfter = kHermiteTapsAfter;

  Sample() = default;

  // Allocates zeroed storage. The caller then fills each channel through
  // mutable_channel() before publishing — see the note on that function.
  Sample(std::uint32_t sample_rate, std::uint16_t num_channels, std::size_t num_frames)
      : m_sample_rate(sample_rate),
        m_num_channels(num_channels),
        m_num_frames(num_frames),
        m_stride(num_frames + kGuardBefore + kGuardAfter),
        m_storage(m_stride * num_channels, 0.0F) {
    assert(num_channels > 0 && "Sample: a sample needs at least one channel");
  }

  // Move-only. Copying a Sample means copying every frame of audio, which is
  // never what the caller meant on any thread — the sharing mechanism is
  // shared_ptr<const Sample>.
  Sample(const Sample&) = delete;
  Sample& operator=(const Sample&) = delete;
  Sample(Sample&&) noexcept = default;
  Sample& operator=(Sample&&) noexcept = default;
  ~Sample() = default;

  [[nodiscard]] std::uint32_t sample_rate() const noexcept { return m_sample_rate; }
  [[nodiscard]] std::uint16_t num_channels() const noexcept { return m_num_channels; }
  [[nodiscard]] std::size_t num_frames() const noexcept { return m_num_frames; }
  [[nodiscard]] bool empty() const noexcept { return m_num_frames == 0; }

  // WORKER THREAD ONLY, before the Sample is published.
  //
  // There is no builder type because the only writer is the decoder that just
  // created the object. Once it becomes a shared_ptr<const Sample>, const-ness
  // makes this inaccessible, which is the actual enforcement — the comment is
  // only here to explain why that is sufficient.
  [[nodiscard]] std::span<float> mutable_channel(std::uint16_t channel) noexcept {
    assert(channel < m_num_channels && "Sample: channel out of range");
    return std::span<float>{m_storage.data() + channel_offset(channel), m_num_frames};
  }

  // The audible frames of one channel, without the guards.
  [[nodiscard]] std::span<const float> channel(std::uint16_t channel_index) const noexcept {
    assert(channel_index < m_num_channels && "Sample: channel out of range");
    return std::span<const float>{m_storage.data() + channel_offset(channel_index), m_num_frames};
  }

  // AUDIO THREAD. A pointer to frame 0 of a channel, valid to index from
  // -kGuardBefore through num_frames() + kGuardAfter - 1. That negative-index
  // validity is the whole point: the interpolator can read frame0[i - 1] at
  // i == 0 without a branch.
  [[nodiscard]] const float* frame0(std::uint16_t channel_index) const noexcept {
    assert(channel_index < m_num_channels && "Sample: channel out of range");
    return m_storage.data() + channel_offset(channel_index);
  }

 private:
  [[nodiscard]] std::size_t channel_offset(std::uint16_t channel_index) const noexcept {
    return (static_cast<std::size_t>(channel_index) * m_stride) + kGuardBefore;
  }

  std::uint32_t m_sample_rate = 0;
  std::uint16_t m_num_channels = 0;
  std::size_t m_num_frames = 0;
  std::size_t m_stride = 0;
  std::vector<float> m_storage;
};

}  // namespace rt

#endif  // CRATEDIG_RT_SAMPLE_HPP
