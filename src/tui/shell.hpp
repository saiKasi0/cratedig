#ifndef CRATEDIG_TUI_SHELL_HPP
#define CRATEDIG_TUI_SHELL_HPP

#include <cstdint>
#include <filesystem>

namespace tui {

// ============================================================================
// TEMPORARY — M1 ONLY. M2 DELETES THIS FILE.
// ============================================================================
// The smallest thing that satisfies the M1 acceptance criterion: "the
// starter-pack kick plays from the TUI shell with no allocation in the
// callback". One pad, one key, a status line.
//
// It is deliberately not a UI. There is no layout, no component model, no
// redraw scheduling and no key map, because M2 replaces all of that with FTXUI
// components built from docs/design/*.html, backed by PTY snapshot tests that
// become the ground truth for what the interface actually is. Anything added
// here in the meantime is work that gets thrown away twice.
//
// What IS meant to survive M2 is everything underneath: the engine, the pad
// event ring, the voice pool, the device adapter and the janitor loop. This file
// only wires them together.
// ============================================================================

struct ShellOptions {
  std::filesystem::path sample_path;
  std::uint32_t sample_rate = 48'000;
  std::uint32_t block_frames = 256;
  unsigned int device_id = 0;
};

// Runs until the user quits. Returns a process exit status.
[[nodiscard]] int run_shell(const ShellOptions& options);

// Prints the available output devices, or a clear message when there are none.
[[nodiscard]] int list_devices();

// Prints versions and, importantly, the license of the FFmpeg build actually
// linked — see docs/LICENSING.md.
void print_version();

}  // namespace tui

#endif  // CRATEDIG_TUI_SHELL_HPP
