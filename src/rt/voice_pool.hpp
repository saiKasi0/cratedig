#ifndef CRATEDIG_RT_VOICE_POOL_HPP
#define CRATEDIG_RT_VOICE_POOL_HPP

#include "rt/envelope.hpp"
#include "rt/interpolator.hpp"
#include "rt/pad_config.hpp"
#include "rt/sample.hpp"

#include <algorithm>
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
// Holds a shared_ptr rather than a raw pointer so nothing it reads can be freed
// underneath it. That ownership is exactly why voices must retire through a
// GarbageRing: dropping the last reference here would run the deleter on the
// audio thread.
//
// It holds the PadConfig, not the Sample. A voice reads the slice bounds, the
// envelope spec, the gain and the tuning as well as the audio, and after a live
// reassignment the pad's config pointer has already moved on -- so the voice has
// to own the whole thing it is playing, not just the buffer. The Sample stays
// alive transitively, because the config holds it.
struct Voice {
  std::shared_ptr<const PadConfig> config;
  PhaseFixed phase = 0;
  PhaseFixed phase_step = kPhaseOne;
  float gain = 0.0F;

  Envelope env;

  // Resolved once at trigger, so the inner loop never re-derives them from the
  // config or re-clamps against the sample length.
  std::size_t start_frame = 0;
  std::size_t end_frame = 0;
  std::size_t fade_in = 0;
  std::size_t fade_out = 0;
  float inv_fade_in = 0.0F;
  float inv_fade_out = 0.0F;

  // Trigger sequence number. Voice stealing picks the smallest, which makes
  // "steal the oldest" an exact rule rather than "whichever we happen to find".
  std::uint64_t started_at = 0;

  // Loudest absolute value this voice contributed during the last block, on the
  // first output channel. Written by render_voice, read by the engine when it
  // publishes telemetry -- never by the DSP, so it cannot affect the audio and
  // is therefore not part of the determinism contract.
  float peak = 0.0F;

  // Which pad started this voice. Carried alongside config->pad so telemetry and
  // choke can read it without dereferencing, and so a voice keeps its
  // attribution even as the pad's config moves on underneath it.
  std::uint8_t pad = 0;

  // Captured at trigger, like every other per-voice setting: a pad flipped to
  // reverse while a hit is ringing must not turn that hit around mid-flight.
  bool reverse = false;

  // Frames into the NEXT block before this voice begins sounding.
  //
  // This is what makes a trigger sample-accurate rather than block-quantised: a
  // hit is placed where it belongs inside the block instead of at whichever
  // boundary happened to come first. Consumed by the first render_add() after
  // the trigger and zero thereafter -- it describes one block, not the voice.
  //
  // Implemented as a starting index for the render loop rather than by splitting
  // the block into sub-spans. Costs nothing per frame, and the envelope and the
  // declick fades line up for free because both advance per RENDERED frame: skip
  // the frame and you skip its envelope step too, which is exactly right.
  //
  // An offset at or past the block length leaves nothing to render this block
  // and starts the voice at the beginning of the next one. The engine clamps
  // before it gets here, so that is a floor rather than a behaviour to rely on.
  std::size_t start_offset = 0;

  // Producing audio. A voice that has run off the end of its slice clears this
  // but keeps `config` until the GarbageRing accepts it — see reclaim().
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

  // AUDIO THREAD. Starts `config`'s slice on a free voice, stealing the oldest
  // if the pool is full, and chokes anything else in the same choke group.
  //
  // `garbage` receives any config displaced from the reused slot. It is
  // templated rather than a GarbageRing<N> so the pool does not have to know the
  // ring's capacity, and so tests can substitute a counting sink.
  //
  // Returns false when the trigger could not be honoured — which happens only if
  // every voice is busy AND the garbage ring is full, i.e. the janitor has
  // stalled. Dropping the hit is the correct outcome there: the alternative is
  // releasing a reference on the audio thread.
  //
  // `frame_offset` places the hit inside the next block — see Voice::start_offset.
  // It defaults to zero, which is where every trigger landed before M4 and is
  // still where a live keypress lands: a key that was pressed between two blocks
  // has no more precise time than "the start of the next one".
  template <typename GarbageSink>
  bool trigger(const std::shared_ptr<const PadConfig>& config, float velocity,
               std::uint32_t engine_sample_rate, GarbageSink& garbage,
               std::size_t frame_offset = 0) noexcept {
    if (config == nullptr || config->sample == nullptr || config->sample->empty() ||
        engine_sample_rate == 0) {
      return false;
    }
    const Sample& sample = *config->sample;

    // Resolved before a slot is taken, so a config describing an empty slice
    // cannot leave a stolen voice silent and un-stealable.
    const std::size_t frames = sample.num_frames();
    const std::size_t start = std::min(config->start_frame, frames);
    const std::size_t end = config->end_frame == 0 ? frames : std::min(config->end_frame, frames);
    if (end <= start) {
      return false;
    }

    Voice* slot = acquire_slot(config, garbage);
    if (slot == nullptr) {
      return false;
    }

    // The slot is now either empty or already holding this exact config.
    // Copying the shared_ptr is an atomic increment: no allocation, no lock, and
    // because the slot is empty it cannot release a reference on this thread.
    assert((slot->config == nullptr || slot->config == config) &&
           "VoicePool: reusing a slot that still owns a different config");
    if (slot->config == nullptr) {
      slot->config = config;
    }

    const std::size_t length = end - start;
    // Half the slice each at most, so the two fades can never overlap and a
    // one-frame slice cannot divide by a clamped-to-zero length.
    const std::size_t half = length / 2;
    slot->fade_in = std::min(config->fade_in_frames, half);
    slot->fade_out = std::min(config->fade_out_frames, half);
    slot->inv_fade_in = slot->fade_in == 0 ? 0.0F : 1.0F / static_cast<float>(slot->fade_in);
    slot->inv_fade_out = slot->fade_out == 0 ? 0.0F : 1.0F / static_cast<float>(slot->fade_out);

    slot->start_frame = start;
    slot->end_frame = end;
    slot->phase = static_cast<PhaseFixed>(start) << kPhaseFractionBits;
    slot->phase_step = step_for(sample, engine_sample_rate, config->pitch_ratio);
    slot->gain = velocity * config->gain;
    slot->started_at = m_next_sequence++;
    slot->peak = 0.0F;
    slot->pad = config->pad;
    slot->reverse = config->reverse;
    slot->start_offset = frame_offset;
    slot->env.trigger(config->env);
    slot->active = true;

    // After the new voice is live, and skipping it by identity rather than by
    // pad: retriggering a pad that chokes its own group must cut the previous
    // hit, which is exactly what a closed hi-hat does to itself.
    choke_group(config->choke_group, slot);
    return true;
  }

  // AUDIO THREAD. The player let go of a pad.
  //
  // Only gate voices care. A one-shot ignoring note-off is the definition of
  // one-shot, so it lives here rather than as a condition at every call site --
  // which matters because M3's keyboard, M4's MIDI and M5's sequencer will all
  // send note-offs without knowing how the pad is configured.
  void note_off(std::uint8_t pad) noexcept {
    for (Voice& voice : m_voices) {
      if (voice.active && voice.pad == pad && voice.config != nullptr &&
          voice.config->trigger == TriggerMode::kGate) {
        voice.env.release(voice.config->release_floor_frames);
      }
    }
  }

  // AUDIO THREAD. Stops one pad, whatever its trigger mode.
  //
  // Distinct from note_off(), which a one-shot ignores by design. This is the
  // one that a one-shot must not ignore: it exists because a long sample cannot
  // otherwise be stopped once it is playing, which is the whole complaint M4.5
  // is answering.
  void stop_pad(std::uint8_t pad) noexcept {
    for (Voice& voice : m_voices) {
      if (voice.active && voice.pad == pad && voice.config != nullptr) {
        voice.env.release(voice.config->release_floor_frames);
      }
    }
  }

  // AUDIO THREAD. Stops everything. The panic.
  //
  // Released rather than cut, so sixteen voices ending at once is silence rather
  // than sixteen clicks in the same frame -- which is the loudest artefact this
  // program could make, and would arrive at the exact moment somebody was trying
  // to make it stop.
  void stop_all() noexcept {
    for (Voice& voice : m_voices) {
      if (voice.active && voice.config != nullptr) {
        voice.env.release(voice.config->release_floor_frames);
      }
    }
  }

  // AUDIO THREAD. Releases every sounding voice in `group`, except `keep`.
  //
  // Group 0 means "not in a group" and chokes nothing -- otherwise every
  // unconfigured pad would cut every other one, which is a spectacular default.
  void choke_group(std::uint8_t group, const Voice* keep) noexcept {
    if (group == 0) {
      return;
    }
    for (Voice& voice : m_voices) {
      if (&voice == keep || !voice.active || voice.config == nullptr) {
        continue;
      }
      if (voice.config->choke_group == group) {
        voice.env.release(voice.config->release_floor_frames);
      }
    }
  }

  // AUDIO THREAD. Mixes every active voice into `channels`, additively.
  //
  // The caller has already cleared the buffers. Adding rather than assigning is
  // what lets the engine keep the "zero, then accumulate" shape it will need
  // once there is more than one source.
  //
  // THE ENGINE NO LONGER CALLS THIS. Since M5 it renders pad by pad into strip
  // buffers (render_pad below), because the mixer needs each pad's contribution
  // on its own before the strips sum. This is kept, and is not dead code: it is
  // the *pre-mixer signal path*, and the regrouping test in
  // tests/unit/engine_render_test.cpp measures the graph against it. Float
  // addition is not associative, so moving to per-strip summation changed the
  // committed e2e hashes; the justification for that change is that these two
  // agree to within a ULP-scale bound, and that claim needs both sides of it to
  // exist. Deleting this would leave the graph with nothing to be checked
  // against but itself.
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

  // AUDIO THREAD. Mixes one pad's active voices into `channels`, additively.
  //
  // The per-pad half of render_add(), and what the mixer graph is built on: a
  // strip is "this pad's voices", so the pool has to be able to produce exactly
  // that. Called once per pad per block.
  //
  // Sixteen passes over sixteen voices rather than one pass with a routing
  // table, deliberately. It is 256 predictable comparisons a block -- nothing,
  // against the interpolation in the inner loop -- and it keeps the question of
  // WHERE A PAD GOES in the engine, next to the buffers, instead of handing the
  // pool a mapping it would then have to be trusted to apply correctly.
  //
  // Every voice is visited by exactly one pass, because Voice::pad is set from a
  // PadConfig the engine already validated against kNumPads. That is what makes
  // it safe for this and not render_add() to be the thing that clears a finished
  // voice's meter, and the reason both halves of that invariant are asserted
  // rather than assumed.
  void render_pad(std::uint8_t pad, std::span<float* const> channels,
                  std::size_t num_frames) noexcept {
    assert(pad < kNumPads && "render_pad: pad out of range");
    if (channels.empty() || num_frames == 0) {
      return;
    }

    for (Voice& voice : m_voices) {
      assert(voice.pad < kNumPads && "render_pad: a voice with no strip would never be rendered");
      if (voice.pad != pad) {
        continue;
      }
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
      if (voice.active || voice.config == nullptr) {
        continue;
      }
      if (garbage.retire(std::move(voice.config))) {
        ++reclaimed;
      } else {
        // retire() left the pointer with us on failure. Put it back so the slot
        // stays non-free and we try again next block.
        assert(voice.config != nullptr && "GarbageRing::retire must not consume on failure");
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

  // Voices that have finished but still hold a config the janitor has not taken.
  // Persistently non-zero means the garbage ring is undersized or the janitor
  // is not running.
  [[nodiscard]] std::size_t pending_reclaim_count() const noexcept {
    std::size_t count = 0;
    for (const Voice& voice : m_voices) {
      count += (!voice.active && voice.config != nullptr) ? 1U : 0U;
    }
    return count;
  }

  [[nodiscard]] std::uint64_t triggers_started() const noexcept { return m_next_sequence; }

 private:
  [[nodiscard]] static PhaseFixed step_for(const Sample& sample, std::uint32_t engine_sample_rate,
                                           float pitch_ratio) noexcept {
    // Deterministic: the same inputs always produce the same step, so the same
    // trigger always plays the same frames. Computed in double and truncated
    // once, rather than accumulated -- see the PhaseFixed note above.
    const double rate_ratio =
        static_cast<double>(sample.sample_rate()) / static_cast<double>(engine_sample_rate);
    // A non-positive or non-finite ratio would make phase run backwards or stop,
    // and this value crossed a thread boundary. Clamped rather than asserted:
    // the audio thread does not get to abort on bad input.
    const double tuning =
        pitch_ratio > 0.0F && pitch_ratio < 64.0F ? static_cast<double>(pitch_ratio) : 1.0;
    return static_cast<PhaseFixed>(rate_ratio * tuning * static_cast<double>(kPhaseOne));
  }

  [[nodiscard]] Voice* find_free_slot() noexcept {
    // Lowest free index, never "the first one we notice": voice identity has to
    // be reproducible for the offline renderer to match the live one.
    for (Voice& voice : m_voices) {
      if (!voice.active && voice.config == nullptr) {
        return &voice;
      }
    }
    return nullptr;
  }

  // Returns a slot that is either empty or already owns `config`, or nullptr if
  // no slot can be made available without breaking the no-destruction rule.
  template <typename GarbageSink>
  [[nodiscard]] Voice* acquire_slot(const std::shared_ptr<const PadConfig>& config,
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
    // the stolen voice already holds this exact config there is nothing to hand
    // the janitor: reusing the reference in place means zero refcount traffic
    // and, more importantly, zero garbage-ring pressure. Retiring here instead
    // would let a fast roll fill the ring and start dropping hits.
    //
    // "This exact config" and not "this exact sample": after a live
    // reassignment the pad has a new config, and reusing the slot in place would
    // leave the voice reading the old slice bounds and envelope.
    if (oldest->config == config) {
      oldest->active = false;
      oldest->env.stop();
      return oldest;
    }

    // Different config: the displaced one must reach the janitor before the slot
    // is reused. If the ring is full we cannot steal, because the only way to
    // proceed would be to drop the reference here — on the audio thread.
    if (!garbage.retire(std::move(oldest->config))) {
      return nullptr;
    }
    oldest->active = false;
    oldest->env.stop();
    return oldest;
  }

  // The declick gain for one position within the slice: a linear ramp up over
  // the first fade_in frames and down over the last fade_out.
  //
  // Two compares and at most one multiply per frame, and the compares predict
  // almost perfectly -- a slice is thousands of frames long and the fades are
  // tens. The alternative, splitting the block into three regions, would mean
  // three loops to maintain and three places for the phase arithmetic to
  // disagree with itself.
  [[nodiscard]] static float declick_gain(const Voice& voice, std::size_t index) noexcept {
    const std::size_t into = index - voice.start_frame;
    if (into < voice.fade_in) {
      return static_cast<float>(into) * voice.inv_fade_in;
    }
    const std::size_t remaining = voice.end_frame - index;
    if (remaining <= voice.fade_out) {
      return static_cast<float>(remaining) * voice.inv_fade_out;
    }
    return 1.0F;
  }

  static void render_voice(Voice& voice, std::span<float* const> channels,
                           std::size_t num_frames) noexcept {
    const Sample& sample = *voice.config->sample;
    const std::uint16_t sample_channels = sample.num_channels();
    const std::size_t out_channels = channels.size();

    // Tracked locally and stored once, rather than through voice.peak on every
    // frame: this is a register, that is a memory round-trip in the innermost
    // loop of the audio callback.
    float block_peak = 0.0F;

    // Where this voice starts inside the block, consumed BEFORE the loop rather
    // than after it: every early return below leaves through `return`, so
    // clearing it afterwards would leave a stale offset on a voice that ended
    // mid-block and apply it again to whatever reused the slot.
    const std::size_t first = voice.start_offset;
    voice.start_offset = 0;

    for (std::size_t frame = first; frame < num_frames; ++frame) {
      const auto index = static_cast<std::size_t>(voice.phase >> kPhaseFractionBits);

      // The END OF THE SLICE, not the end of the sample. This is the line that
      // turns a sample player into a chopper.
      if (index >= voice.end_frame) {
        voice.active = false;
        voice.peak = block_peak;
        return;
      }

      // A released envelope reaching zero ends the voice too -- otherwise a
      // choked hit would hold its slot silently until its slice ran out, and
      // sixteen of those exhaust the pool.
      //
      // Checked BEFORE next(), not after: next() returns the current level and
      // then advances, so testing afterwards would discard the frame it just
      // produced. That frame is the last of the release and is nearly silent, so
      // dropping it is inaudible -- but it also makes the envelope's frame count
      // depend on where the block boundary fell, which is exactly the class of
      // bug the frame-denominated design exists to rule out.
      if (voice.env.idle()) {
        voice.active = false;
        voice.peak = block_peak;
        return;
      }

      const float amplitude = voice.gain * voice.env.next() * declick_gain(voice, index);

      // Where to READ, which is the only thing reverse changes. See
      // PadConfig::reverse for why it is mirrored here rather than by negating
      // the step.
      //
      // Mirrored in FIXED POINT so the fraction comes with it: at a phase of
      // index+0.25 the reverse read is at (M-index)-0.25, which is
      // (M-index-1)+0.75. Mirroring only the integer part would snap reverse
      // playback to frame boundaries and pitch it subtly differently from
      // forward.
      const PhaseFixed read_phase =
          voice.reverse ? (static_cast<PhaseFixed>(voice.start_frame + voice.end_frame - 1)
                           << kPhaseFractionBits) -
                              voice.phase
                        : voice.phase;
      const auto fraction = static_cast<float>(read_phase & kPhaseFractionMask) * kPhaseToFloat;
      const auto offset = static_cast<std::ptrdiff_t>(read_phase >> kPhaseFractionBits);

      for (std::size_t channel = 0; channel < out_channels; ++channel) {
        // Mono feeds every output channel; a sample with fewer channels than the
        // engine has repeats its last one rather than leaving silence. No branch
        // on channel count in the caller, and no allocation to build a mapping.
        const auto source_channel = static_cast<std::uint16_t>(
            channel < sample_channels ? channel : (sample_channels - 1U));
        const float* frame0 = sample.frame0(source_channel);

        // The guard frames are what make these four reads safe at index 0 and at
        // num_frames - 1 without a bounds check here. A slice boundary in the
        // MIDDLE of the sample needs no guards at all -- there is real audio on
        // both sides of it, which is exactly why it clicks and why declick_gain
        // exists.
        const float value = hermite4(frame0[offset - 1], frame0[offset], frame0[offset + 1],
                                     frame0[offset + 2], fraction);
        const float scaled = amplitude * value;
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
