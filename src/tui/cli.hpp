#ifndef CRATEDIG_TUI_CLI_HPP
#define CRATEDIG_TUI_CLI_HPP

namespace tui {

// The non-interactive entry points: plain stdout, no terminal negotiation, no
// FTXUI. Both must work with no sound card and no file, because they are how
// someone diagnoses having neither -- starting a fullscreen UI to answer
// "which devices do I have" would be the wrong shape.

// Prints versions and, importantly, the license of the FFmpeg build actually
// linked -- see docs/LICENSING.md.
void print_version();

// Prints the available output devices, or a clear message when there are none.
[[nodiscard]] int list_devices();

}  // namespace tui

#endif  // CRATEDIG_TUI_CLI_HPP
