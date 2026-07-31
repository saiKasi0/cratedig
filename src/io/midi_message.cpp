#include "io/midi_message.hpp"

#include "rt/pad_event.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace io {
namespace {

// Status nibbles. Only these two are notes; everything else on the wire is
// somebody else's business.
constexpr std::uint8_t kStatusNoteOff = 0x80;
constexpr std::uint8_t kStatusNoteOn = 0x90;

constexpr std::uint8_t kStatusBit = 0x80;
constexpr std::uint8_t kStatusKindMask = 0xF0;
constexpr std::uint8_t kStatusChannelMask = 0x0F;

// A note-on and a note-off both carry two data bytes.
constexpr std::size_t kNoteMessageBytes = 3;

// MIDI velocity is seven bits, so full scale is 127 rather than 128. Dividing by
// 128 would make a maximum-velocity hit come out at 0.992 and no hit ever reach
// 1.0, which is the kind of wrong that never gets noticed and quietly costs
// headroom on every pad.
constexpr float kMaxVelocity = 127.0F;

}  // namespace

std::optional<rt::PadEvent> decode_midi(std::span<const std::uint8_t> bytes,
                                        const MidiMap& map) noexcept {
  if (bytes.empty()) {
    return std::nullopt;
  }

  const std::uint8_t status = bytes[0];
  if ((status & kStatusBit) == 0) {
    // No status byte. RtMidi never produces this (see the header), so it is a
    // corrupt message rather than running status, and guessing would invent a
    // note nobody played.
    return std::nullopt;
  }

  const std::uint8_t kind = status & kStatusKindMask;
  if (kind != kStatusNoteOn && kind != kStatusNoteOff) {
    return std::nullopt;  // control change, pitch bend, clock -- not ours
  }

  if (map.channel != kAnyChannel && (status & kStatusChannelMask) != map.channel) {
    return std::nullopt;
  }

  if (bytes.size() < kNoteMessageBytes) {
    return std::nullopt;  // truncated
  }

  const std::uint8_t note = bytes[1];
  const std::uint8_t velocity = bytes[2];
  if ((note & kStatusBit) != 0 || (velocity & kStatusBit) != 0) {
    // Data bytes have their high bit clear by definition. One that does not is
    // the start of the NEXT message arriving early, which means this one was
    // truncated -- accepting it would play a note at an arbitrary velocity.
    return std::nullopt;
  }

  if (note < map.base_note) {
    return std::nullopt;
  }
  const auto pad = static_cast<std::size_t>(note - map.base_note);
  if (pad >= rt::kNumPads) {
    return std::nullopt;  // above the grid: a real note, just not one of ours
  }

  // A NOTE-ON AT VELOCITY ZERO IS A NOTE-OFF. This is not an edge case, it is
  // what a large share of controllers and sequencers actually send -- running
  // status makes it cheaper on the wire than a separate 0x80, so it became the
  // common form. Treating it as a note-on gives a pad that retriggers silently
  // and never releases, which on a gate pad is a note stuck on forever.
  const bool note_off = kind == kStatusNoteOff || velocity == 0;

  return rt::PadEvent{
      .pad = static_cast<std::uint8_t>(pad),
      .kind = note_off ? rt::PadEventKind::kNoteOff : rt::PadEventKind::kNoteOn,

      // LINEAR, deliberately. A velocity curve is a taste decision, and the
      // honest default is to pass on what the controller sent -- most of them
      // already apply a curve of their own, so a second one here would be
      // shaping a shaped signal. Note-offs carry zero: nothing reads it, and a
      // stale velocity on a release would be a number that means nothing.
      .velocity = note_off ? 0.0F : static_cast<float>(velocity) / kMaxVelocity,

      // Zero, and honestly so. RtMidi reports a delta since the previous
      // message on its own clock, which cannot be converted into an offset
      // within a block the audio thread has not started yet. A live hit lands at
      // the top of the next block, which is where a keypress lands too.
      .frame_offset = 0,
  };
}

}  // namespace io
