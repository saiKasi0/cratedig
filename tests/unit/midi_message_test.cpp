// MIDI bytes to PadEvents.
//
// Every case here is a literal byte array, so this is the half of MIDI support
// that CANNOT SKIP -- no controller, no CoreMIDI, no thread. That split is
// deliberate and is what makes "MIDI integration test green" mean something on a
// machine with nothing plugged in: midi_device_test.cpp covers the hardware
// path and is allowed to skip, this covers what the bytes MEAN and is not.

#include "io/midi_message.hpp"

#include "rt/pad_event.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

// Decodes a message written the way it appears on the wire.
[[nodiscard]] std::optional<rt::PadEvent> decode(std::initializer_list<std::uint8_t> bytes,
                                                 const io::MidiMap& map = {}) {
  const std::vector<std::uint8_t> buffer{bytes};
  return io::decode_midi(buffer, map);
}

}  // namespace

TEST_CASE("a note-on becomes a pad hit", "[unit]") {
  // Note 36 is the default base, so it is pad 0. Velocity 127 is full scale.
  const std::optional<rt::PadEvent> event = decode({0x90, 36, 127});
  REQUIRE(event.has_value());
  CHECK(event->pad == 0);
  CHECK(event->kind == rt::PadEventKind::kNoteOn);
  CHECK(event->velocity == 1.0F);
  CHECK(event->frame_offset == 0);
}

TEST_CASE("velocity is scaled by 127, not 128", "[unit]") {
  // Dividing by 128 would make a maximum-velocity hit 0.992 and put a ceiling no
  // pad could ever reach. Silent, permanent, and worth one assertion.
  const std::optional<rt::PadEvent> full = decode({0x90, 36, 127});
  REQUIRE(full.has_value());
  CHECK(full->velocity == 1.0F);

  const std::optional<rt::PadEvent> one = decode({0x90, 36, 1});
  REQUIRE(one.has_value());
  CHECK(one->velocity > 0.0F);
  CHECK(one->velocity < 0.01F);
}

TEST_CASE("notes map onto the pad grid from the base note up", "[unit]") {
  for (std::uint8_t offset = 0; offset < rt::kNumPads; ++offset) {
    const std::optional<rt::PadEvent> event =
        decode({0x90, static_cast<std::uint8_t>(36 + offset), 100});
    INFO("note " << static_cast<int>(36 + offset));
    REQUIRE(event.has_value());
    REQUIRE(event->pad == offset);
  }
}

TEST_CASE("a note outside the grid is ignored rather than wrapped", "[unit]") {
  // Both directions. A note below the base must not produce a huge unsigned pad
  // index, and one above must not wrap onto pad 0 -- either would play a pad
  // nobody asked for, which is worse than silence.
  CHECK_FALSE(decode({0x90, 35, 100}).has_value());   // one below the base
  CHECK_FALSE(decode({0x90, 0, 100}).has_value());    // the bottom of the keyboard
  CHECK_FALSE(decode({0x90, 52, 100}).has_value());   // one above the grid
  CHECK_FALSE(decode({0x90, 127, 100}).has_value());  // the top of the keyboard
}

TEST_CASE("a note-on at velocity zero is a note-off", "[unit]") {
  // NOT an edge case. Running status makes a zero-velocity note-on cheaper on
  // the wire than a separate 0x80, so it became the common form and a large
  // share of controllers and sequencers send it.
  //
  // Read as a note-on, it gives a pad that retriggers silently and never
  // releases -- on a gate pad, a note stuck on forever.
  const std::optional<rt::PadEvent> event = decode({0x90, 38, 0});
  REQUIRE(event.has_value());
  CHECK(event->kind == rt::PadEventKind::kNoteOff);
  CHECK(event->pad == 2);
  CHECK(event->velocity == 0.0F);
}

TEST_CASE("an explicit note-off is a note-off", "[unit]") {
  // The other form, which must reach the same answer -- including its release
  // velocity being discarded rather than passed on as a level.
  const std::optional<rt::PadEvent> event = decode({0x80, 38, 64});
  REQUIRE(event.has_value());
  CHECK(event->kind == rt::PadEventKind::kNoteOff);
  CHECK(event->pad == 2);
  CHECK(event->velocity == 0.0F);
}

TEST_CASE("the channel filter accepts one channel or all of them", "[unit]") {
  // Default is every channel, which is right for a machine with one controller.
  CHECK(decode({0x90, 36, 100}).has_value());
  CHECK(decode({0x9A, 36, 100}).has_value());  // channel 10

  const io::MidiMap only_ten{.base_note = 36, .channel = 10};
  CHECK(decode({0x9A, 36, 100}, only_ten).has_value());
  CHECK_FALSE(decode({0x90, 36, 100}, only_ten).has_value());

  // And the filter applies to note-offs too. Filtering only note-ons would let a
  // release through from a channel whose press was ignored, releasing a note
  // this program never started.
  CHECK_FALSE(decode({0x80, 36, 0}, only_ten).has_value());
  CHECK(decode({0x8A, 36, 0}, only_ten).has_value());
}

TEST_CASE("a custom base note moves the whole grid", "[unit]") {
  const io::MidiMap high{.base_note = 60};  // middle C
  CHECK_FALSE(decode({0x90, 36, 100}, high).has_value());

  const std::optional<rt::PadEvent> event = decode({0x90, 60, 100}, high);
  REQUIRE(event.has_value());
  CHECK(event->pad == 0);
}

TEST_CASE("messages that are not notes are ignored", "[unit]") {
  // The normal traffic on a MIDI cable. None of it is an error; it simply is not
  // ours, and a decoder that complained about every control change would make
  // the log useless.
  CHECK_FALSE(decode({0xB0, 7, 100}).has_value());   // control change (volume)
  CHECK_FALSE(decode({0xC0, 5}).has_value());        // program change
  CHECK_FALSE(decode({0xE0, 0, 64}).has_value());    // pitch bend
  CHECK_FALSE(decode({0xA0, 36, 100}).has_value());  // polyphonic aftertouch
  CHECK_FALSE(decode({0xD0, 100}).has_value());      // channel aftertouch
  CHECK_FALSE(decode({0xF8}).has_value());           // timing clock
  CHECK_FALSE(decode({0xFE}).has_value());           // active sensing
}

TEST_CASE("malformed messages are rejected rather than guessed at", "[unit]") {
  // Empty, truncated, and -- the interesting one -- a message with no status
  // byte. RtMidi never delivers running status (ALSA suppresses it explicitly,
  // CoreMIDI splits packets into whole messages), so a first byte with its high
  // bit clear is CORRUPT. Carrying state to interpret it would turn a damaged
  // message into a plausible note.
  CHECK_FALSE(decode({}).has_value());
  CHECK_FALSE(decode({0x90}).has_value());      // status only
  CHECK_FALSE(decode({0x90, 36}).has_value());  // missing velocity
  CHECK_FALSE(decode({36, 100}).has_value());   // data with no status byte

  // A data byte with its high bit set is the next message arriving early, which
  // means this one was cut short. Accepting it would play a note at a velocity
  // that is really somebody else's status byte.
  CHECK_FALSE(decode({0x90, 0x90, 100}).has_value());
  CHECK_FALSE(decode({0x90, 36, 0x90}).has_value());
}

TEST_CASE("a longer buffer is decoded from its first message", "[unit]") {
  // RtMidi delivers one message per callback, so trailing bytes should not
  // happen -- but reading past three bytes would be a bug waiting for the day
  // they do.
  const std::optional<rt::PadEvent> event = decode({0x90, 36, 100, 0x90, 37, 100});
  REQUIRE(event.has_value());
  CHECK(event->pad == 0);
  CHECK(event->kind == rt::PadEventKind::kNoteOn);
}
