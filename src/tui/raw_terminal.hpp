#ifndef CRATEDIG_TUI_RAW_TERMINAL_HPP
#define CRATEDIG_TUI_RAW_TERMINAL_HPP

#include <chrono>
#include <optional>

namespace tui {

// ============================================================================
// TEMPORARY — M1 ONLY. M2 DELETES THIS FILE.
// ============================================================================
// This is a placeholder so M1 can satisfy "the starter-pack kick plays from the
// TUI shell" without pulling FTXUI forward from M2. It is throwaway scaffolding,
// not a foundation: do not build features on it, and do not grow it into a
// widget layer.
//
// M2 replaces src/tui/ with real FTXUI components built from docs/design/*.html
// (see the caveat in CLAUDE.md about treating those mockups as layout reference
// only), and adds the PTY snapshot tests that become the ground truth for what
// the UI actually is. When that lands, this file and shell.{hpp,cpp} go away.
// ============================================================================

// Puts the terminal into cbreak mode -- keys arrive immediately, without
// waiting for Enter, and without being echoed -- and restores it on destruction.
//
// Restoring also happens on SIGINT and SIGTERM. A program that leaves a terminal
// in raw mode when it dies leaves the user with a shell that does not echo and
// does not respond to Ctrl-C, which they then have to fix with a blind `reset`.
// Ctrl-C during playback is the single most likely way this program will ever be
// exited, so that path has to work.
class RawTerminal {
 public:
  RawTerminal();
  ~RawTerminal();

  RawTerminal(const RawTerminal&) = delete;
  RawTerminal& operator=(const RawTerminal&) = delete;
  RawTerminal(RawTerminal&&) = delete;
  RawTerminal& operator=(RawTerminal&&) = delete;

  // False when stdin is not a terminal (a pipe, a CI runner). The caller is
  // expected to degrade rather than fail.
  [[nodiscard]] bool active() const noexcept { return m_active; }

  // Returns the next key, or nullopt if none arrived before the timeout. The
  // timeout is what lets the caller redraw its status line and run the janitor
  // while nobody is typing.
  //
  // Resolution is 1/10 s: this is implemented with termios VTIME rather than
  // poll(), because VTIME is exactly this and needs no second syscall.
  [[nodiscard]] std::optional<char> read_key(std::chrono::milliseconds timeout) const;

 private:
  bool m_active = false;
};

}  // namespace tui

#endif  // CRATEDIG_TUI_RAW_TERMINAL_HPP
