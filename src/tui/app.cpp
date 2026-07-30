#include "tui/app.hpp"

#include "engine/engine.hpp"
#include "ingest/decoder.hpp"
#include "io/audio_device.hpp"
#include "rt/pad_event.hpp"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace tui {
namespace {

// Comfortably above anything a device will negotiate. The engine's buffers are
// sized from this at construction, before the device has told us what it
// actually granted, so it has to be a ceiling rather than a guess -- and open()
// is checked against it afterwards.
constexpr std::uint32_t kMaxBlockFrames = 8'192;

// ~30 Hz. The design is dense but calm (docs/design/DESIGN_BRIEF.md) and nothing
// on screen changes faster than the eye reads it, so a higher rate would only
// spend CPU that the audio thread would rather have.
constexpr auto kFrameInterval = std::chrono::milliseconds(33);

constexpr std::uint8_t kPad = 0;

std::string format_seconds(double seconds) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << seconds << " s";
  return out.str();
}

}  // namespace

int run_app(const AppOptions& options) {
  // Checked before anything expensive: refusing after decoding a five-minute
  // file would be a slow way to learn the output is a pipe. FTXUI redirects
  // *input* from /dev/tty when stdin is piped, but it has nowhere to send the
  // interface if stdout is not a terminal.
  if (isatty(STDOUT_FILENO) == 0) {
    std::cerr << "error: stdout is not a terminal — cratedig's interface needs one.\n"
              << "       for scripted use see: cratedig --list-devices, cratedig --version\n";
    return 1;
  }

  // 1. Load. On this thread, before anything real-time exists, which is exactly
  //    where decoding belongs.
  std::shared_ptr<const rt::Sample> sample;
  std::string sample_name;
  std::string sample_summary = "no sample loaded";

  if (!options.sample_path.empty()) {
    const ingest::SampleLoad load = ingest::load_sample(options.sample_path, options.sample_rate);
    if (!load.ok()) {
      std::cerr << "error: " << ingest::describe(load.error);
      if (!load.detail.empty()) {
        std::cerr << " (" << load.detail << ")";
      }
      std::cerr << '\n';
      return 1;
    }
    sample = load.sample;
    sample_name = options.sample_path.filename().string();

    const double seconds =
        static_cast<double>(sample->num_frames()) / static_cast<double>(sample->sample_rate());
    sample_summary = sample_name + " · " + std::to_string(sample->sample_rate()) + " Hz · " +
                     (sample->num_channels() == 1 ? "mono" : "stereo") + " · " +
                     format_seconds(seconds);
  }

  // 2. Build the engine and assign the pad. set_pad_sample() is documented as
  //    pre-start only, and this is that moment.
  engine::Engine engine{engine::Engine::Config{.sample_rate = options.sample_rate,
                                               .num_channels = 2,
                                               .max_block_frames = kMaxBlockFrames,
                                               .seed = 0}};
  if (sample != nullptr) {
    engine.set_pad_sample(kPad, sample);
  }

  // 3. Open the device, unless we were told not to.
  io::AudioDevice device;
  bool audio_running = false;

  if (!options.no_audio) {
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
      std::cerr << "\ntry: cratedig --list-devices, or --no-audio to run without one\n";
      return 1;
    }

    // The device decides the block size, not us. If it hands back something
    // larger than the engine was built for, stop -- rendering past the end of
    // the engine's contract is not something to discover as a crackle.
    if (device.actual_block_frames() > engine.config().max_block_frames) {
      std::cerr << "error: device wants " << device.actual_block_frames()
                << "-frame blocks, engine is built for " << engine.config().max_block_frames
                << '\n';
      return 1;
    }

    if (const io::DeviceError started = device.start(); started != io::DeviceError::kNone) {
      std::cerr << "error: " << io::describe(started) << " (" << device.last_error() << ")\n";
      return 1;
    }
    audio_running = true;
  }

  // 4. The interface.
  //
  // NOTE (M2 task 1): this frame is deliberately thin -- it exists so the
  // FTXUI loop, the key path and the janitor tick can be verified on their own,
  // before the real PERFORM layout from docs/design/*.html lands with the
  // waveform. It is replaced wholesale, not extended.
  ftxui::App screen = ftxui::App::Fullscreen();

  // Nothing in this interface responds to the mouse, and leaving tracking on
  // makes a terminal emit mouse escape sequences into the PTY snapshot tests.
  screen.TrackMouse(false);

  const std::string audio_summary =
      options.no_audio
          ? std::string{"no audio"}
          : device.api_name() + " · " + std::to_string(device.actual_block_frames()) + " frames";

  auto frame = ftxui::Renderer([&] {
    std::ostringstream counters;
    counters << "voices " << engine.active_voices() << '/' << engine::Engine::kMaxVoices
             << "   xruns " << device.xrun_count() << "   dropped " << engine.dropped_events()
             << '/' << engine.dropped_triggers() << "   frames " << engine.frames_rendered();

    return ftxui::vbox({
               ftxui::hbox({
                   ftxui::text(" cratedig ") | ftxui::bold,
                   ftxui::text(CRATEDIG_VERSION) | ftxui::dim,
                   ftxui::text("  perform"),
                   ftxui::filler(),
                   ftxui::text(sample_summary) | ftxui::dim,
                   ftxui::text(" "),
               }),
               ftxui::separatorEmpty(),
               ftxui::vbox({
                   ftxui::text(counters.str()),
                   ftxui::separatorEmpty(),
                   ftxui::text(sample == nullptr ? "no sample on pad 01"
                                                 : "pad 01  " + sample_name),
               }) | ftxui::borderRounded |
                   ftxui::flex,
               ftxui::hbox({
                   ftxui::text("  perform   ") | ftxui::dim,
                   ftxui::text(std::to_string(options.sample_rate) + " Hz · " + audio_summary),
                   ftxui::filler(),
                   ftxui::text("[space] trigger   [q] quit ") | ftxui::dim,
               }),
           }) |
           ftxui::flex;
  });

  auto root = ftxui::CatchEvent(frame, [&](const ftxui::Event& event) {
    // The janitor tick. The engine spawns no threads of its own, so somebody has
    // to call collect_garbage(); doing it on the frame tick keeps the whole
    // design single-writer and costs nothing when there is nothing to collect.
    if (event == ftxui::Event::Custom) {
      static_cast<void>(engine.collect_garbage());
      return false;  // let the loop redraw
    }
    if (event == ftxui::Event::Character(' ')) {
      static_cast<void>(
          engine.trigger_pad(rt::PadEvent{.pad = kPad, .velocity = 1.0F, .frame_offset = 0}));
      return true;
    }
    if (event == ftxui::Event::Character('q') || event == ftxui::Event::Escape) {
      screen.Exit();
      return true;
    }
    return false;
  });

  // The refresh thread exists because Loop() is blocking and the counters change
  // without any key being pressed. Declared after `screen` so it is joined
  // before `screen` is destroyed.
  std::atomic<bool> ticking{true};
  std::thread refresh{[&] {
    while (ticking.load(std::memory_order_relaxed)) {
      screen.PostEvent(ftxui::Event::Custom);
      std::this_thread::sleep_for(kFrameInterval);
    }
  }};

  screen.Loop(root);

  ticking.store(false, std::memory_order_relaxed);
  refresh.join();

  // 5. Stop before collecting: once the stream is stopped nothing else can
  //    retire, so this last sweep is guaranteed to drain the ring.
  if (audio_running) {
    device.stop();
    device.close();
  }
  static_cast<void>(engine.collect_garbage());

  if (device.xrun_count() > 0) {
    std::cout << device.xrun_count() << " xrun(s) during this session\n";
  }
  return 0;
}

}  // namespace tui
