#ifndef CRATEDIG_RT_ENVELOPE_HPP
#define CRATEDIG_RT_ENVELOPE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace rt {

// Per-voice amplitude envelope: linear ADSR, denominated in frames.
//
// LINEAR, not exponential. One multiply-add per frame, no transcendental
// anywhere near the audio callback, and on an amplitude envelope the difference
// is a subtlety of taste rather than of correctness. An exponential curve is a
// PadConfig field somebody can add later; a call to expf() in the inner loop is
// not something to add later.
//
// IN FRAMES, not seconds or milliseconds. The block size the device negotiated
// must not be able to change the shape of an envelope, and a frame count cannot
// be affected by it. This is the same reasoning that fixed M2's meter fall, and
// it is what keeps block-size invariance -- rendering N frames as one call or as
// N/128 calls -- true of the envelope as well as of the phase accumulator.
//
// NO ACCUMULATION. The level is computed as `start + position * step` rather
// than by adding `step` on every frame. Both are block-size invariant; only this
// one is also drift-free, and it costs the same multiply-add. A 400 ms attack at
// 48 kHz is 19 200 additions, which is enough for float error to be visible in a
// characterisation test even though it would never be audible -- and a test that
// has to allow slop is a test that has stopped pinning anything down.

enum class EnvStage : std::uint8_t {
  kIdle = 0,  // silent, and reusable
  kAttack,
  kDecay,
  kSustain,
  kRelease,
};

// The four numbers, as a plain value so PadConfig can hold one.
//
// `sustain` is a LINEAR gain in [0, 1], not dB: it is multiplied into the sample
// directly, and a dB field would mean a pow() on the audio thread or a
// conversion the caller has to remember. The interface converts for display.
struct AdsrFrames {
  std::size_t attack = 0;
  std::size_t decay = 0;
  float sustain = 1.0F;
  std::size_t release = 0;
};

class Envelope {
 public:
  // AUDIO THREAD. Starts at the beginning of the attack segment.
  //
  // Deliberately starts at gain 0 even when attack is zero-length -- in that
  // case the first frame of the decay segment is already 1.0, so nothing is
  // lost, and the "zero attack" case does not need its own branch anywhere else.
  void trigger(const AdsrFrames& spec) noexcept {
    m_spec = spec;  // before begin(), which advances through it when segments are zero-length
    begin(EnvStage::kAttack, 0.0F, 1.0F, spec.attack);
  }

  // AUDIO THREAD. Falls to silence from WHEREVER THE LEVEL IS NOW.
  //
  // Releasing from the current level rather than from sustain is the whole
  // point: a pad choked 5 ms into a 200 ms attack must fall from the quiet level
  // it actually reached, not jump up to sustain first and then fall. That jump
  // is an audible click, and it is the classic way to get one.
  //
  // `floor_frames` is the SHORTEST this fall is allowed to take, and it exists
  // because getting the level right is only half of not clicking. AdsrFrames'
  // release defaults to zero, so a default pad released from full scale went to
  // silence in a single frame -- a step discontinuity, which is the other
  // classic way to get one. Choke groups and gate note-offs both did that from
  // M3 until M4.5.
  //
  // The floor is the caller's policy rather than part of the spec: this stays a
  // pure ADSR that knows nothing about declicking, and PadConfig carries the
  // number so a pad that genuinely wants a hard cut can still ask for zero.
  void release(std::size_t floor_frames) noexcept {
    if (m_stage == EnvStage::kIdle) {
      return;
    }
    begin(EnvStage::kRelease, level(), 0.0F, std::max(m_spec.release, floor_frames));
  }

  // AUDIO THREAD. Silences immediately. For a voice being stolen, where the
  // sound is about to be replaced anyway.
  void stop() noexcept {
    m_stage = EnvStage::kIdle;
    m_start = 0.0F;
    m_step = 0.0F;
    m_frames = 0;
    m_position = 0;
  }

  // AUDIO THREAD. The gain for the current frame, then advance by one.
  [[nodiscard]] float next() noexcept {
    const float value = level();
    if (m_stage == EnvStage::kIdle || m_stage == EnvStage::kSustain) {
      return value;  // both are flat; nothing to advance
    }
    ++m_position;
    if (m_position >= m_frames) {
      advance();
    }
    return value;
  }

  [[nodiscard]] EnvStage stage() const noexcept { return m_stage; }

  [[nodiscard]] bool idle() const noexcept { return m_stage == EnvStage::kIdle; }

  // The gain for the current frame, without advancing.
  [[nodiscard]] float level() const noexcept {
    if (m_stage == EnvStage::kIdle) {
      return 0.0F;
    }
    if (m_stage == EnvStage::kSustain) {
      return m_spec.sustain;
    }
    return m_start + (m_step * static_cast<float>(m_position));
  }

 private:
  // Sets up one linear segment. A zero-length segment is legal and means "arrive
  // instantly": advance() runs on the next next() and the segment contributes no
  // frames, which is what makes attack = 0 or release = 0 work without a special
  // case at every call site.
  void begin(EnvStage stage, float from, float to, std::size_t frames) noexcept {
    m_stage = stage;
    m_start = from;
    m_frames = frames;
    m_position = 0;
    m_step = frames == 0 ? 0.0F : (to - from) / static_cast<float>(frames);
    if (frames == 0) {
      advance();
    }
  }

  void advance() noexcept {
    switch (m_stage) {
      case EnvStage::kAttack:
        begin(EnvStage::kDecay, 1.0F, m_spec.sustain, m_spec.decay);
        break;
      case EnvStage::kDecay:
        m_stage = EnvStage::kSustain;
        m_position = 0;
        break;
      case EnvStage::kRelease:
        stop();
        break;
      case EnvStage::kIdle:
      case EnvStage::kSustain:
        break;  // flat stages never advance
    }
  }

  AdsrFrames m_spec{};
  EnvStage m_stage = EnvStage::kIdle;
  float m_start = 0.0F;
  float m_step = 0.0F;
  std::size_t m_frames = 0;
  std::size_t m_position = 0;
};

}  // namespace rt

#endif  // CRATEDIG_RT_ENVELOPE_HPP
