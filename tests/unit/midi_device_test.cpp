// The RtMidi input adapter.
//
// Everything here runs with NO MIDI HARDWARE, because that is the state of every
// CI container and most development machines. A device layer whose tests only
// run when something is plugged in is a device layer nobody tests -- so the
// no-hardware paths are the subject rather than the fallback, and the one case
// that genuinely needs a port is tagged [device] and skips loudly.

#include "io/midi_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

// True when this binary is built with ThreadSanitizer.
//
// The backend-touching cases skip under it, and the reason is documented in
// midi_device.hpp: RtMidi terminates rather than throws when MIDIClientCreate
// fails, and TSan's slowdown makes that failure common enough (4 in 25 here,
// against 0 in ~240 on dev) to make those tests report a defect in a
// configuration rather than in the code.
//
// Same precedent as the RT guard, which is compiled out under TSan and whose
// tests SKIP loudly instead of passing vacuously -- see rt_scope.hpp.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CRATEDIG_MIDI_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define CRATEDIG_MIDI_TSAN 1
#endif
#ifndef CRATEDIG_MIDI_TSAN
#define CRATEDIG_MIDI_TSAN 0
#endif

constexpr bool kMidiBackendUsable = CRATEDIG_MIDI_TSAN == 0;

// Counts what arrives, so the handler signature is exercised even where no
// message can be delivered.
struct Recorder {
  std::size_t calls = 0;
  std::vector<std::uint8_t> last;
};

void record(void* context, const std::uint8_t* bytes, std::size_t count) {
  auto* recorder = static_cast<Recorder*>(context);
  ++recorder->calls;
  recorder->last.assign(bytes, bytes + count);
}

}  // namespace

TEST_CASE("MidiDevice constructs without any MIDI hardware", "[unit]") {
  // Constructing must not initialise a backend or write anything to the
  // terminal. The equivalent AudioDevice case exists because libasound wrote
  // complaints over a running TUI; the same trap is one API over.
  const io::MidiDevice device;
  CHECK_FALSE(device.is_open());
  CHECK(device.last_error().empty());
}

TEST_CASE("MidiDevice enumeration tolerates having no ports", "[unit]") {
  if constexpr (!kMidiBackendUsable) {
    SKIP("RtMidi terminates on a failed MIDIClientCreate under TSan -- see midi_device.hpp");
  }
  // An empty list is the NORMAL answer in a container, not an error. A caller
  // that only wants to list must be able to.
  const io::MidiDevice device;
  const std::vector<io::MidiPortInfo> ports = device.input_ports();

  // Whatever the count, the two accessors must agree -- a `has_input_port()`
  // that disagreed with `input_ports().empty()` would send the UI looking for a
  // port that is not there.
  CHECK(device.has_input_port() == !ports.empty());

  for (const io::MidiPortInfo& port : ports) {
    INFO("port " << port.id << ": " << port.name);
    CHECK(port.id < ports.size());
  }
}

TEST_CASE("MidiDevice names its API", "[unit]") {
  if constexpr (!kMidiBackendUsable) {
    SKIP("RtMidi terminates on a failed MIDIClientCreate under TSan -- see midi_device.hpp");
  }
  // Reported on the mode line, so an empty string would be a blank where a fact
  // should be. RtMidi answers even with no ports open.
  const io::MidiDevice device;
  CHECK_FALSE(device.api_name().empty());
}

TEST_CASE("MidiDevice reports rather than crashes when it cannot open", "[unit]") {
  if constexpr (!kMidiBackendUsable) {
    SKIP("RtMidi terminates on a failed MIDIClientCreate under TSan -- see midi_device.hpp");
  }
  io::MidiDevice device;
  Recorder recorder;

  const std::vector<io::MidiPortInfo> ports = device.input_ports();
  const io::MidiError opened = device.open(0, &record, &recorder);

  if (ports.empty()) {
    // The container case, and the one that matters: no ports means a specific
    // refusal, not the out-of-range one. They are different situations and the
    // message a user sees should say which.
    CHECK((opened == io::MidiError::kNoPortAvailable ||
           opened == io::MidiError::kBackendUnavailable));
    CHECK_FALSE(device.is_open());
  } else {
    INFO("opening port 0 of " << ports.size() << ": " << io::describe(opened));
    CHECK((opened == io::MidiError::kNone || opened == io::MidiError::kOpenFailed));
  }
  CHECK(recorder.calls == 0);  // nothing was delivered either way
}

TEST_CASE("MidiDevice refuses a port index it does not have", "[unit]") {
  if constexpr (!kMidiBackendUsable) {
    SKIP("RtMidi terminates on a failed MIDIClientCreate under TSan -- see midi_device.hpp");
  }
  io::MidiDevice device;
  Recorder recorder;

  // Deliberately absurd, so this holds whether or not the machine has hardware.
  const io::MidiError opened = device.open(9'999, &record, &recorder);
  CHECK((opened == io::MidiError::kPortOutOfRange || opened == io::MidiError::kNoPortAvailable ||
         opened == io::MidiError::kBackendUnavailable));
  CHECK_FALSE(device.is_open());
}

TEST_CASE("MidiDevice closing an unopened port is a no-op", "[unit]") {
  // Called from the destructor on every path, including the one where open()
  // failed -- so it has to be safe on a device that never had a port.
  io::MidiDevice device;
  device.close();
  device.close();
  CHECK_FALSE(device.is_open());
}

TEST_CASE("MidiDevice describes every error code", "[unit]") {
  // Exhaustive, so a new enumerator added without a message fails here rather
  // than showing a user "unknown MIDI error".
  constexpr std::array<io::MidiError, 7> kAll{
      io::MidiError::kNone,
      io::MidiError::kNoPortAvailable,
      io::MidiError::kPortOutOfRange,
      io::MidiError::kOpenFailed,
      io::MidiError::kAlreadyOpen,
      io::MidiError::kNotOpen,
      io::MidiError::kBackendUnavailable,
  };

  for (const io::MidiError error : kAll) {
    const std::string_view text = io::describe(error);
    INFO("error code " << static_cast<int>(error));
    REQUIRE_FALSE(text.empty());
    REQUIRE(text != "unknown MIDI error");
  }
}

TEST_CASE("MidiDevice opens a real port", "[device]") {
  if constexpr (!kMidiBackendUsable) {
    SKIP("RtMidi terminates on a failed MIDIClientCreate under TSan -- see midi_device.hpp");
  }
  // The one case that needs hardware. Skips loudly rather than passing
  // vacuously, exactly as the fixture-backed tests do.
  io::MidiDevice device;
  if (!device.has_input_port()) {
    SKIP("no MIDI input port on this machine");
  }

  Recorder recorder;
  const io::MidiError opened = device.open(0, &record, &recorder);
  INFO("open: " << io::describe(opened) << " (" << device.last_error() << ")");
  REQUIRE(opened == io::MidiError::kNone);
  CHECK(device.is_open());

  // A second open must be refused rather than silently replacing the first,
  // which would leave the previous callback's context dangling.
  CHECK(device.open(0, &record, &recorder) == io::MidiError::kAlreadyOpen);

  device.close();
  CHECK_FALSE(device.is_open());
}
