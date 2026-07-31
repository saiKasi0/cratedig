#ifndef CRATEDIG_TUI_APP_HPP
#define CRATEDIG_TUI_APP_HPP

#include <cstdint>
#include <filesystem>

namespace tui {

// The interactive application: loads a sample, opens the output device, and
// runs the FTXUI event loop until the user quits.
//
// This is the M2 replacement for the termios shell M1 shipped. What survives
// from M1 is everything underneath -- the engine, the pad event ring, the voice
// pool, the device adapter and the janitor loop; only the presentation layer
// changed.

struct AppOptions {
  // Empty means "start with nothing loaded". That is a legitimate state rather
  // than an error: the interface has to be able to render it, and the snapshot
  // tests assert what it looks like.
  std::filesystem::path sample_path;

  std::uint32_t sample_rate = 48'000;
  std::uint32_t block_frames = 256;
  unsigned int device_id = 0;

  // Run the interface with no audio device at all. This is what makes the UI
  // testable under a PTY in a container, where there is no /dev/snd -- and it
  // is honest about it rather than pretending to play: the mode line says so.
  bool no_audio = false;

  // Never negotiate the Kitty keyboard protocol, even with a terminal that
  // answers the query.
  //
  // An escape hatch rather than a feature. The protocol is only enabled after
  // the terminal says it implements it, so this should never be needed -- but a
  // terminal that answers and then behaves differently would otherwise leave no
  // way to start the program at all, and "reinstall your terminal" is not an
  // answer. The cost of taking it is losing key release, which is to say gate
  // pads (src/tui/keys.hpp).
  bool legacy_keys = false;
};

// Runs until the user quits. Returns a process exit status.
[[nodiscard]] int run_app(const AppOptions& options);

}  // namespace tui

#endif  // CRATEDIG_TUI_APP_HPP
