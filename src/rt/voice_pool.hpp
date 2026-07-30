#ifndef CRATEDIG_RT_VOICE_POOL_HPP
#define CRATEDIG_RT_VOICE_POOL_HPP

#include "rt/interpolator.hpp"
#include "rt/sample.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace rt {

// Playback position is 32.32 fixed point, not a float or a double.
//
// This is the decision the determinism contract rests on. A float phase
// accumulator drifts as the integer part grows and the exponent eats the
// fractional bits, so the same sample played from the same place produces
// different frames depending on how many blocks it took to get there — which
// makes "render N frames as one block == render them as 375 blocks" a property
// that holds by luck and fails once the file is long enough. Integer addition
// has none of that: the accumulator is exact, so block-size invariance holds by
// construction rather than by tolerance.
//
// 32 fractional bits give a step resolution of ~2.3e-10 frames; over a
// ten-minute 48 kHz file the accumulated position error is exactly zero.
using PhaseFixed = std::uint64_t;

inline constexpr std::uint32_t kPhaseFractionBits = 32;
inline constexpr PhaseFixed kPhaseOne = PhaseFixed{1} << kPhaseFractionBits;
inline constexpr PhaseFixed kPhaseFractionMask = kPhaseOne - 1;

// Scales a 32-bit fraction into [0, 1). Exact for the top 24 bits, which is all
// a float interpolation coefficient can hold anyway.
inline constexpr float kPhaseToFloat = 1.0F / 4'294'967'296.0F;

// One sounding note.
//
// Holds a shared_ptr rather than a raw pointer so a sample cannot be freed while
// a voice is still reading it. That ownership is exactly why voices must retire
// through a GarbageRing: dropping the last reference here would run the deleter
// on the audio thread.
struct Voice {
  std::shared_ptr<const Sample> sample;
  PhaseFixed phase = 0;
  PhaseFixed phase_step = kPhaseOne;
  float gain = 0.0F;

  // Trigger sequence number. Voice stealing picks the smallest, which makes
  // "steal the oldest" an exact rule rather than "whichever we happen to find".
  std::uint64_t started_at = 0;

  // Loudest absolute value this voice contributed during the last block, on the
  // first output channel. Written by render_voice, read by the engine when it
  // publishes telemetry -- never by the DSP, so it cannot affect the audio and
  // is therefore not part of the determinism contract.
  float peak = 0.0F;

  // Which pad started this voice. Carried so the engine can attribute level and
  // playhead back to a pad without a side table; M3's choke groups need the same
  // answer, and adding it then would mean touching every trigger path twice.
  std::uint8_t pad = 0;

  // Producing audio. A voice that has run off the end of its sample clears this
  // but keeps `sample` until the GarbageRing accepts it — see reclaim().
  bool active = false;
};

// Fixed-size pool of voices. Allocation-free, lock-free, deterministic.
//
// Every operation here runs on the audio thread. There is no locking because
// there is no sharing: the control thread reaches this only through the
// PadEvent ring.
template <std::size_t Capacity>
class VoicePool {
  static_assert(Capacity > 0, "VoicePool: need at least one voice");

 public:
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  // AUDIO THREAD. Starts `sample` on a free voice, stealing the oldest if the
  // pool is full.
  //
  // `garbage` receives any sample displaced from the reused slot. It is
  // templated rather than a GarbageRing<N> so the pool does not have to know the
  // ring's capacity, and so tests can substitute a counting sink.
  //
  // Returns false when the trigger could not be honoured — which happens only if
  // every voice is busy AND the garbage ring is full, i.e. the janitor has
  // stalled. Dropping the hit is the correct outcome there: the alternative is
  // releasing a reference on the audio thread.
  template <typename GarbageSink>
  bool trigger(const std::shared_ptr<const Sample>& sample, float gain,
               std::uint32_t engine_sample_rate, GarbageSink& garbage,
               std::uint8_t pad = 0) noexcept {
    if (sample == nullptr || sample->empty() || engine_sample_rate == 0) {
      return false;
    }

    Voice* slot = acquire_slot(sample, garbage);
    if (slot == nullptr) {
      return false;
    }

    // The slot is now either empty or already holding this exact sample.
    // Copying the shared_ptr is an atomic increment: no allocation, no lock, and
    // because the slot is empty it cannot release a reference on this thread.
    assert((slot->sample == nullptr || slot->sample == sample) &&
           "VoicePool: reusing a slot that still owns a different sample");
    if (slot->sample == nullptr) {
      slot->sample = sample;
    }
    slot->phase = 0;
    slot->phase_step = step_for(*sample, engine_sample_rate);
    slot->gain = gain;
    slot->started_at = m_next_sequence++;
    slot->peak = 0.0F;
    slot->pad = pad;
    slot->active = true;
    return true;
  }

  // AUDIO THREAD. Mixes every active voice into `channels`, additively.
  //
  // The caller has already cleared the buffers. Adding rather than assigning is
  // what lets the engine keep the "zero, then accumulate" shape it will need
  // once there is more than one source.
  void render_add(std::span<float* const> channels, std::size_t num_frames) noexcept {
    if (channels.empty() || num_frames == 0) {
      return;
    }

    for (Voice& voice : m_voices) {
      if (!voice.active) {
        voice.peak = 0.0F;  // a silent voice must not hold a meter up
        continue;
      }
      render_voice(voice, channels, num_frames);
    }
  }

  // AUDIO THREAD. Read-only view of the pool, for publishing telemetry.
  //
  // Const rather than a bespoke accessor per consumer: the engine already needs
  // pad, peak, phase and active together to publish a playhead and pad meters,
  // and M3's choke groups and M5's per-strip metering want the same walk.
  [[nodiscard]] std::span<const Voice> voices() const noexcept { return m_voices; }

  // AUDIO THREAD, once per block after rendering. Hands finished voices' samples
  // to the janitor.
  //
  // Separate from render_add() because it can fail: if the ring is full the
  // voice keeps its reference and this is retried next block. A voice in that
  // state is silent but not yet reusable, which is the honest representation —
  // the alternative is dropping the pointer here and destroying a Sample on the
  // audio thread, the one thing GarbageRing exists to prevent.
  template <typename GarbageSink>
  std::size_t reclaim(GarbageSink& garbage) noexcept {
    std::size_t reclaimed = 0;
    for (Voice& voice : m_voices) {
      if (voice.active || voice.sample == nullptr) {
        continue;
      }
      if (garbage.retire(std::move(voice.sample))) {
        ++reclaimed;
      } else {
        // retire() left the pointer with us on failure. Put it back so the slot
        // stays non-free and we try again next block.
        assert(voice.sample != nullptr && "GarbageRing::retire must not consume on failure");
      }
    }
    return reclaimed;
  }

  [[nodiscard]] std::size_t active_count() const noexcept {
    std::size_t count = 0;
    for (const Voice& voice : m_voices) {
      count += voice.active ? 1U : 0U;
    }
    return count;
  }

  // Voices that have finished but still hold a sample the janitor has not taken.
  // Persistently non-zero means the garbage ring is undersized or the janitor
  // is not running.
  [[nodiscard]] std::size_t pending_reclaim_count() const noexcept {
    std::size_t count = 0;
    for (const Voice& voice : m_voices) {
      count += (!voice.active && voice.sample != nullptr) ? 1U : 0U;
    }
    return count;
  }

  [[nodiscard]] std::uint64_t triggers_started() const noexcept { return m_next_sequence; }

 private:
  [[nodiscard]] static PhaseFixed step_for(const Sample& sample,
                                           std::uint32_t engine_sample_rate) noexcept {
    // Deterministic: the same two integers always produce the same step, so the
    // same trigger always plays the same frames.
    const double ratio =
        static_cast<double>(sample.sample_rate()) / static_cast<double>(engine_sample_rate);
    return static_cast<PhaseFixed>(ratio * static_cast<double>(kPhaseOne));
  }

  [[nodiscard]] Voice* find_free_slot() noexcept {
    // Lowest free index, never "the first one we notice": voice identity has to
    // be reproducible for the offline renderer to match the live one.
    for (Voice& voice : m_voices) {
      if (!voice.active && voice.sample == nullptr) {
        return &voice;
      }
    }
    return nullptr;
  }

  // Returns a slot that is either empty or already owns `sample`, or nullptr if
  // no slot can be made available without breaking the no-destruction rule.
  template <typename GarbageSink>
  [[nodiscard]] Voice* acquire_slot(const std::shared_ptr<const Sample>& sample,
                                    GarbageSink& garbage) noexcept {
    if (Voice* free_slot = find_free_slot(); free_slot != nullptr) {
      return free_slot;
    }

    Voice* oldest = nullptr;
    for (Voice& voice : m_voices) {
      if (!voice.active) {
        continue;
      }
      if (oldest == nullptr || voice.started_at < oldest->started_at) {
        oldest = &voice;
      }
    }
    if (oldest == nullptr) {
      return nullptr;
    }

    // Retriggering a pad that is already sounding is the single most common
    // thing anyone does with a sampler — a rolled hi-hat, a stuttered kick. When
    // the stolen voice already holds this exact sample there is nothing to hand
    // the janitor: reusing the reference in place means zero refcount traffic
    // and, more importantly, zero garbage-ring pressure. Retiring here instead
    // would let a fast roll fill the ring and start dropping hits.
    if (oldest->sample == sample) {
      oldest->active = false;
      return oldest;
    }

    // Different sample: the displaced one must reach the janitor before the slot
    // is reused. If the ring is full we cannot steal, because the only way to
    // proceed would be to drop the reference here — on the audio thread.
    if (!garbage.retire(std::move(oldest->sample))) {
      return nullptr;
    }
    oldest->active = false;
    return oldest;
  }

  static void render_voice(Voice& voice, std::span<float* const> channels,
                           std::size_t num_frames) noexcept {
    const Sample& sample = *voice.sample;
    const std::size_t sample_frames = sample.num_frames();
    const std::uint16_t sample_channels = sample.num_channels();
    const std::size_t out_channels = channels.size();

    // Tracked locally and stored once, rather than through voice.peak on every
    // frame: this is a register, that is a memory round-trip in the innermost
    // loop of the audio callback.
    float block_peak = 0.0F;

    for (std::size_t frame = 0; frame < num_frames; ++frame) {
      const auto index = static_cast<std::size_t>(voice.phase >> kPhaseFractionBits);
      if (index >= sample_frames) {
        voice.active = false;
        voice.peak = block_peak;
        return;
      }

      const auto fraction = static_cast<float>(voice.phase & kPhaseFractionMask) * kPhaseToFloat;
      const auto offset = static_cast<std::ptrdiff_t>(index);

      for (std::size_t channel = 0; channel < out_channels; ++channel) {
        // Mono feeds every output channel; a sample with fewer channels than the
        // engine has repeats its last one rather than leaving silence. No branch
        // on channel count in the caller, and no allocation to build a mapping.
        const auto source_channel = static_cast<std::uint16_t>(
            channel < sample_channels ? channel : (sample_channels - 1U));
        const float* frame0 = sample.frame0(source_channel);

        // The guard frames are what make these four reads safe at index 0 and at
        // sample_frames - 1 without a bounds check here.
        const float value = hermite4(frame0[offset - 1], frame0[offset], frame0[offset + 1],
                                     frame0[offset + 2], fraction);
        const float scaled = voice.gain * value;
        channels[channel][frame] += scaled;

        // Channel 0 only. A pad meter is one bar, so metering every channel
        // would double the work in the hottest loop in the program to produce a
        // number nothing displays.
        if (channel == 0) {
          const float magnitude = scaled < 0.0F ? -scaled : scaled;
          block_peak = magnitude > block_peak ? magnitude : block_peak;
        }
      }

      voice.phase += voice.phase_step;
    }
    voice.peak = block_peak;
  }

  std::array<Voice, Capacity> m_voices{};
  std::uint64_t m_next_sequence = 0;
};

}  // namespace rt

#endif  // CRATEDIG_RT_VOICE_POOL_HPP
