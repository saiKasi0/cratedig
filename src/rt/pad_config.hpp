#ifndef CRATEDIG_RT_PAD_CONFIG_HPP
#define CRATEDIG_RT_PAD_CONFIG_HPP

#include "rt/envelope.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rt {

// Everything the audio thread needs to know about one pad, as one owned object
// reached through one pointer.
//
// THE ONE-POINTER RULE
// --------------------
// docs/ARCHITECTURE.md derives it: anything the audio thread reads which the
// control thread can change must be reachable through a single pointer, so that
// changing it is a single swap. A config the control thread mutated field by
// field could not be published atomically at all -- the audio thread would read
// a slice range from the new edit with an envelope from the old one.
//
// So a PadConfig is IMMUTABLE once published. Editing a pad means building a new
// one from the old, publishing it through rt::HandoffRing, and letting the
// displaced one die on the janitor thread. That is why every consumer holds
// shared_ptr<const PadConfig> and never PadConfig&.
//
// NO std::string HERE, deliberately. This is what the *audio thread* reads; a
// pad's display name belongs to the interface (tui::PadState already has one)
// and its project metadata belongs to M6's save file. Keeping this struct to
// what the callback dereferences is what stops it accumulating fields nobody on
// the audio thread ever looks at.
//
// M3 fills in the playback half (slice range, envelope, choke group, tuning);
// M5 adds the mixer half and M8 the insert chain, into this same struct rather
// than into parallel ones -- see the PadConfig block in docs/ARCHITECTURE.md.
// Reverse and loop mode are deliberately absent until the DSP for them lands in
// M5: a field nothing honours is worse than no field.

// What a note-off means for this pad.
enum class TriggerMode : std::uint8_t {
  // Plays to the end of the slice whatever the player does. What a drum machine
  // does, and what most sampled material wants.
  kOneShot = 0,

  // Sustains while held and releases when let go. Needs a real note-off, which
  // means MIDI (M4) or the Kitty keyboard protocol (M3) -- a terminal that
  // cannot report key release simply never sends one, and a gate pad then
  // behaves as a one-shot rather than sticking on.
  kGate,
};

// ~0.7 ms at 48 kHz. Long enough to turn a step into a ramp the ear reads as
// silence, short enough not to soften a kick's attack -- which a 2 ms fade
// audibly does, and which would make every chopped drum sound slightly wrong in
// a way that is hard to attribute to the fade.
inline constexpr std::size_t kDefaultFadeFrames = 32;

struct PadConfig {
  // What plays. Null is legal and means an unloaded pad, which is silent rather
  // than an error -- a 4x4 grid is mostly empty when you start.
  std::shared_ptr<const Sample> sample;

  // Which pad this config is for.
  //
  // Carried inside the config rather than alongside it in the ring, so the ring
  // stays a plain channel of owning handles instead of needing an envelope
  // struct with its own move semantics. The engine validates it on arrival: it
  // has travelled from the control thread and is not trusted, exactly like
  // PadEvent::pad.
  std::uint8_t pad = 0;

  // The slice, as a half-open range of source frames.
  //
  // end_frame == 0 means "to the end of the sample", so a default-constructed
  // config plays the whole thing. That is what makes PadConfig{sample, pad} --
  // which is all set_pad_sample() builds -- mean the obvious thing, and it is
  // the only sentinel here; an explicit range past the end is clamped rather
  // than trusted.
  std::size_t start_frame = 0;
  std::size_t end_frame = 0;

  AdsrFrames env{};

  // Declick: a short fade at each end of the SLICE, independent of the envelope.
  //
  // These are two different problems and conflating them produces a sampler that
  // clicks. A slice boundary lands wherever the chop put it, so the waveform
  // steps discontinuously to and from zero there; that is a property of
  // POSITION and is what these fix. The envelope is musical and is triggered by
  // events. Zero-crossing snap (ingest::snap_to_zero_crossing) removes most of
  // the discontinuity; this is the backstop for when it cannot.
  //
  // Clamped to half the slice at trigger time, so a fade can never run past the
  // slice's midpoint however short the slice is.
  std::size_t fade_in_frames = kDefaultFadeFrames;
  std::size_t fade_out_frames = kDefaultFadeFrames;

  // Linear, multiplied with the trigger velocity.
  float gain = 1.0F;

  // Playback speed, multiplied into the phase step. 2.0 is an octave up.
  // Retuning a chop resamples it -- there is no time-stretch until v2.
  float pitch_ratio = 1.0F;

  // The shortest a RELEASE is allowed to take, whatever `env.release` says.
  //
  // Not a musical setting -- a declick, which is why it defaults to the same
  // kDefaultFadeFrames the boundary fades use. `AdsrFrames::release` defaults to
  // zero, so without this a default pad released from full scale fell to silence
  // in one frame: a step discontinuity, and a click. Choke groups and gate
  // note-offs both did that from M3 until M4.5, and the panic key would have
  // made it sixteen at once.
  //
  // Zero is legal and means a hard cut, for a pad that genuinely wants one --
  // which is why this is a field rather than a constant inside Envelope.
  std::size_t release_floor_frames = kDefaultFadeFrames;

  // 0 means "not in a group". Triggering a pad in group G releases every other
  // sounding voice in G, which is how a closed hat cuts an open one.
  std::uint8_t choke_group = 0;

  TriggerMode trigger = TriggerMode::kOneShot;
};

static_assert(kNumPads <= 256, "PadConfig::pad is a uint8_t");

}  // namespace rt

#endif  // CRATEDIG_RT_PAD_CONFIG_HPP
