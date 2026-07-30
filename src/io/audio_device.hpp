#ifndef CRATEDIG_IO_AUDIO_DEVICE_HPP
#define CRATEDIG_IO_AUDIO_DEVICE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// RtAudio.h is deliberately NOT included here. src/io/ is the only place in the
// codebase allowed to include it (CLAUDE.md), and audio_device.cpp is the only
// file in src/io/ that does. Keeping it out of this header is what stops the
// device API leaking into the engine, the TUI, and the tests -- which is in turn
// what lets Engine::render() run in a plain loop for offline export and Docker
// CI, where there is no sound card at all.
//
// The forward declaration plus unique_ptr is the mechanism; the destructor is
// defined in the .cpp because the deleter needs the complete type.
class RtAudio;

namespace engine {
class Engine;
}

namespace io {

enum class DeviceError : std::uint8_t {
  kNone = 0,
  kNoDeviceAvailable,
  kUnsupportedChannelCount,
  kOpenFailed,
  kStartFailed,
  kAlreadyOpen,
  kNotOpen,
};

[[nodiscard]] std::string_view describe(DeviceError error) noexcept;

struct DeviceInfo {
  unsigned int id = 0;
  std::string name;
  unsigned int output_channels = 0;
  unsigned int preferred_sample_rate = 0;
  bool is_default_output = false;
};

// Owns the RtAudio stream and pumps Engine::render() from its callback.
//
// The one hard rule here: nothing this class does on the audio thread may
// allocate, lock, or block. The callback builds its channel-pointer array on the
// stack and calls straight into the engine, which opens its own RT_SCOPE.
class AudioDevice {
 public:
  // Beyond this the callback's stack array would need to grow; stereo is what
  // M1 uses and the mixer's own channel budget arrives with M5.
  static constexpr std::size_t kMaxChannels = 8;

  struct Config {
    std::uint32_t sample_rate = 48'000;
    std::uint16_t num_channels = 2;

    // Requested, not guaranteed. RtAudio negotiates with the device and reports
    // what it actually got -- see actual_block_frames(), which is what the
    // engine must be configured for.
    std::uint32_t block_frames = 256;

    // Zero means "the system default output".
    unsigned int device_id = 0;
  };

  AudioDevice();
  ~AudioDevice();

  AudioDevice(const AudioDevice&) = delete;
  AudioDevice& operator=(const AudioDevice&) = delete;
  AudioDevice(AudioDevice&&) = delete;
  AudioDevice& operator=(AudioDevice&&) = delete;

  // Every output device the current API can see. Returns an empty vector when
  // there are none, which is the normal state in a container and must not be
  // treated as an error by callers that only want to list.
  [[nodiscard]] std::vector<DeviceInfo> output_devices() const;

  [[nodiscard]] bool has_output_device() const;

  // By value: RtAudio builds this string on demand, so a string_view into it
  // would dangle the moment this function returned.
  [[nodiscard]] std::string api_name() const;

  // Opens a stream that will call engine.render(). The engine must outlive this
  // object, and its config must match `config` -- in particular
  // max_block_frames must be at least actual_block_frames().
  [[nodiscard]] DeviceError open(engine::Engine& engine, const Config& config);

  [[nodiscard]] DeviceError start();

  void stop() noexcept;
  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] bool is_running() const noexcept;

  // What RtAudio actually negotiated. Only meaningful once open() succeeded.
  [[nodiscard]] std::uint32_t actual_block_frames() const noexcept;

  // Callback deadlines missed. Non-zero means audible dropouts happened; the
  // shell shows it because a silent xrun counter teaches the wrong lesson about
  // how well the engine is keeping up.
  [[nodiscard]] std::uint64_t xrun_count() const noexcept;

  // RtAudio's own message for the last failure, which is far more useful than
  // the enum when a device is busy or a rate is unsupported.
  [[nodiscard]] const std::string& last_error() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace io

#endif  // CRATEDIG_IO_AUDIO_DEVICE_HPP
