#include "tui/shell.hpp"

#include "engine/engine.hpp"
#include "ingest/decoder.hpp"
#include "io/audio_device.hpp"
#include "rt/pad_event.hpp"
#include "tui/raw_terminal.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace tui {
namespace {

// Comfortably above anything a device will negotiate. The engine's buffers are
// sized from this at construction, before the device has told us what it
// actually granted, so it has to be a ceiling rather than a guess -- and open()
// is checked against it afterwards.
constexpr std::uint32_t kMaxBlockFrames = 8'192;

constexpr char kQuitKey = 'q';
constexpr char kTriggerKey = ' ';
constexpr std::uint8_t kPad = 0;

void print_status(const engine::Engine& engine, const io::AudioDevice& device) {
  // \r and no newline: the status line rewrites itself in place. This is the
  // entire extent of this shell's "rendering".
  std::cout << "\r  voices " << engine.active_voices() << '/' << engine::Engine::kMaxVoices
            << "   xruns " << device.xrun_count() << "   dropped " << engine.dropped_events() << '/'
            << engine.dropped_triggers() << "   frames " << engine.frames_rendered() << "        "
            << std::flush;
}

}  // namespace

void print_version() {
  std::cout << "cratedig 0.0.1 — the terminal crate-digging DAW\n";
  std::cout << "  " << ingest::ffmpeg_versions() << '\n';

  // Printed because docs/LICENSING.md scopes the LGPL requirement to distributed
  // binaries and enforces it at configure time with
  // CRATEDIG_REQUIRE_LGPL_FFMPEG. This is the runtime half: which FFmpeg is
  // actually linked should be observable, not assumed.
  std::cout << "  FFmpeg license: " << ingest::ffmpeg_license() << '\n';
  if (!ingest::ffmpeg_license().starts_with("LGPL")) {
    std::cout << "  note: this is a GPL FFmpeg build. Fine for local use — nothing is being\n"
              << "        redistributed — but a CRATEDIG binary distributed against it would\n"
              << "        be GPL rather than Apache-2.0. See docs/LICENSING.md.\n";
  }
}

int list_devices() {
  const io::AudioDevice device;
  const std::vector<io::DeviceInfo> devices = device.output_devices();

  std::cout << "audio API: " << device.api_name() << '\n';
  if (devices.empty()) {
    // Not an error status: a container legitimately has none, and `cratedig
    // --list-devices` succeeding with an empty list is more useful in a script
    // than a non-zero exit.
    std::cout << "no output devices found\n";
    return 0;
  }

  for (const io::DeviceInfo& info : devices) {
    std::cout << "  [" << info.id << "] " << info.name << "  " << info.output_channels << " ch, "
              << info.preferred_sample_rate << " Hz"
              << (info.is_default_output ? "  (default)" : "") << '\n';
  }
  return 0;
}

int run_shell(const ShellOptions& options) {
  std::cout << "cratedig 0.0.1\n";

  // 1. Load. This happens on this thread, before anything real-time exists,
  //    which is exactly where decoding belongs.
  std::cout << "loading " << options.sample_path.string() << " ...\n";
  const ingest::SampleLoad load = ingest::load_sample(options.sample_path, options.sample_rate);
  if (!load.ok()) {
    std::cerr << "error: " << ingest::describe(load.error);
    if (!load.detail.empty()) {
      std::cerr << " (" << load.detail << ")";
    }
    std::cerr << '\n';
    return 1;
  }

  const double seconds = static_cast<double>(load.sample->num_frames()) /
                         static_cast<double>(load.sample->sample_rate());
  std::cout << "  " << std::fixed << std::setprecision(2) << seconds << " s, "
            << load.sample->sample_rate() << " Hz, "
            << (load.sample->num_channels() == 1 ? "mono" : "stereo") << '\n';

  // 2. Build the engine and assign the pad. set_pad_sample() is documented as
  //    pre-start only, and this is that moment.
  engine::Engine engine{engine::Engine::Config{.sample_rate = options.sample_rate,
                                               .num_channels = 2,
                                               .max_block_frames = kMaxBlockFrames,
                                               .seed = 0}};
  engine.set_pad_sample(kPad, load.sample);

  // 3. Open the device.
  io::AudioDevice device;
  const io::AudioDevice::Config device_config{.sample_rate = options.sample_rate,
                                              .num_channels = 2,
                                              .block_frames = options.block_frames,
                                              .device_id = options.device_id};
  const io::DeviceError opened = device.open(engine, device_config);
  if (opened != io::DeviceError::kNone) {
    std::cerr << "error: " << io::describe(opened);
    if (!device.last_error().empty()) {
      std::cerr << " (" << device.last_error() << ")";
    }
    std::cerr << "\ntry: cratedig --list-devices\n";
    return 1;
  }

  // The device decides the block size, not us. If it hands back something larger
  // than the engine was built for, stop -- rendering past the end of the engine's
  // contract is not something to discover as a crackle.
  if (device.actual_block_frames() > engine.config().max_block_frames) {
    std::cerr << "error: device wants " << device.actual_block_frames()
              << "-frame blocks, engine is built for " << engine.config().max_block_frames << '\n';
    return 1;
  }

  if (const io::DeviceError started = device.start(); started != io::DeviceError::kNone) {
    std::cerr << "error: " << io::describe(started) << " (" << device.last_error() << ")\n";
    return 1;
  }

  std::cout << "  " << device.api_name() << ", " << device.actual_block_frames() << " frames, "
            << options.sample_rate << " Hz\n\n";

  const RawTerminal terminal;
  if (!terminal.active()) {
    // Not a terminal: playing would produce a program that cannot be stopped by
    // a keystroke and holds the sound card until killed.
    std::cerr << "error: stdin is not a terminal — the interactive shell needs one.\n"
              << "       (M2 adds an offline render path for scripted use.)\n";
    device.stop();
    device.close();
    return 1;
  }

  std::cout << "  [space] trigger pad " << static_cast<int>(kPad) << "   [q] quit\n\n";

  // 4. The run loop. This thread is also M1's janitor -- the engine spawns no
  //    threads of its own, so somebody has to call collect_garbage(), and doing
  //    it here keeps the whole design single-writer.
  bool running = true;
  while (running) {
    const std::optional<char> key = terminal.read_key(std::chrono::milliseconds(100));
    if (key.has_value()) {
      switch (*key) {
        case kTriggerKey:
          if (!engine.trigger_pad(rt::PadEvent{.pad = kPad, .velocity = 1.0F, .frame_offset = 0})) {
            // The ring is full, which at human typing speed means the audio
            // thread has stopped draining it. Worth seeing rather than swallowing.
            std::cerr << "\nwarning: event ring full, hit dropped\n";
          }
          break;
        case kQuitKey:
          running = false;
          break;
        default:
          break;
      }
    }

    static_cast<void>(engine.collect_garbage());
    print_status(engine, device);
  }

  std::cout << "\n";

  // 5. Stop before collecting: once the stream is stopped nothing else can
  //    retire, so this last sweep is guaranteed to drain the ring.
  device.stop();
  device.close();
  static_cast<void>(engine.collect_garbage());

  if (device.xrun_count() > 0) {
    std::cout << device.xrun_count() << " xrun(s) during this session\n";
  }
  return 0;
}

}  // namespace tui
