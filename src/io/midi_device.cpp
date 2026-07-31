#include "io/midi_device.hpp"

#include <RtMidi.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace io {
namespace {

// What RtMidi hands us, routed to the caller's handler.
//
// The signature is RtMidi's, so this is the one place that knows it. Everything
// above sees MidiHandler, which carries no RtMidi types at all -- that is what
// keeps RtMidi.h inside this file.
struct Callback {
  MidiHandler handler = nullptr;
  void* context = nullptr;
};

void deliver(double /*delta_seconds*/, std::vector<unsigned char>* message, void* user) {
  // Delta time is deliberately dropped. It is the gap since the PREVIOUS
  // message, measured by RtMidi's own clock, and the engine places events
  // against the audio clock instead -- mixing the two would put a second,
  // disagreeing notion of time into the event path.
  if (message == nullptr || message->empty() || user == nullptr) {
    return;
  }
  auto* callback = static_cast<Callback*>(user);
  if (callback->handler == nullptr) {
    return;
  }
  callback->handler(callback->context, message->data(), message->size());
}

}  // namespace

std::string_view describe(MidiError error) noexcept {
  switch (error) {
    case MidiError::kNone:
      return "no error";
    case MidiError::kNoPortAvailable:
      return "no MIDI input port available";
    case MidiError::kPortOutOfRange:
      return "MIDI port index out of range";
    case MidiError::kOpenFailed:
      return "could not open the MIDI port";
    case MidiError::kAlreadyOpen:
      return "a MIDI port is already open";
    case MidiError::kNotOpen:
      return "no MIDI port is open";
    case MidiError::kBackendUnavailable:
      return "the system MIDI service is unavailable";
  }
  return "unknown MIDI error";
}

struct MidiDevice::Impl {
  // Constructed on first use, exactly as AudioDevice does and for the same
  // reason: instantiating RtMidiIn initialises a backend, and a program that
  // never asks for MIDI should not pay for one -- nor risk a backend writing to
  // the terminal the TUI is drawing on.
  std::unique_ptr<RtMidiIn> midi;
  Callback callback;
  bool open = false;
  std::string last_error;

  // Returns NULL when there is no usable backend, which is a state that really
  // happens.
  //
  // RTMIDI THROWS, AND NOT ONLY FROM THE CONSTRUCTOR. On macOS the CoreMIDI
  // client is created by a lazy singleton that getPortCount() reaches as well,
  // so MIDIClientCreate failing surfaces from whichever call happens to come
  // first. MIDIClientCreate returns -304 while the MIDI server is busy or
  // restarting, and several processes starting at once is enough to provoke it.
  //
  // Found by ctest running the suite in parallel: it aborted about one run in
  // four. Guarding only construction did NOT fix it -- the second round of
  // failures came out of enumeration, which is why every entry point below is
  // wrapped rather than just this one. Uncaught, it is a std::terminate in a
  // device layer whose whole promise is to report rather than crash.
  //
  // Retried on the next call rather than latched off, because the failure is
  // transient by nature. Nothing polls this at frame rate -- enumeration is a
  // startup and on-demand operation -- so a retry is paid for only when MIDI is
  // already broken.
  RtMidiIn* backend() {
    if (midi != nullptr) {
      return midi.get();
    }
    try {
      auto created = std::make_unique<RtMidiIn>();
      // RtMidi 6 reports LATER errors through a callback rather than throwing,
      // so capturing the text lets a failure say "MidiInCore: no ports
      // available" instead of only "kOpenFailed".
      created->setErrorCallback(
          [](RtMidiError::Type /*type*/, const std::string& text, void* user) {
            if (user != nullptr) {
              static_cast<Impl*>(user)->last_error = text;
            }
          },
          this);
      midi = std::move(created);
    } catch (const RtMidiError& error) {
      last_error = error.getMessage();
    } catch (const std::exception& error) {
      last_error = error.what();
    }
    return midi.get();
  }
};

MidiDevice::MidiDevice() : m_impl(std::make_unique<Impl>()) {}

MidiDevice::~MidiDevice() {
  close();
}

std::vector<MidiPortInfo> MidiDevice::input_ports() const {
  std::vector<MidiPortInfo> ports;
  RtMidiIn* backend = m_impl->backend();
  if (backend == nullptr) {
    return ports;  // no backend reads the same as no ports, which is the truth
  }
  try {
    const unsigned int count = backend->getPortCount();
    ports.reserve(count);
    for (unsigned int index = 0; index < count; ++index) {
      ports.push_back(MidiPortInfo{.id = index, .name = backend->getPortName(index)});
    }
  } catch (const std::exception& error) {
    m_impl->last_error = error.what();
    ports.clear();  // a partial list would be worse than none: the ids would lie
  }
  return ports;
}

bool MidiDevice::has_input_port() const {
  RtMidiIn* backend = m_impl->backend();
  if (backend == nullptr) {
    return false;
  }
  try {
    return backend->getPortCount() > 0;
  } catch (const std::exception& error) {
    m_impl->last_error = error.what();
    return false;
  }
}

std::string MidiDevice::api_name() const {
  RtMidiIn* backend = m_impl->backend();
  if (backend == nullptr) {
    return "none";  // a name rather than a blank: the mode line has a slot for it
  }
  try {
    return RtMidi::getApiName(backend->getCurrentApi());
  } catch (const std::exception& error) {
    m_impl->last_error = error.what();
    return "none";
  }
}

MidiError MidiDevice::open(unsigned int port_id, MidiHandler handler, void* context) {
  if (m_impl->open) {
    return MidiError::kAlreadyOpen;
  }
  RtMidiIn* backend = m_impl->backend();
  if (backend == nullptr) {
    return MidiError::kBackendUnavailable;
  }

  try {
    const unsigned int count = backend->getPortCount();
    if (count == 0) {
      return MidiError::kNoPortAvailable;
    }
    if (port_id >= count) {
      return MidiError::kPortOutOfRange;
    }

    // The callback is installed BEFORE the port opens. RtMidi queues messages
    // that arrive before a callback exists, and would then deliver a burst of
    // stale notes the moment one is attached -- notes the player pressed before
    // the program was listening.
    m_impl->callback = Callback{.handler = handler, .context = context};
    backend->setCallback(&deliver, &m_impl->callback);

    // Active sensing and timing clock are ignored. Some controllers send active
    // sensing every 300 ms forever, and clock 24 times a beat; forwarding either
    // would wake the handler continuously to deliver something nothing here acts
    // on. Sysex is ignored too -- M4 reads notes, and a large sysex dump would
    // arrive as one enormous message on this thread.
    backend->ignoreTypes(/*midiSysex=*/true, /*midiTime=*/true, /*midiSense=*/true);

    m_impl->last_error.clear();
    backend->openPort(port_id, "cratedig in");
    if (!backend->isPortOpen()) {
      backend->cancelCallback();
      m_impl->callback = Callback{};
      return MidiError::kOpenFailed;
    }
  } catch (const std::exception& error) {
    m_impl->last_error = error.what();
    m_impl->callback = Callback{};
    return MidiError::kOpenFailed;
  }

  m_impl->open = true;
  return MidiError::kNone;
}

void MidiDevice::close() noexcept {
  if (m_impl == nullptr || !m_impl->open) {
    return;
  }
  // Callback first, then the port: cancelling afterwards would leave a window in
  // which a message could arrive for a handler whose context is already gone.
  // close() is noexcept and runs from the destructor, so an escaping exception
  // here would be a terminate on the way out rather than a reported failure.
  try {
    if (m_impl->midi != nullptr) {
      m_impl->midi->cancelCallback();
      m_impl->midi->closePort();
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch): nothing left to report to
  }
  m_impl->callback = Callback{};
  m_impl->open = false;
}

bool MidiDevice::is_open() const noexcept {
  return m_impl != nullptr && m_impl->open;
}

const std::string& MidiDevice::last_error() const noexcept {
  return m_impl->last_error;
}

}  // namespace io
