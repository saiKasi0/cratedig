#include "tui/raw_terminal.hpp"

#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <cstring>

namespace tui {
namespace {

// File-scope because a signal handler can only reach file-scope state. Only one
// RawTerminal can exist at a time, which is true by construction: there is one
// stdin.
termios g_saved_termios{};

// sig_atomic_t, not bool or std::atomic: this is read and written from a signal
// handler, and sig_atomic_t is the one type the standard guarantees is safe
// there.
volatile std::sig_atomic_t g_raw_active = 0;

void restore_terminal() noexcept {
  if (g_raw_active != 0) {
    g_raw_active = 0;
    // tcsetattr is on POSIX's async-signal-safe list, which is why the handler
    // below can call this directly rather than setting a flag and hoping the
    // main loop gets another turn. On SIGTERM it may not.
    static_cast<void>(tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios));
  }
}

extern "C" void handle_fatal_signal(int signal_number) {
  restore_terminal();

  // Re-raise with the default disposition so the process dies the way the
  // signal intended and reports the right exit status to the shell. Installed
  // with SA_RESETHAND, so this handler is already uninstalled by now.
  std::raise(signal_number);
}

void install_handler(int signal_number) noexcept {
  struct sigaction action {};

  action.sa_handler = &handle_fatal_signal;
  sigemptyset(&action.sa_mask);
  // Cast because glibc defines SA_RESETHAND as unsigned while sa_flags is int;
  // on macOS both are int, so this only shows up on the Linux build.
  action.sa_flags = static_cast<int>(SA_RESETHAND);
  static_cast<void>(sigaction(signal_number, &action, nullptr));
}

}  // namespace

RawTerminal::RawTerminal() {
  if (isatty(STDIN_FILENO) == 0) {
    return;  // piped or redirected; the caller degrades rather than fails
  }
  if (tcgetattr(STDIN_FILENO, &g_saved_termios) != 0) {
    return;
  }

  termios raw = g_saved_termios;

  // Cbreak, not fully raw: ICANON off so keys arrive without Enter, ECHO off so
  // they do not litter the display. ISIG stays ON deliberately -- Ctrl-C must
  // keep working, and the handler above makes it safe.
  raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));

  // VMIN 0 with VTIME > 0 makes read() a timed poll: it returns after VTIME
  // deciseconds even with nothing typed. That is what gives the run loop a tick
  // for redrawing and for collecting garbage.
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    return;
  }

  g_raw_active = 1;
  m_active = true;

  install_handler(SIGINT);
  install_handler(SIGTERM);
  install_handler(SIGHUP);
}

RawTerminal::~RawTerminal() {
  restore_terminal();
}

std::optional<char> RawTerminal::read_key(std::chrono::milliseconds timeout) const {
  if (!m_active) {
    return std::nullopt;
  }

  // VTIME is in deciseconds and capped at 255. Clamped to at least 1 so a caller
  // passing a very short timeout gets a poll rather than a blocking read.
  termios current{};
  if (tcgetattr(STDIN_FILENO, &current) == 0) {
    auto deciseconds = static_cast<cc_t>(timeout.count() / 100);
    if (deciseconds == 0) {
      deciseconds = 1;
    }
    if (current.c_cc[VTIME] != deciseconds) {
      current.c_cc[VTIME] = deciseconds;
      static_cast<void>(tcsetattr(STDIN_FILENO, TCSANOW, &current));
    }
  }

  char key = 0;
  const ssize_t count = read(STDIN_FILENO, &key, 1);
  if (count == 1) {
    return key;
  }
  return std::nullopt;
}

}  // namespace tui
