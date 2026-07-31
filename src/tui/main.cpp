// The cratedig entry point.
//
// Argument parsing and dispatch only — everything it does lives behind
// src/tui/app.hpp, src/tui/cli.hpp, src/io/ and src/engine/, so that the same
// paths are reachable from tests and, later, from the offline renderer.

#include "tui/app.hpp"
#include "tui/cli.hpp"

#include <CLI/CLI.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace {

// The whole body lives here so main() itself can be a single try/catch.
//
// src/ingest/, FTXUI, CLI11 and the standard library all use exceptions — only
// src/rt/ and src/engine/ are built without them (CLAUDE.md). Letting one escape
// main() means std::terminate and a core dump instead of a message, which is a
// poor way to learn that a file was unreadable.
int run(int argc, char** argv) {
  CLI::App app{"cratedig — the terminal crate-digging DAW"};

  std::string sample_path;
  std::uint32_t sample_rate = 48'000;
  std::uint32_t block_frames = 256;
  unsigned int device_id = 0;
  bool no_audio = false;
  bool legacy_keys = false;
  bool want_devices = false;
  bool want_version = false;

  app.add_option("file", sample_path, "audio file to load onto pad 0")->check(CLI::ExistingFile);
  app.add_option("--sample-rate", sample_rate, "engine sample rate in Hz")->capture_default_str();
  app.add_option("--block", block_frames, "requested callback block size in frames")
      ->capture_default_str();
  app.add_option("--device", device_id, "output device id (0 = system default)")
      ->capture_default_str();
  app.add_flag("--no-audio", no_audio, "run the interface without opening an audio device");
  app.add_flag("--legacy-keys", legacy_keys,
               "never negotiate the Kitty keyboard protocol (loses gate pads)");
  app.add_flag("--list-devices", want_devices, "list audio output devices and exit");
  app.add_flag("--version", want_version, "print version, FFmpeg build and license, then exit");

  CLI11_PARSE(app, argc, argv);

  // Both of these must work with no sound card and no file, because they are how
  // someone diagnoses having neither.
  if (want_version) {
    tui::print_version();
    return 0;
  }
  if (want_devices) {
    return tui::list_devices();
  }

  // No file is a legitimate starting state, not an error: the interface renders
  // "nothing loaded" and M6's onboarding tour begins exactly there.
  const tui::AppOptions options{.sample_path = sample_path,
                                .sample_rate = sample_rate,
                                .block_frames = block_frames,
                                .device_id = device_id,
                                .no_audio = no_audio,
                                .legacy_keys = legacy_keys};
  return tui::run_app(options);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  } catch (...) {
    // Nothing in this program throws a non-std exception, but a terminal left in
    // an alternate screen buffer by an escaping unknown is a bad enough outcome
    // to be worth the three lines. FTXUI restores the terminal during unwinding
    // either way.
    std::cerr << "error: unknown failure\n";
    return 1;
  }
}
