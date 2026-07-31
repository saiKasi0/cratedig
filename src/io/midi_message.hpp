#ifndef CRATEDIG_IO_MIDI_MESSAGE_HPP
#define CRATEDIG_IO_MIDI_MESSAGE_HPP

#include "rt/pad_event.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace io {

// Turning MIDI bytes into a PadEvent.
//
// A PURE FUNCTION, which is the whole point. It needs no device, no hardware and
// no thread, so it is tested against literal byte arrays and therefore CANNOT
// SKIP -- the correctness of MIDI input does not depend on anyone having a
// controller plugged in. midi_device.cpp is the part that needs hardware, and it
// contains no interpretation at all.
//
// Where the velocity curve lives, as rt::PadEvent has said since M1: "the
// src/io/ MIDI layer owns the curve choice in M4 and the engine never has to
// know where a hit came from".

inline constexpr std::uint8_t kAnyChannel = 0xFF;

struct MidiMap {
  // The note that plays pad 0.
  //
  // 36 is C1, the General MIDI bass drum, and what essentially every pad
  // controller ships mapped to its first pad -- so a plugged-in MPD or Maschine
  // lines up with cratedig's grid without configuring anything.
  std::uint8_t base_note = 36;

  // 0..15, or kAnyChannel to accept all of them. Accepting everything is the
  // right default for a machine with one controller attached; the filter is for
  // a rig where something else is on the same cable.
  std::uint8_t channel = kAnyChannel;
};

// Decode one complete MIDI message.
//
// Returns nothing for anything that is not a note this map cares about -- a
// controller change, a note on another channel, a note outside the pad range,
// or a malformed message. "Nothing" rather than an error, because the normal
// state of a MIDI cable is carrying things this program has no opinion about.
//
// ONE COMPLETE MESSAGE, always with its status byte. RtMidi guarantees it: the
// ALSA backend calls snd_midi_event_no_status() to suppress running status
// explicitly, and the CoreMIDI backend splits packets into whole messages. So a
// first byte with its high bit clear is a MALFORMED message and is rejected,
// rather than being treated as running-status data -- carrying state to guess at
// a case the layer below cannot produce would turn a corrupt message into a
// plausible note.
[[nodiscard]] std::optional<rt::PadEvent> decode_midi(std::span<const std::uint8_t> bytes,
                                                      const MidiMap& map) noexcept;

}  // namespace io

#endif  // CRATEDIG_IO_MIDI_MESSAGE_HPP
