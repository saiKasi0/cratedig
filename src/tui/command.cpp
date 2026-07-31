#include "tui/command.hpp"

#include "rt/sequencer.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tui {
namespace {

[[nodiscard]] std::vector<std::string_view> split_words(std::string_view line) {
  std::vector<std::string_view> words;
  std::size_t index = 0;
  while (index < line.size()) {
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
      ++index;
    }
    const std::size_t start = index;
    while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
      ++index;
    }
    if (index > start) {
      words.push_back(line.substr(start, index - start));
    }
  }
  return words;
}

// from_chars rather than stoi: no exceptions (this compiles in a lane that has
// them, but the engine lane does not and the habit is worth keeping), no locale,
// and it rejects "12abc" instead of quietly reading 12.
[[nodiscard]] bool parse_number(std::string_view text, std::size_t& out) {
  std::size_t value = 0;
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto [pointer, code] = std::from_chars(first, last, value);
  if (code != std::errc{} || pointer != last) {
    return false;
  }
  out = value;
  return true;
}

// A named constructor, because designated initializers are not usable here:
// `message` is the last field and has no default, so `Command{.kind = ...}` is
// -Wmissing-field-initializers on Linux clang, and giving it a `{}` default
// instead trips clang-tidy's readability-redundant-member-init. Both complaints
// are about the same one field, and this answers both.
[[nodiscard]] Command command_of(CommandKind kind) {
  Command out;
  out.kind = kind;
  return out;
}

[[nodiscard]] Command error(std::string message) {
  Command out = command_of(CommandKind::kError);
  out.message = std::move(message);
  return out;
}

[[nodiscard]] Command parse_chop(const std::vector<std::string_view>& words) {
  if (words.size() < 2) {
    return error("chop what? try: chop transient, chop grid 16, chop reset");
  }
  const std::string_view what = words[1];

  if (what == "transient") {
    return command_of(CommandKind::kChopTransient);
  }
  if (what == "reset") {
    return command_of(CommandKind::kChopReset);
  }
  if (what == "grid") {
    if (words.size() < 3) {
      return error("chop grid needs a count, e.g. chop grid 16");
    }
    std::size_t count = 0;
    if (!parse_number(words[2], count) || count == 0) {
      return error("chop grid: " + std::string{words[2]} + " is not a count");
    }
    // A thousand slices of a five-minute file is four slices per pad bank per
    // second, which is not a chop anyone meant to ask for. Refused with the
    // limit named rather than silently clamped, so a typed extra zero is
    // visible rather than mysterious.
    if (count > 512) {
      return error("chop grid: " + std::string{words[2]} + " is more than 512 slices");
    }
    Command out = command_of(CommandKind::kChopGrid);
    out.count = count;
    return out;
  }
  return error("unknown chop: " + std::string{what});
}

[[nodiscard]] Command parse_slot(const std::vector<std::string_view>& words) {
  if (words.size() < 2 || words[1] != "assign") {
    return error("slot what? try: slot assign 5 3");
  }
  if (words.size() < 4) {
    return error("slot assign needs a slice and a pad, e.g. slot assign 5 3");
  }
  std::size_t slice = 0;
  std::size_t pad = 0;
  if (!parse_number(words[2], slice) || slice == 0) {
    return error("slot assign: " + std::string{words[2]} + " is not a slice number");
  }
  if (!parse_number(words[3], pad) || pad == 0) {
    return error("slot assign: " + std::string{words[3]} + " is not a pad number");
  }
  Command out = command_of(CommandKind::kSlotAssign);
  out.slice = slice;
  out.pad = pad;
  return out;
}

// `pad gate [N]` / `pad oneshot [N]`.
//
// The pad number is OPTIONAL and means "all sixteen" when left off, because
// gate is a way of playing rather than a property of one chop -- someone who
// wants held pads wants them on the bank, not on pad 7. Naming one is still
// allowed, since a bank with one sustaining pad in it is a real thing to want.
[[nodiscard]] Command parse_pad(const std::vector<std::string_view>& words) {
  if (words.size() < 2) {
    return error("pad what? try: pad gate, pad oneshot, pad gate 3");
  }
  const std::string_view what = words[1];
  if (what != "gate" && what != "oneshot") {
    return error("unknown pad mode: " + std::string{what} + " (gate or oneshot)");
  }

  Command out = command_of(what == "gate" ? CommandKind::kPadGate : CommandKind::kPadOneShot);
  if (words.size() >= 3) {
    std::size_t pad = 0;
    if (!parse_number(words[2], pad) || pad == 0) {
      return error("pad " + std::string{what} + ": " + std::string{words[2]} +
                   " is not a pad number");
    }
    out.pad = pad;
  }
  return out;
}

// Tempo as a fixed-point hundredth, from `120`, `92.5` or `89.53`.
//
// REFUSED RATHER THAN ROUNDED when there is more precision than a hundredth.
// A tempo silently rounded to something else is a tempo the player did not set,
// and the difference shows up as drift against another machine four minutes
// later rather than as anything visible at the moment of typing.
[[nodiscard]] bool parse_bpm_x100(std::string_view text, std::uint32_t& out) {
  const std::size_t dot = text.find('.');
  const std::string_view whole = text.substr(0, dot);
  std::size_t units = 0;
  if (!parse_number(whole, units)) {
    return false;
  }
  // Far above any tempo, and here only so the multiply below cannot overflow on
  // a pasted twenty-digit number. Out-of-range tempos are refused by the caller,
  // which can name the actual limits.
  if (units > 100'000) {
    return false;
  }

  std::size_t hundredths = 0;
  if (dot != std::string_view::npos) {
    const std::string_view fraction = text.substr(dot + 1);
    if (fraction.empty() || fraction.size() > 2) {
      return false;
    }
    std::size_t value = 0;
    if (!parse_number(fraction, value)) {
      return false;
    }
    // `.5` is five tenths, `.05` is five hundredths. Reading both as 5 would
    // make `bpm 92.5` mean 92.05 -- a whole different tempo, quietly.
    hundredths = fraction.size() == 1 ? value * 10 : value;
  }

  out = static_cast<std::uint32_t>((units * 100) + hundredths);
  return true;
}

[[nodiscard]] Command parse_bpm(const std::vector<std::string_view>& words) {
  if (words.size() < 2) {
    return error("bpm needs a tempo, e.g. bpm 92.5");
  }
  std::uint32_t bpm = 0;
  if (!parse_bpm_x100(words[1], bpm)) {
    return error("bpm: " + std::string{words[1]} + " is not a tempo (try 92 or 92.5)");
  }
  // The bounds are named from the constants rather than spelled out, so the
  // message cannot outlive them.
  if (bpm < rt::kMinBpmX100 || bpm > rt::kMaxBpmX100) {
    return error("bpm: " + std::string{words[1]} + " is outside " + format_bpm(rt::kMinBpmX100) +
                 "-" + format_bpm(rt::kMaxBpmX100));
  }
  Command out = command_of(CommandKind::kBpm);
  out.bpm_x100 = bpm;
  return out;
}

[[nodiscard]] Command parse_swing(const std::vector<std::string_view>& words) {
  if (words.size() < 2) {
    return error("swing needs a percentage, e.g. swing 58");
  }
  std::size_t percent = 0;
  if (!parse_number(words[1], percent)) {
    return error("swing: " + std::string{words[1]} + " is not a percentage");
  }
  // ZERO IS VALID HERE, unlike every count and index in this file: straight is a
  // setting, not an absent one, and `swing 0` is how you take swing back off.
  if (percent > rt::kMaxSwingPercent) {
    return error("swing: " + std::string{words[1]} + " is more than " +
                 std::to_string(rt::kMaxSwingPercent) + "%");
  }
  Command out = command_of(CommandKind::kSwing);
  out.swing = static_cast<std::uint8_t>(percent);
  return out;
}

// `pattern N` / `pattern length N` / `pattern clear`.
//
// RANGE-CHECKED HERE, where `slot assign` deliberately is not. The difference is
// what the limit depends on: how many slices exist is program state the parser
// must not need, while how many patterns there are is a compile-time constant.
// Checking against a constant keeps parsing a pure function and lets the message
// name the number.
[[nodiscard]] Command parse_pattern(const std::vector<std::string_view>& words) {
  if (words.size() < 2) {
    return error("pattern what? try: pattern 3, pattern length 16, pattern clear");
  }
  const std::string_view what = words[1];

  if (what == "clear") {
    return command_of(CommandKind::kPatternClear);
  }
  if (what == "length") {
    if (words.size() < 3) {
      return error("pattern length needs a step count, e.g. pattern length 16");
    }
    std::size_t count = 0;
    if (!parse_number(words[2], count) || count == 0) {
      return error("pattern length: " + std::string{words[2]} + " is not a step count");
    }
    if (count > rt::kMaxSteps) {
      return error("pattern length: " + std::string{words[2]} + " is more than " +
                   std::to_string(rt::kMaxSteps) + " steps");
    }
    Command out = command_of(CommandKind::kPatternLength);
    out.count = count;
    return out;
  }

  std::size_t number = 0;
  if (!parse_number(what, number) || number == 0) {
    return error("pattern: " + std::string{what} + " is not a pattern number");
  }
  if (number > rt::kMaxPatterns) {
    return error("pattern: no pattern " + std::string{what} + " (have " +
                 std::to_string(rt::kMaxPatterns) + ")");
  }
  Command out = command_of(CommandKind::kPatternSelect);
  out.pattern = number;
  return out;
}

// `song 1 2 3 1` / `song clear`.
//
// The only verb where TRAILING JUNK IS FATAL. Everywhere else an extra word is
// ignored, because the command was already complete and throwing it away over a
// stray keystroke is worse; here every word after the verb is an argument, so
// `song 1 2 x` would silently become a two-slot song rather than the four-slot
// one that was being typed.
[[nodiscard]] Command parse_song(const std::vector<std::string_view>& words) {
  if (words.size() < 2) {
    return error("song what? try: song 1 2 3, song clear");
  }
  if (words[1] == "clear") {
    return command_of(CommandKind::kSongClear);
  }

  Command out = command_of(CommandKind::kSong);
  out.song.reserve(words.size() - 1);
  for (std::size_t index = 1; index < words.size(); ++index) {
    if (out.song.size() >= rt::kMaxSongSlots) {
      return error("song: more than " + std::to_string(rt::kMaxSongSlots) + " slots");
    }
    std::size_t number = 0;
    if (!parse_number(words[index], number) || number == 0 || number > rt::kMaxPatterns) {
      return error("song: " + std::string{words[index]} + " is not a pattern number (1-" +
                   std::to_string(rt::kMaxPatterns) + ")");
    }
    out.song.push_back(static_cast<std::uint8_t>(number));
  }
  return out;
}

[[nodiscard]] Command parse_metro(const std::vector<std::string_view>& words) {
  Command out = command_of(CommandKind::kMetronome);
  if (words.size() < 2) {
    return out;  // bare `metro` flips it, which is what a click track is for
  }
  if (words[1] == "on") {
    out.toggle = Switch::kOn;
    return out;
  }
  if (words[1] == "off") {
    out.toggle = Switch::kOff;
    return out;
  }
  return error("metro: " + std::string{words[1]} + " is not on or off");
}

}  // namespace

std::string format_bpm(std::uint32_t bpm_x100) {
  const std::uint32_t hundredths = bpm_x100 % 100;
  return std::to_string(bpm_x100 / 100) + "." + (hundredths < 10 ? "0" : "") +
         std::to_string(hundredths);
}

Command parse_command(std::string_view line) {
  const std::vector<std::string_view> words = split_words(line);
  if (words.empty()) {
    return Command{};  // an empty line is a cancel, not a mistake
  }

  const std::string_view verb = words[0];

  // `slice` is accepted wherever `chop` is -- see the header for why both.
  if (verb == "chop" || verb == "slice") {
    return parse_chop(words);
  }
  if (verb == "slot") {
    return parse_slot(words);
  }
  if (verb == "pad") {
    return parse_pad(words);
  }
  if (verb == "bpm") {
    return parse_bpm(words);
  }
  if (verb == "swing") {
    return parse_swing(words);
  }
  if (verb == "pattern") {
    return parse_pattern(words);
  }
  if (verb == "song") {
    return parse_song(words);
  }
  if (verb == "metro") {
    return parse_metro(words);
  }
  // `edit` with no number opens whichever slice is already selected, which is
  // what you want after `[`/`]` in PERFORM; with one, it jumps.
  if (verb == "edit") {
    if (words.size() < 2) {
      return command_of(CommandKind::kEdit);
    }
    std::size_t slice = 0;
    if (!parse_number(words[1], slice) || slice == 0) {
      return error("edit: " + std::string{words[1]} + " is not a slice number");
    }
    Command out = command_of(CommandKind::kEdit);
    out.slice = slice;
    return out;
  }
  if (verb == "perform") {
    return command_of(CommandKind::kPerform);
  }
  if (verb == "q" || verb == "quit") {
    return command_of(CommandKind::kQuit);
  }

  return error("unknown command: " + std::string{verb});
}

}  // namespace tui
