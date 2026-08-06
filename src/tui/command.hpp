#ifndef CRATEDIG_TUI_COMMAND_HPP
#define CRATEDIG_TUI_COMMAND_HPP

#include "ingest/onset.hpp"

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
  kChopTune,    // open CHOP, the live re-chop preview
  kSlotAssign,  // `slice`..`slice_last` -> `pad`.., all 1-based as typed
  kPadGate,     // `pad` 1-based, or 0 meaning every pad
  kPadOneShot,  // ditto
  kEdit,        // open EDIT on `slice`, 1-based, or 0 meaning the current one
  kPerform,     // back to PERFORM

  // The sequencer, M4. Transport is on the keys rather than here: starting and
  // stopping is something you do WHILE listening, and a verb you have to finish
  // typing before it takes effect is the wrong shape for that.
  kBpm,            // `bpm_x100`, already range-checked
  kBpmDetect,      // read the tempo off the file that is showing
  kTapeSpeed,      // `decibels` carries the ratio; scales bpm_x100
  kSwing,          // `swing` percent
  kPatternSelect,  // `pattern`, 1-based as typed
  kPatternLength,  // `count` steps
  kPatternClear,
  kSong,       // `song`, 1-based pattern numbers in play order
  kSongClear,  // back to one pattern repeating
  kMetronome,  // `toggle`

  // Playing a pattern in rather than typing it, M6.
  kRecordArm,       // `toggle`
  kRecordQuantise,  // `count` is the note DENOMINATOR as typed: 16, 8, 4 or 2
  kRecordReplace,   // `toggle`: on replaces the pattern, off overdubs onto it
  kRecordUndo,      // put the pattern back as it was before the take

  // Recording AUDIO, M6. A different feature from the four above and therefore
  // a different word.
  //
  // `rec` plays a pattern in; `capture` records sound. Calling both of them
  // "record" is what cost this milestone two commits built against the wrong
  // reading of one sentence, so they do not share a verb, a key or a noun.
  kCaptureStart,   // `toggle`: bare flips it, on starts, off stops and keeps
  kCaptureSource,  // `text` is "master" or "input"
  kCaptureArm,     // `decibels` is the level that starts it by itself
  kCaptureDrop,    // stop and throw the take away

  // Stop sounding voices. `pad` is 1-based as typed, or 0 for all of them --
  // and unlike `pad gate`, "all" here also stops the transport, because a
  // running sequencer would retrigger within a step and the silence would last
  // a fraction of a beat.
  kStop,

  // The mixer, M5. `pad` is 1-based as typed throughout, matching every other
  // verb here; `bus` is 0-based because a-d is not a number anyone types twice.
  kMix,        // open MIX; `pattern` reused as the page, 0 = channels, 1 = buses
  kStripGain,  // `pad` (or `bus` when `bus_target`), `decibels`
  kStripPan,   // `pad`, `pan_percent` in [-100, 100]
  kStripMute,  // `pad`, `toggle`
  kStripSolo,  // `pad`, `toggle`
  kStripBus,   // `pad` -> `bus`
  kStripEq,    // `pad`, `band`, and either `toggle == kOff` or the three values
  kStripComp,  // `pad`, and either `toggle == kOff` or the four values
  kLimiter,    // `toggle`, and `decibels` as the ceiling when turning it on

  // The crate, M5.5. `:load` ADDS rather than replaces, which is the whole
  // point: a session can hold several records at once.
  kLoadFile,    // `text` is the path, exactly as typed
  kSelectFile,  // `file`, 1-based as listed
  kUnloadFile,  // `file`, or 0 meaning whichever is showing
  kListFiles,
  kBrowse,  // open BROWSE; `text` is a directory to start in, or empty

  // The pad envelope, which the engine has honoured since M3 and nothing could
  // set. `pad`, `text` naming the segment, and `decibels` or `attack_ms`
  // carrying the value.
  kPadEnvelope,

  // Playback speed for one pad. `decibels` carries the RATIO -- the field is
  // reused rather than a fourth float added, and the parser has already turned
  // semitones into one, so nothing downstream needs to know which was typed.
  kPadPitch,

  // Play a pad's slice backwards. `toggle` as everywhere else -- bare flips it.
  kPadReverse,

  kQuit,
};

// Where a threshold-armed capture starts by itself, when no level is typed.
//
// -24 dBFS: comfortably above the noise floor of anything worth sampling and
// comfortably below the level a record actually plays at, which is what a
// threshold has to sit between to be useful rather than merely present.
inline constexpr float kDefaultCaptureThresholdDb = -24.0F;

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

  // How fine `chop transient` should cut. Defaults to the finest, which is what
  // the verb has always done, so `:chop transient` alone is unchanged.
  ingest::ChopDensity density = ingest::ChopDensity::kStrum;
  std::size_t slice = 0;  // slot assign, 1-based as the user typed it

  // The last slice of a `slot assign` range, 1-based and INCLUSIVE. Equal to
  // `slice` for a single assignment, so every consumer walks a range and there
  // is no second code path for the one-slice case.
  std::size_t slice_last = 0;

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

  // -- the mixer -------------------------------------------------------------
  //
  // Denominated the way a person types them: decibels, percent, a bus letter.
  // The conversion to rt::StripConfig's linear gains and [-1, 1] balance happens
  // in the app, which is where the engine is -- the parser's job is to decide
  // what was MEANT, and "-6 dB" is what was meant.
  float decibels = 0.0F;
  int pan_percent = 0;

  // 0-based, so it indexes rt::kNumBuses directly. `bus_target` says the command
  // is ABOUT a bus rather than about a pad, which `gain` needs and the others do
  // not: `bus 3 a` routes pad 3, `gain bus a -6` moves bus A's own fader.
  std::uint8_t bus = 0;
  bool bus_target = false;

  // EQ band, 1-based as typed. 1 is the low shelf, 4 the high shelf.
  std::uint8_t band = 0;

  // EQ: frequency, gain and shape (Q for the peaking bands, S for the shelves).
  float frequency = 0.0F;
  float shape = 0.0F;

  // Compressor, in the units MIXER.md states them in. Times are milliseconds
  // here and frames in rt::CompressorConfig, because a person thinks in
  // milliseconds and the DSP must not depend on the block size.
  float ratio = 1.0F;
  float knee_db = 0.0F;
  float makeup_db = 0.0F;
  float attack_ms = 0.0F;
  float release_ms = 0.0F;

  // Which loaded file, 1-based as the crate lists them. Zero means "the one
  // showing", which is the only sensible default for `unload`.
  std::size_t file = 0;

  // A path, or any other argument that is text rather than a number.
  //
  // TAKEN AS THE REST OF THE LINE, not as a word: `~/Music/my breaks/loop 3.wav`
  // is a perfectly ordinary path and splitting it on spaces would make it four
  // arguments and one confusing error. Trailing whitespace is trimmed; nothing
  // else is interpreted, because the parser has no filesystem and expanding `~`
  // or a glob here would be guessing on behalf of a caller that can actually
  // look.
  std::string text;

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
