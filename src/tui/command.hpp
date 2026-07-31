#ifndef CRATEDIG_TUI_COMMAND_HPP
#define CRATEDIG_TUI_COMMAND_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tui {

// The `:` command line.
//
// PARSING IS A PURE FUNCTION, separate from running anything. That is what lets
// every accepted spelling, every rejected one, and every error message be tested
// without a terminal, an engine or a file -- the same reason render() is a pure
// function of UiState.
//
// This is the minimal command line M3 needs, NOT the palette the mockups draw.
// The completion menu, the `:messages` browser and the fuzzy matcher belong to
// M6/M7; what is here is what `:chop transient` requires, which the ROADMAP
// names in the M3 acceptance criterion.

enum class CommandKind : std::uint8_t {
  kNone = 0,  // blank line: cancel, do nothing, say nothing
  kError,     // could not be understood; `message` says why
  kChopTransient,
  kChopGrid,  // `count` parts
  kChopReset,
  kSlotAssign,  // `slice` -> `pad`, both 1-based as typed
  kPadGate,     // `pad` 1-based, or 0 meaning every pad
  kPadOneShot,  // ditto
  kQuit,
};

struct Command {
  CommandKind kind = CommandKind::kNone;

  std::size_t count = 0;  // chop grid
  std::size_t slice = 0;  // slot assign, 1-based as the user typed it

  // slot assign and pad gate/oneshot, 1-based as the user typed it. Zero means
  // "every pad", which is only reachable by leaving the number off -- see the
  // note in parse_command about why 0 is otherwise always a mistake.
  std::size_t pad = 0;

  // For kError, what was wrong -- phrased for the mode line, so it names the
  // correct spelling rather than just refusing.
  std::string message;
};

// Parses one command line, without the leading ':'.
//
// `slice` is accepted everywhere `chop` is. The ROADMAP's acceptance criterion
// says `:chop transient` and the mockups' command palette says
// `slice transient`; rather than pick one and make the other documentation
// wrong, both work and docs/design/README.md records the discrepancy.
[[nodiscard]] Command parse_command(std::string_view line);

}  // namespace tui

#endif  // CRATEDIG_TUI_COMMAND_HPP
