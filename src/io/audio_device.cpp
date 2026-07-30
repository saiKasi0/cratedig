#include "io/audio_device.hpp"

#include "engine/engine.hpp"

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
  std::uint32_t block_frames = 0;

  // Written by the audio thread, read by the UI. Relaxed: it is a monotonic
  // diagnostic counter and nothing else is ordered by it.
  std::atomic<std::uint64_t> xruns{0};

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

int AudioDevice::Impl::render(void* output_buffer, void* /*input_buffer*/, unsigned int frame_count,
                              double /*stream_time*/, RtAudioStreamStatus status, void* user_data) {
  auto* impl = static_cast<AudioDevice::Impl*>(user_data);

  if ((status & RTAUDIO_OUTPUT_UNDERFLOW) != 0) {
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

  impl->engine->render(std::span<float* const>{channels.data(), channel_count}, frame_count);
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
  }
  return "unknown error";
}

AudioDevice::AudioDevice() : m_impl(std::make_unique<Impl>()) {}

AudioDevice::~AudioDevice() {
  stop();
  close();
}

std::vector<DeviceInfo> AudioDevice::output_devices() const {
  std::vector<DeviceInfo> devices;

  // Zero devices is normal, not an error: a container has no /dev/snd, and the
  // Linux CI run is the ROADMAP acceptance path. Anything here that treated an
  // empty list as a failure would make the whole suite unrunnable in Docker.
  for (const unsigned int id : m_impl->backend().getDeviceIds()) {
    const RtAudio::DeviceInfo info = m_impl->backend().getDeviceInfo(id);
    if (info.outputChannels == 0) {
      continue;  // input-only device
    }
    devices.push_back(DeviceInfo{.id = id,
                                 .name = info.name,
                                 .output_channels = info.outputChannels,
                                 .preferred_sample_rate = info.preferredSampleRate,
                                 .is_default_output = info.isDefaultOutput});
  }
  return devices;
}

bool AudioDevice::has_output_device() const {
  return !output_devices().empty();
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

  RtAudio::StreamOptions options;
  options.flags = kStreamFlags;
  options.streamName = "cratedig";

  m_impl->engine = &engine;
  m_impl->channels = config.num_channels;

  // RtAudio rewrites this with what the device actually granted, which is why
  // actual_block_frames() exists and why the caller must size the engine's
  // max_block_frames from it rather than from what was asked for.
  unsigned int frames = config.block_frames;

  m_impl->last_error.clear();
  const RtAudioErrorType status =
      m_impl->backend().openStream(&parameters, nullptr, RTAUDIO_FLOAT32, config.sample_rate,
                                   &frames, &Impl::render, m_impl.get(), &options);
  if (status != RTAUDIO_NO_ERROR) {
    m_impl->engine = nullptr;
    return DeviceError::kOpenFailed;
  }

  m_impl->block_frames = frames;
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
  if (m_impl->audio != nullptr && m_impl->audio->isStreamRunning()) {
    // stopStream drains; abortStream would cut the tail off mid-block. The
    // difference is audible on the last note.
    static_cast<void>(m_impl->backend().stopStream());
  }
}

void AudioDevice::close() noexcept {
  if (m_impl->audio != nullptr && m_impl->audio->isStreamOpen()) {
    m_impl->backend().closeStream();
  }
  m_impl->engine = nullptr;
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

std::uint64_t AudioDevice::xrun_count() const noexcept {
  return m_impl->xruns.load(std::memory_order_relaxed);
}

const std::string& AudioDevice::last_error() const noexcept {
  return m_impl->last_error;
}

}  // namespace io
