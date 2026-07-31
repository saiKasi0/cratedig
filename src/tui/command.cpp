#include "tui/command.hpp"

#include <charconv>
#include <cstddef>
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

}  // namespace

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
