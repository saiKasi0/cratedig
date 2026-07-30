#ifndef CRATEDIG_RT_PAD_CONFIG_HPP
#define CRATEDIG_RT_PAD_CONFIG_HPP

#include "rt/pad_event.hpp"
#include "rt/sample.hpp"

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
};

static_assert(kNumPads <= 256, "PadConfig::pad is a uint8_t");

}  // namespace rt

#endif  // CRATEDIG_RT_PAD_CONFIG_HPP
