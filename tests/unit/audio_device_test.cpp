// The device layer, tested the way CI can actually run it: with no sound card.
//
// Docker has no /dev/snd, and the Linux run is the ROADMAP acceptance path, so
// "zero devices" is a first-class state here rather than an error. Anything that
// requires real hardware is tagged [device] and excluded from the default run --
// see docs/TESTING.md.

#include "io/audio_device.hpp"

#include "engine/engine.hpp"
#include "rt/recorder.hpp"

#include <algorithm>
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

TEST_CASE("AudioDevice enumerates inputs separately from outputs", "[unit]") {
  const io::AudioDevice device;
  const std::vector<io::DeviceInfo> inputs = device.input_devices();

  for (const io::DeviceInfo& info : inputs) {
    CHECK(info.input_channels > 0);
    CHECK_FALSE(info.name.empty());
  }
  CHECK(device.has_input_device() == !inputs.empty());

  // The two lists are FILTERS over one enumeration, not two enumerations, so a
  // duplex device appears in both and a speaker in only one. Checking the
  // relationship rather than the counts is what makes this runnable in a
  // container, where both are empty and the relationship still holds.
  const std::vector<io::DeviceInfo> outputs = device.output_devices();
  for (const io::DeviceInfo& info : outputs) {
    const bool listed_as_input =
        std::any_of(inputs.begin(), inputs.end(),
                    [&](const io::DeviceInfo& other) { return other.id == info.id; });
    CHECK(listed_as_input == (info.input_channels > 0));
  }
}

TEST_CASE("AudioDevice reports a missing input rather than opening half a stream", "[unit]") {
  const engine::Engine::Config config{
      .sample_rate = 48'000, .num_channels = 2, .max_block_frames = 2'048, .seed = 0};
  engine::Engine eng{config};
  io::AudioDevice device;

  io::AudioDevice::Config duplex{};
  duplex.sample_rate = 48'000;
  duplex.num_channels = 2;
  duplex.block_frames = 256;
  duplex.input_channels = 2;

  const io::DeviceError status = device.open(eng, duplex);

  if (!device.has_output_device()) {
    CHECK(status == io::DeviceError::kNoDeviceAvailable);
  } else if (!device.has_input_device()) {
    // ITS OWN CODE, and this is why it exists: a machine with speakers and no
    // microphone must be told that recording is unavailable, not that audio is.
    CHECK(status == io::DeviceError::kNoInputDeviceAvailable);
    CHECK_FALSE(device.is_open());
    CHECK(device.input_channels() == 0);
  } else if (status == io::DeviceError::kNone) {
    CHECK(device.is_open());
    CHECK(device.input_channels() == 2);
    device.close();
    // Closing forgets the input side too -- a stale count would have the
    // callback build channel pointers into a buffer that no longer exists.
    CHECK(device.input_channels() == 0);
  } else {
    // A MONO MICROPHONE ASKED FOR TWO CHANNELS lands here, which is the
    // ordinary case on a laptop rather than an exotic one. What matters is
    // that the refusal leaves NOTHING open: RtAudio probes the output first
    // and returns on a failed input probe without undoing it, so without the
    // cleanup in open() this reported failure and is_open() at the same time.
    INFO("duplex open failed: " << io::describe(status) << " (" << device.last_error() << ")");
    CHECK_FALSE(device.is_open());
    CHECK(device.input_channels() == 0);
  }
}

TEST_CASE("AudioDevice refuses an input wider than the callback's array", "[unit]") {
  const engine::Engine::Config config{
      .sample_rate = 48'000, .num_channels = 2, .max_block_frames = 2'048, .seed = 0};
  engine::Engine eng{config};
  io::AudioDevice device;

  // Same reasoning as the output side: the callback builds a fixed stack array
  // for the input too, so anything wider is refused at open() rather than
  // overrunning it on the audio thread.
  io::AudioDevice::Config too_wide{};
  too_wide.num_channels = 2;
  too_wide.input_channels = io::AudioDevice::kMaxChannels + 1;
  CHECK(device.open(eng, too_wide) == io::DeviceError::kUnsupportedChannelCount);
}

TEST_CASE("AudioDevice opens output only by default", "[unit]") {
  // The default has to stay what it was: every caller before M6 asked for no
  // input, and a device layer that quietly started capturing would take a
  // microphone permission nobody asked for.
  const io::AudioDevice::Config defaults{};
  CHECK(defaults.input_channels == 0);
  CHECK(defaults.input_device_id == 0);

  const io::AudioDevice device;
  CHECK(device.input_channels() == 0);
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
      io::DeviceError::kNone,
      io::DeviceError::kNoDeviceAvailable,
      io::DeviceError::kUnsupportedChannelCount,
      io::DeviceError::kOpenFailed,
      io::DeviceError::kStartFailed,
      io::DeviceError::kAlreadyOpen,
      io::DeviceError::kNotOpen,
      io::DeviceError::kNoInputDeviceAvailable,
  };
  for (const io::DeviceError code : codes) {
    CHECK_FALSE(io::describe(code).empty());
    CHECK(io::describe(code) != "unknown error");
  }
}

TEST_CASE("AudioDevice captures through real hardware", "[device]") {
  // M6's acceptance for the device layer, and it needs a sound card with a
  // capture side. Tagged [device] for that and for one more reason: opening and
  // STARTING a real input is what asks macOS for the microphone, and a default
  // test run must never pop a system dialog at somebody.
  engine::Engine eng{engine::Engine::Config{
      .sample_rate = 48'000, .num_channels = 2, .max_block_frames = 2'048, .seed = 0}};
  io::AudioDevice device;

  const std::vector<io::DeviceInfo> inputs = device.input_devices();
  if (inputs.empty() || !device.has_output_device()) {
    SKIP("no capture device present");
  }

  // Whatever the default input actually offers, capped at stereo -- the same
  // negotiation run_app() does, and for the same reason: a laptop microphone is
  // mono, and asking it for two channels fails.
  const io::DeviceInfo* chosen = &inputs.front();
  for (const io::DeviceInfo& info : inputs) {
    if (info.is_default_input) {
      chosen = &info;
      break;
    }
  }

  io::AudioDevice::Config config{};
  config.sample_rate = 48'000;
  config.num_channels = 2;
  config.block_frames = 256;
  config.input_channels = static_cast<std::uint16_t>(std::min(2U, chosen->input_channels));
  config.input_device_id = chosen->id;

  const io::DeviceError opened = device.open(eng, config);
  if (opened != io::DeviceError::kNone) {
    // Duplex across two different devices needs an aggregate device on
    // CoreAudio, and a machine may simply refuse. Reported, not failed: this
    // case is about the capture path working where the hardware allows it.
    INFO("duplex open: " << io::describe(opened) << " (" << device.last_error() << ")");
    SKIP("this machine will not open a duplex stream");
  }

  REQUIRE(device.input_channels() == config.input_channels);
  REQUIRE(device.start() == io::DeviceError::kNone);
  REQUIRE(eng.start_recording(rt::RecordSource::kInput));

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (eng.take_frames() < 24'000 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    static_cast<void>(eng.collect_take());
  }

  static_cast<void>(eng.stop_recording());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  static_cast<void>(eng.collect_take());
  const std::uint64_t callbacks = device.callback_count();
  device.stop();
  device.close();

  if (callbacks == 0) {
    SKIP(
        "the audio device accepted the stream and delivered no callbacks — "
        "this machine's audio stack is not usable right now (on macOS, "
        "`sudo killall coreaudiod` clears a wedged one)");
  }

  // Half a second of capture, through a real callback, from real hardware.
  // NOT an assertion about the CONTENT: a silent room is a legitimate
  // recording, and asserting on level here would fail on a muted microphone.
  CHECK(eng.take_frames() >= 24'000);
  CHECK(eng.telemetry().record_dropped_frames == 0);
  CHECK(eng.build_take() != nullptr);
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
  const std::uint64_t callbacks = device.callback_count();
  device.stop();
  device.close();

  // A DEVICE THAT OPENED, STARTED AND DELIVERED NOTHING is not this program
  // failing, and reporting it as one sends the reader looking in the wrong
  // place. It happens: a macOS CoreAudio left holding a stale client accepts
  // the stream, parks its IO thread and calls back never.
  //
  // Skipped rather than failed, and named rather than silent -- the same
  // contract every other [device] case here keeps (docs/TESTING.md: "a test
  // that skips loudly is more useful than one nobody remembers to invoke").
  // Before this, the run did not even get here: stop() blocked for ever
  // draining a stream with nothing in it, and the suite hung.
  if (callbacks == 0) {
    SKIP(
        "the audio device accepted the stream and delivered no callbacks — "
        "this machine's audio stack is not usable right now (on macOS, "
        "`sudo killall coreaudiod` clears a wedged one)");
  }

  // xruns are reported, not asserted. Whether a freshly opened stream drops its
  // first callback depends on how busy the machine is and how the driver warms
  // up, so a zero-xrun assertion here tests the CI runner's load rather than
  // this code -- and fails intermittently, which is worse than not testing it.
  // The engine's own real-time safety is established by rt_safety_test.cpp,
  // which needs no hardware at all.
  INFO("xruns during the run: " << xruns);

  CHECK(rendered >= target);
}
