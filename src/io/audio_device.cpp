#include "io/audio_device.hpp"

#include "engine/engine.hpp"
#include "rt/recorder.hpp"

#include <RtAudio.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace io {
namespace {

// RTAUDIO_NONINTERLEAVED gives a planar callback buffer: channel 0's frames,
// then channel 1's. That is exactly the layout Engine::render() takes, so the
// callback needs no interleave pass and no scratch buffer -- which is the
// difference between a real-time-safe callback and one that wants a heap
// allocation on the first block.
//
// RTAUDIO_MINIMIZE_LATENCY is deliberately NOT set. It does not nudge the buffer
// size toward the request, it overrides it: asking for 256 frames on CoreAudio
// produced a 15-frame callback, ~0.3 ms, and an xrun on startup. Block size is a
// latency-versus-safety decision the caller is making on purpose, and a device
// layer that silently substitutes its own answer takes that decision away. The
// negotiated value is reported through actual_block_frames() either way.
constexpr RtAudioStreamFlags kStreamFlags = RTAUDIO_NONINTERLEAVED;

// rt::Recorder carries its own copy of this limit, because src/rt/ may not
// include anything outside src/rt/ (CLAUDE.md) and the device layer is exactly
// what it must not know about. This file is allowed to see both, so this is
// where the two are kept in step -- the recorder's header says the check lives
// here, and here it is rather than being a promise nobody made good on.
static_assert(rt::kMaxRecordChannels >= AudioDevice::kMaxChannels,
              "the recorder must accept every channel a stream can deliver");

}  // namespace

struct AudioDevice::Impl {
  // Constructed on first use, not on construction of AudioDevice.
  //
  // Instantiating RtAudio initialises a backend, and on a Linux box with no
  // sound card libasound writes several lines of complaint straight to stderr.
  // With `cratedig --no-audio` that lands on top of the interface, because the
  // TUI and libasound share a terminal -- which is how this was found, as ALSA
  // warnings in a PTY snapshot. Nothing that never opens a device should pay
  // for a backend it is not going to use.
  std::unique_ptr<RtAudio> audio;
  engine::Engine* engine = nullptr;
  std::uint32_t channels = 0;
  std::uint32_t input_channels = 0;
  std::uint32_t block_frames = 0;

  // Written by the audio thread, read by the UI. Relaxed: it is a monotonic
  // diagnostic counter and nothing else is ordered by it.
  std::atomic<std::uint64_t> xruns{0};

  // Callbacks delivered since the stream opened.
  //
  // NOT a statistic. It is what stop() uses to decide whether there is anything
  // to drain -- see the note there. Relaxed for the same reason as xruns.
  std::atomic<std::uint64_t> callbacks{0};

  std::string last_error;

  RtAudio& backend() {
    if (audio == nullptr) {
      audio = std::make_unique<RtAudio>(
          RtAudio::UNSPECIFIED,
          // RtAudio 6 reports through this instead of throwing. Capturing the
          // text means a failure can say "Device unavailable" rather than only
          // "kOpenFailed".
          [this](RtAudioErrorType /*type*/, const std::string& text) { last_error = text; });
    }
    return *audio;
  }

  // AUDIO THREAD. Everything here is stack-only by construction.
  //
  // A static member rather than a free function in an anonymous namespace: Impl
  // is a private nested type, so nothing outside the class can name it, and
  // casting the void* to something else would defeat the point of having it.
  static int render(void* output_buffer, void* input_buffer, unsigned int frame_count,
                    double stream_time, RtAudioStreamStatus status, void* user_data);
};

int AudioDevice::Impl::render(void* output_buffer, void* input_buffer, unsigned int frame_count,
                              double /*stream_time*/, RtAudioStreamStatus status, void* user_data) {
  auto* impl = static_cast<AudioDevice::Impl*>(user_data);
  impl->callbacks.fetch_add(1, std::memory_order_relaxed);  // see stop()

  // BOTH DIRECTIONS COUNT. An input overflow means captured audio was thrown
  // away before this callback ever saw it -- a hole in a take that
  // rt::Recorder's own drop counter cannot see, because those frames never
  // reached it. Folded into one counter deliberately: to a person, a stream
  // that could not keep up is one fact rather than two.
  if ((status & (RTAUDIO_OUTPUT_UNDERFLOW | RTAUDIO_INPUT_OVERFLOW)) != 0) {
    impl->xruns.fetch_add(1, std::memory_order_relaxed);  // diagnostic only
  }

  auto* base = static_cast<float*>(output_buffer);
  const auto channel_count = static_cast<std::size_t>(impl->channels);

  // std::array on the stack, sized at compile time: building this with a vector
  // would allocate on every callback, which is the exact failure RT_SCOPE exists
  // to catch. open() has already rejected anything wider than kMaxChannels.
  std::array<float*, AudioDevice::kMaxChannels> channels{};
  for (std::size_t channel = 0; channel < channel_count; ++channel) {
    channels[channel] = base + (channel * frame_count);
  }

  // The input side, same layout and the same stack-only construction. RtAudio
  // hands a null buffer on an output-only stream, and the engine takes an empty
  // span to mean exactly that -- so there is ONE call below rather than a
  // branch into two renders that would drift apart.
  std::array<const float*, AudioDevice::kMaxChannels> inputs{};
  std::size_t input_count = 0;
  if (input_buffer != nullptr) {
    input_count = static_cast<std::size_t>(impl->input_channels);
    const auto* input_base = static_cast<const float*>(input_buffer);
    for (std::size_t channel = 0; channel < input_count; ++channel) {
      inputs[channel] = input_base + (channel * frame_count);
    }
  }

  impl->engine->render(std::span<float* const>{channels.data(), channel_count},
                       std::span<const float* const>{inputs.data(), input_count}, frame_count);
  return 0;
}

std::string_view describe(DeviceError error) noexcept {
  switch (error) {
    case DeviceError::kNone:
      return "no error";
    case DeviceError::kNoDeviceAvailable:
      return "no audio output device is available";
    case DeviceError::kUnsupportedChannelCount:
      return "unsupported channel count";
    case DeviceError::kOpenFailed:
      return "the audio stream could not be opened";
    case DeviceError::kStartFailed:
      return "the audio stream could not be started";
    case DeviceError::kAlreadyOpen:
      return "the device is already open";
    case DeviceError::kNotOpen:
      return "the device is not open";
    case DeviceError::kNoInputDeviceAvailable:
      return "no audio input device is available";
  }
  return "unknown error";
}

AudioDevice::AudioDevice() : m_impl(std::make_unique<Impl>()) {}

AudioDevice::~AudioDevice() {
  stop();
  close();
}

std::vector<DeviceInfo> AudioDevice::devices(bool want_output) const {
  std::vector<DeviceInfo> devices;

  // Zero devices is normal, not an error: a container has no /dev/snd, and the
  // Linux CI run is the ROADMAP acceptance path. Anything here that treated an
  // empty list as a failure would make the whole suite unrunnable in Docker.
  for (const unsigned int id : m_impl->backend().getDeviceIds()) {
    const RtAudio::DeviceInfo info = m_impl->backend().getDeviceInfo(id);
    if (want_output ? info.outputChannels == 0 : info.inputChannels == 0) {
      continue;  // cannot do the direction being asked about
    }
    devices.push_back(DeviceInfo{.id = id,
                                 .name = info.name,
                                 .output_channels = info.outputChannels,
                                 .input_channels = info.inputChannels,
                                 .preferred_sample_rate = info.preferredSampleRate,
                                 .is_default_output = info.isDefaultOutput,
                                 .is_default_input = info.isDefaultInput});
  }
  return devices;
}

std::vector<DeviceInfo> AudioDevice::output_devices() const {
  return devices(true);
}

std::vector<DeviceInfo> AudioDevice::input_devices() const {
  return devices(false);
}

bool AudioDevice::has_output_device() const {
  return !output_devices().empty();
}

bool AudioDevice::has_input_device() const {
  return !input_devices().empty();
}

std::string AudioDevice::api_name() const {
  return RtAudio::getApiDisplayName(m_impl->backend().getCurrentApi());
}

DeviceError AudioDevice::open(engine::Engine& engine, const Config& config) {
  if (is_open()) {
    return DeviceError::kAlreadyOpen;
  }
  if (config.num_channels == 0 || config.num_channels > kMaxChannels) {
    return DeviceError::kUnsupportedChannelCount;
  }

  unsigned int device_id = config.device_id;
  if (device_id == 0) {
    device_id = m_impl->backend().getDefaultOutputDevice();
  }
  if (device_id == 0) {
    return DeviceError::kNoDeviceAvailable;
  }

  RtAudio::StreamParameters parameters;
  parameters.deviceId = device_id;
  parameters.nChannels = config.num_channels;
  parameters.firstChannel = 0;

  // The capture side, and null when none was asked for -- which is what makes
  // openStream() below open a half-duplex stream rather than a duplex one.
  RtAudio::StreamParameters input_parameters;
  RtAudio::StreamParameters* input_pointer = nullptr;
  if (config.input_channels > 0) {
    if (config.input_channels > kMaxChannels) {
      return DeviceError::kUnsupportedChannelCount;
    }
    unsigned int input_id = config.input_device_id;
    if (input_id == 0) {
      input_id = m_impl->backend().getDefaultInputDevice();
    }
    if (input_id == 0) {
      return DeviceError::kNoInputDeviceAvailable;
    }
    input_parameters.deviceId = input_id;
    input_parameters.nChannels = config.input_channels;
    input_parameters.firstChannel = 0;
    input_pointer = &input_parameters;
  }

  RtAudio::StreamOptions options;
  options.flags = kStreamFlags;
  options.streamName = "cratedig";

  m_impl->engine = &engine;
  m_impl->channels = config.num_channels;
  m_impl->input_channels = config.input_channels;

  // RtAudio rewrites this with what the device actually granted, which is why
  // actual_block_frames() exists and why the caller must size the engine's
  // max_block_frames from it rather than from what was asked for.
  unsigned int frames = config.block_frames;

  m_impl->last_error.clear();
  const RtAudioErrorType status =
      m_impl->backend().openStream(&parameters, input_pointer, RTAUDIO_FLOAT32, config.sample_rate,
                                   &frames, &Impl::render, m_impl.get(), &options);
  if (status != RTAUDIO_NO_ERROR) {
    // CLOSE WHAT IT MANAGED TO OPEN, and this is not defensive tidying.
    //
    // RtApi::openStream probes the output device first and the input device
    // second, and returns on a failed input probe WITHOUT undoing the output it
    // has already opened (RtAudio.cpp, RtApi::openStream). So a duplex open
    // that fails on the capture side leaves the playback hardware held, the
    // callback never installed, and isStreamOpen() answering true -- which made
    // this class report kOpenFailed and is_open() at the same time, and would
    // have made a caller's retry return kAlreadyOpen.
    //
    // Found by the duplex test on a machine whose default input is mono: asking
    // it for two channels fails exactly here.
    if (m_impl->audio != nullptr && m_impl->audio->isStreamOpen()) {
      m_impl->backend().closeStream();
    }
    m_impl->engine = nullptr;
    m_impl->input_channels = 0;
    return DeviceError::kOpenFailed;
  }

  m_impl->block_frames = frames;
  m_impl->callbacks.store(0, std::memory_order_relaxed);  // per stream, not per process
  return DeviceError::kNone;
}

DeviceError AudioDevice::start() {
  if (!is_open()) {
    return DeviceError::kNotOpen;
  }
  m_impl->last_error.clear();
  if (m_impl->backend().startStream() != RTAUDIO_NO_ERROR) {
    return DeviceError::kStartFailed;
  }
  return DeviceError::kNone;
}

void AudioDevice::stop() noexcept {
  if (m_impl->audio == nullptr || !m_impl->audio->isStreamRunning()) {
    return;
  }

  // A STREAM THAT NEVER CALLED BACK HAS NOTHING TO DRAIN, and waiting for it to
  // drain anyway is a hang rather than a delay.
  //
  // stopStream() waits on a condition the callback signals. That is right when
  // audio is flowing -- it is what keeps the last block from being cut off
  // mid-note, which is audible. It is a deadlock when the callback has never
  // run: the thing being waited for cannot happen.
  //
  // Not hypothetical. A macOS machine whose CoreAudio had been left holding a
  // stale client opened and started a stream normally, parked its IO thread,
  // and delivered nothing -- and this function then blocked for ever in
  // pthread_cond_wait, taking the whole program with it. Found by a sample of a
  // hung test process, having first been mistaken for a slow test.
  if (m_impl->callbacks.load(std::memory_order_relaxed) == 0) {
    static_cast<void>(m_impl->backend().abortStream());
    return;
  }
  static_cast<void>(m_impl->backend().stopStream());
}

void AudioDevice::close() noexcept {
  if (m_impl->audio != nullptr && m_impl->audio->isStreamOpen()) {
    m_impl->backend().closeStream();
  }
  m_impl->engine = nullptr;
  m_impl->input_channels = 0;
}

bool AudioDevice::is_open() const noexcept {
  return m_impl->audio != nullptr && m_impl->audio->isStreamOpen();
}

bool AudioDevice::is_running() const noexcept {
  return m_impl->audio != nullptr && m_impl->audio->isStreamRunning();
}

std::uint32_t AudioDevice::actual_block_frames() const noexcept {
  return m_impl->block_frames;
}

std::uint16_t AudioDevice::input_channels() const noexcept {
  return static_cast<std::uint16_t>(m_impl->input_channels);
}

std::uint64_t AudioDevice::callback_count() const noexcept {
  return m_impl->callbacks.load(std::memory_order_relaxed);
}

std::uint64_t AudioDevice::xrun_count() const noexcept {
  return m_impl->xruns.load(std::memory_order_relaxed);
}

const std::string& AudioDevice::last_error() const noexcept {
  return m_impl->last_error;
}

}  // namespace io
