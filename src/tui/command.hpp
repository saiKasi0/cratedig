#ifndef CRATEDIG_TUI_COMMAND_HPP
#define CRATEDIG_TUI_COMMAND_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
  kEdit,        // open EDIT on `slice`, 1-based, or 0 meaning the current one
  kPerform,     // back to PERFORM

  // The sequencer, M4. Transport is on the keys rather than here: starting and
  // stopping is something you do WHILE listening, and a verb you have to finish
  // typing before it takes effect is the wrong shape for that.
  kBpm,            // `bpm_x100`, already range-checked
  kSwing,          // `swing` percent
  kPatternSelect,  // `pattern`, 1-based as typed
  kPatternLength,  // `count` steps
  kPatternClear,
  kSong,       // `song`, 1-based pattern numbers in play order
  kSongClear,  // back to one pattern repeating
  kMetronome,  // `toggle`

  kQuit,
};

// `on`, `off`, or neither -- which is a request to flip whatever it is now.
//
// A tri-state rather than a bool, because the parser cannot know the current
// setting and must not guess: bare `metro` means "the other one", and squeezing
// that into a bool here would need the state the parser deliberately does not
// have.
enum class Switch : std::uint8_t {
  kToggle = 0,
  kOn,
  kOff,
};

struct Command {
  CommandKind kind = CommandKind::kNone;

  std::size_t count = 0;  // chop grid, pattern length
  std::size_t slice = 0;  // slot assign, 1-based as the user typed it

  // slot assign and pad gate/oneshot, 1-based as the user typed it. Zero means
  // "every pad", which is only reachable by leaving the number off -- see the
  // note in parse_command about why 0 is otherwise always a mistake.
  std::size_t pad = 0;

  // Pattern select, 1-based as typed.
  std::size_t pattern = 0;

  // Tempo, times 100. FIXED POINT rather than a float, for the reason
  // rt::SequencerState stores it that way: 89.5 bpm has to survive the trip
  // from the keyboard to the step arithmetic exactly, and a float bpm puts a
  // rounding into the one calculation that must not drift.
  std::uint32_t bpm_x100 = 0;

  std::uint8_t swing = 0;

  Switch toggle = Switch::kToggle;

  // The song order, 1-based pattern numbers as typed. Empty for every other
  // command, and for `song clear` -- which is kSongClear rather than an empty
  // kSong, so "no song" cannot be confused with "a song nobody filled in".
  std::vector<std::uint8_t> song;

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

// A tempo as text, two decimals, from the same fixed point the parser produces.
//
// HERE rather than beside the other formatters, so that reading a tempo and
// writing one live in the same file and cannot disagree about how many decimals
// a tempo has. No float is involved in either direction, which is what makes
// `bpm 92.5` come back as `92.50` and not `92.49`.
[[nodiscard]] std::string format_bpm(std::uint32_t bpm_x100);

}  // namespace tui

#endif  // CRATEDIG_TUI_COMMAND_HPP
