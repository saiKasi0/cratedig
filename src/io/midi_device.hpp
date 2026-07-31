#ifndef CRATEDIG_IO_MIDI_DEVICE_HPP
#define CRATEDIG_IO_MIDI_DEVICE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// RtMidi.h is deliberately NOT included here, for exactly the reason
// audio_device.hpp keeps RtAudio.h out: src/io/ is the only place in the
// codebase allowed to include it (CLAUDE.md), and midi_device.cpp is the only
// file in src/io/ that does. scripts/check_layering.sh enforces both.
//
// The forward declaration plus unique_ptr is the mechanism; the destructor is
// defined in the .cpp because the deleter needs the complete type.
class RtMidiIn;

namespace io {

enum class MidiError : std::uint8_t {
  kNone = 0,
  kNoPortAvailable,
  kPortOutOfRange,
  kOpenFailed,
  kAlreadyOpen,
  kNotOpen,

  // The system MIDI service could not be reached at all -- distinct from having
  // no ports, which is a working service with nothing plugged into it. On macOS
  // this is MIDIClientCreate failing while the MIDI server is busy or
  // restarting; RtMidi reports it by THROWING from its constructor.
  kBackendUnavailable,
};

[[nodiscard]] std::string_view describe(MidiError error) noexcept;

struct MidiPortInfo {
  unsigned int id = 0;
  std::string name;
};

// What a MIDI callback hands back: the raw bytes of one message.
//
// A FUNCTION POINTER AND A CONTEXT, not a std::function. RtMidi calls this on
// its own high-priority thread, and while that thread is not the audio thread,
// it is close enough to one that a std::function's potential allocation and
// indirection are not worth the convenience. It is also what keeps this header
// free of any decision about what the bytes mean -- src/io/midi_message.hpp
// owns that, and the engine owns what to do about it.
using MidiHandler = void (*)(void* context, const std::uint8_t* bytes, std::size_t count);

// UPSTREAM DEFECT, RECORDED HERE BECAUSE IT CANNOT BE FIXED FROM THIS SIDE
// ------------------------------------------------------------------------
// RtMidi 6.0.0 declares MidiInCore::getCoreMidiClientSingleton() as `throw()`
// -- equivalent to noexcept since C++11 -- and then calls error(), which
// THROWS when MIDIClientCreate fails. An exception leaving a noexcept function
// calls std::terminate immediately, without unwinding, so no try/catch anywhere
// up the stack can ever run. RtMidi.cpp:1154.
//
// The trigger is MIDIClientCreate returning -304, which happens while the macOS
// MIDI server is busy or restarting. Measured on this machine: 0 failures in
// ~240 constructions under the dev build and 1 under asan, against 4 in 25 under
// TSan -- TSan's slowdown pushes the XPC handshake with MIDIServer past a
// timeout. So this is overwhelmingly a sanitiser-environment artefact rather
// than something a user meets, but the crash mode is real and the honest thing
// is to say so rather than to have it rediscovered.
//
// Everything below still guards the calls that CAN be caught -- openPort and
// friends throw from ordinary functions -- so a busy port is reported normally.
// Only the client-creation path is beyond reach.

// Owns an RtMidi input port and forwards its messages.
//
// THE THREAD THIS CALLS BACK ON IS NOT THE CONTROL THREAD. RtMidi delivers on a
// thread of its own, which is why the engine gives MIDI its own SPSC ring rather
// than letting it share the keyboard's: rt::SpscRing is single-PRODUCER, and two
// producers on one ring is a data race that looks like it works.
class MidiDevice {
 public:
  MidiDevice();
  ~MidiDevice();

  MidiDevice(const MidiDevice&) = delete;
  MidiDevice& operator=(const MidiDevice&) = delete;
  MidiDevice(MidiDevice&&) = delete;
  MidiDevice& operator=(MidiDevice&&) = delete;

  // Every MIDI input the current API can see. An empty vector is the NORMAL
  // state in a container and on a machine with nothing plugged in, and callers
  // that only want to list must not treat it as an error.
  [[nodiscard]] std::vector<MidiPortInfo> input_ports() const;

  [[nodiscard]] bool has_input_port() const;

  // By value: RtMidi builds these strings on demand, so a view into one would
  // dangle the moment this returned.
  [[nodiscard]] std::string api_name() const;

  // Opens `port_id` and starts delivering to `handler`. The context must outlive
  // the open port.
  //
  // Ignores active sensing and timing clock, which some controllers send
  // continuously -- forwarding them would wake the handler hundreds of times a
  // second to deliver nothing anyone asked for.
  [[nodiscard]] MidiError open(unsigned int port_id, MidiHandler handler, void* context);

  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept;

  // RtMidi's own message for the last failure, which is far more useful than the
  // enum when a port is busy or has disappeared.
  [[nodiscard]] const std::string& last_error() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace io

#endif  // CRATEDIG_IO_MIDI_DEVICE_HPP
