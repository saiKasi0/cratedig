// The device layer, tested the way CI can actually run it: with no sound card.
//
// Docker has no /dev/snd, and the Linux run is the ROADMAP acceptance path, so
// "zero devices" is a first-class state here rather than an error. Anything that
// requires real hardware is tagged [device] and excluded from the default run --
// see docs/TESTING.md.

#include "io/audio_device.hpp"

#include "engine/engine.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("AudioDevice constructs without a sound card", "[unit]") {
  // Constructing RtAudio must not throw, abort, or hang when there is nothing to
  // talk to. If this ever regresses, every test in the binary fails at once in
  // Docker, so it is worth asserting on its own.
  const io::AudioDevice device;
  CHECK_FALSE(device.api_name().empty());
}

TEST_CASE("AudioDevice enumeration tolerates having no devices", "[unit]") {
  const io::AudioDevice device;
  const std::vector<io::DeviceInfo> devices = device.output_devices();

  // No assertion on the count: a developer machine has several, a container has
  // none, and both are correct. What must hold is that whatever comes back is
  // well formed.
  for (const io::DeviceInfo& info : devices) {
    CHECK(info.output_channels > 0);
    CHECK_FALSE(info.name.empty());
  }
  CHECK(device.has_output_device() == !devices.empty());
}

TEST_CASE("AudioDevice reports rather than crashes when it cannot open", "[unit]") {
  const engine::Engine::Config config{
      .sample_rate = 48'000, .num_channels = 2, .max_block_frames = 2'048, .seed = 0};
  engine::Engine eng{config};
  io::AudioDevice device;

  const io::AudioDevice::Config device_config{
      .sample_rate = 48'000, .num_channels = 2, .block_frames = 256, .device_id = 0};

  const io::DeviceError status = device.open(eng, device_config);

  if (!device.has_output_device()) {
    // The container case, and the one CI exercises.
    CHECK(status == io::DeviceError::kNoDeviceAvailable);
    CHECK_FALSE(device.is_open());
  } else if (status == io::DeviceError::kNone) {
    // A developer machine. Opening must be undoable without leaving a stream
    // behind, since the shell opens and closes on every device change.
    CHECK(device.is_open());
    CHECK(device.actual_block_frames() > 0);
    device.close();
    CHECK_FALSE(device.is_open());
  } else {
    // A device exists but is unavailable -- exclusive mode, wrong rate, busy.
    // That must arrive as a value with a message, never as a crash.
    INFO("open failed: " << io::describe(status) << " (" << device.last_error() << ")");
    CHECK_FALSE(device.is_open());
  }
}

TEST_CASE("AudioDevice rejects a channel count it cannot service", "[unit]") {
  const engine::Engine::Config config{
      .sample_rate = 48'000, .num_channels = 2, .max_block_frames = 2'048, .seed = 0};
  engine::Engine eng{config};
  io::AudioDevice device;

  // The callback builds a fixed-size stack array; anything wider has to be
  // refused at open() rather than overrun it on the audio thread.
  io::AudioDevice::Config too_wide{};
  too_wide.num_channels = io::AudioDevice::kMaxChannels + 1;
  CHECK(device.open(eng, too_wide) == io::DeviceError::kUnsupportedChannelCount);

  io::AudioDevice::Config silent{};
  silent.num_channels = 0;
  CHECK(device.open(eng, silent) == io::DeviceError::kUnsupportedChannelCount);
}

TEST_CASE("AudioDevice refuses to start when nothing is open", "[unit]") {
  io::AudioDevice device;
  CHECK(device.start() == io::DeviceError::kNotOpen);
  CHECK_FALSE(device.is_running());

  // stop() and close() on a closed device must be no-ops, not undefined
  // behaviour: the destructor calls both unconditionally.
  device.stop();
  device.close();
  CHECK_FALSE(device.is_open());
}

TEST_CASE("AudioDevice starts with a clean xrun count", "[unit]") {
  const io::AudioDevice device;
  CHECK(device.xrun_count() == 0);
  CHECK(device.last_error().empty());
}

TEST_CASE("AudioDevice describes every error code", "[unit]") {
  const io::DeviceError codes[] = {
      io::DeviceError::kNone,        io::DeviceError::kNoDeviceAvailable,
      io::DeviceError::kUnsupportedChannelCount, io::DeviceError::kOpenFailed,
      io::DeviceError::kStartFailed, io::DeviceError::kAlreadyOpen,
      io::DeviceError::kNotOpen,
  };
  for (const io::DeviceError code : codes) {
    CHECK_FALSE(io::describe(code).empty());
    CHECK(io::describe(code) != "unknown error");
  }
}

TEST_CASE("AudioDevice plays through real hardware", "[device]") {
  // Excluded from the default ctest run: it needs a sound card, and it makes
  // noise. Run deliberately with `ctest -L device`.
  engine::Engine eng{engine::Engine::Config{
      .sample_rate = 48'000, .num_channels = 2, .max_block_frames = 2'048, .seed = 0}};
  io::AudioDevice device;

  if (!device.has_output_device()) {
    SKIP("no audio output device present");
  }

  const io::AudioDevice::Config device_config{
      .sample_rate = 48'000, .num_channels = 2, .block_frames = 256, .device_id = 0};
  const io::DeviceError opened = device.open(eng, device_config);
  INFO("open: " << io::describe(opened) << " (" << device.last_error() << ")");
  REQUIRE(opened == io::DeviceError::kNone);
  REQUIRE(device.actual_block_frames() <= eng.config().max_block_frames);

  REQUIRE(device.start() == io::DeviceError::kNone);
  CHECK(device.is_running());

  // Wait for the callback to deliver about a second of audio, but never
  // indefinitely: a device that opens and then never fires would otherwise hang
  // CI rather than reporting anything.
  const std::uint64_t before = eng.frames_rendered();
  const std::uint64_t target = before + 48'000;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (eng.frames_rendered() < target && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const std::uint64_t rendered = eng.frames_rendered();
  const std::uint64_t xruns = device.xrun_count();
  device.stop();
  device.close();

  // xruns are reported, not asserted. Whether a freshly opened stream drops its
  // first callback depends on how busy the machine is and how the driver warms
  // up, so a zero-xrun assertion here tests the CI runner's load rather than
  // this code -- and fails intermittently, which is worse than not testing it.
  // The engine's own real-time safety is established by rt_safety_test.cpp,
  // which needs no hardware at all.
  INFO("xruns during the run: " << xruns);

  CHECK(rendered >= target);
}
