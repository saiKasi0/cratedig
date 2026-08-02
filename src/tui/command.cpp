#include "tui/command.hpp"

#include "rt/limiter.hpp"
#include "rt/sequencer.hpp"
#include "rt/strip.hpp"

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

// `N` or `N-M`, 1-based and inclusive. A single number is the one-element range,
// so callers never need to tell the two apart.
//
// Reversed ranges are refused rather than normalised: `8-1` is much more likely
// to be a typo than a request to assign backwards, and silently reversing it
// would put slice 8 on pad 1 while the line says the opposite.
[[nodiscard]] bool parse_range(std::string_view text, std::size_t& first, std::size_t& last) {
  const std::size_t dash = text.find('-');
  if (dash == std::string_view::npos) {
    if (!parse_number(text, first) || first == 0) {
      return false;
    }
    last = first;
    return true;
  }
  if (!parse_number(text.substr(0, dash), first) || first == 0) {
    return false;
  }
  if (!parse_number(text.substr(dash + 1), last) || last == 0) {
    return false;
  }
  return last >= first;
}

// `slot assign S P`, where either side may be a range.
//
// `1-8 1` fills upward from a starting pad, which is the form that makes a chop
// land on a bank in one line. `1-8 1-8` says the same thing explicitly, and a
// length mismatch is refused with BOTH counts named -- silently truncating would
// leave pads holding whatever they held before, which looks like the command
// half-worked rather than like it was wrong.
[[nodiscard]] Command parse_slot(const std::vector<std::string_view>& words) {
  if (words.size() < 2 || words[1] != "assign") {
    return error("slot what? try: slot assign 5 3, or slot assign 1-8 1");
  }
  if (words.size() < 4) {
    return error("slot assign needs a slice and a pad, e.g. slot assign 5 3");
  }

  std::size_t slice = 0;
  std::size_t slice_last = 0;
  if (!parse_range(words[2], slice, slice_last)) {
    return error("slot assign: " + std::string{words[2]} + " is not a slice or a range");
  }

  std::size_t pad = 0;
  std::size_t pad_last = 0;
  if (!parse_range(words[3], pad, pad_last)) {
    return error("slot assign: " + std::string{words[3]} + " is not a pad or a range");
  }

  // A single pad is a STARTING pad when the slices are a range: `1-8 1` fills
  // pads 1 to 8. That is why the pad range is not carried in the Command -- it
  // is always as long as the slice range, and app.cpp walks from `pad`.
  const std::size_t slices = slice_last - slice + 1;
  const std::size_t pads = pad_last - pad + 1;
  if (pads != 1 && pads != slices) {
    return error("slot assign: " + std::to_string(slices) + " slices into " + std::to_string(pads) +
                 " pads");
  }

  Command out = command_of(CommandKind::kSlotAssign);
  out.slice = slice;
  out.slice_last = slice_last;
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

// `stop` / `stop N`.
//
// Bare `stop` is everything AND the transport; `stop N` is one pad and leaves
// the transport alone. The asymmetry is deliberate: "stop everything" is a
// panic and has to actually produce silence, while "stop pad 3" is surgery on
// something still running.
[[nodiscard]] Command parse_stop(const std::vector<std::string_view>& words) {
  Command out = command_of(CommandKind::kStop);
  if (words.size() < 2) {
    return out;  // all of them, and the transport with them
  }
  std::size_t pad = 0;
  if (!parse_number(words[1], pad) || pad == 0) {
    return error("stop: " + std::string{words[1]} + " is not a pad number");
  }
  if (pad > rt::kNumPads) {
    return error("stop: no pad " + std::string{words[1]} + " (have " +
                 std::to_string(rt::kNumPads) + ")");
  }
  out.pad = pad;
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

// -- the mixer ---------------------------------------------------------------

// A signed decimal, for the values a mixer is denominated in: "-6", "3.5",
// "-0.25". Refused rather than clamped -- the caller names the real limits, so
// the message can say what the range actually is.
[[nodiscard]] bool parse_decimal(std::string_view text, float& out) {
  if (text.empty()) {
    return false;
  }
  const bool negative = text.front() == '-';
  if (negative || text.front() == '+') {
    text.remove_prefix(1);
  }
  const std::size_t dot = text.find('.');
  std::size_t units = 0;
  if (!parse_number(text.substr(0, dot), units) || units > 1'000'000) {
    return false;
  }

  double value = static_cast<double>(units);
  if (dot != std::string_view::npos) {
    const std::string_view fraction = text.substr(dot + 1);
    if (fraction.empty() || fraction.size() > 4) {
      return false;
    }
    std::size_t digits = 0;
    if (!parse_number(fraction, digits)) {
      return false;
    }
    double scale = 1.0;
    for (std::size_t index = 0; index < fraction.size(); ++index) {
      scale *= 10.0;
    }
    value += static_cast<double>(digits) / scale;
  }

  out = static_cast<float>(negative ? -value : value);
  return true;
}

// A bus, as the letter the screen shows. 0-based on the way out.
[[nodiscard]] bool parse_bus_letter(std::string_view text, std::uint8_t& out) {
  if (text.size() != 1) {
    return false;
  }
  const char letter = text.front();
  const char lower =
      letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
  if (lower < 'a' || lower >= static_cast<char>('a' + rt::kNumBuses)) {
    return false;
  }
  out = static_cast<std::uint8_t>(lower - 'a');
  return true;
}

// A pad, 1-based as typed. Zero is refused rather than read as "all": on the
// mixer every verb acts on one strip, and `mute 0` is a typo for `mute 10` far
// more often than it is a request to mute everything.
[[nodiscard]] bool parse_pad_number(std::string_view text, std::size_t& out) {
  return parse_number(text, out) && out >= 1 && out <= rt::kNumPads;
}

[[nodiscard]] Command parse_mix(const std::vector<std::string_view>& words) {
  Command out = command_of(CommandKind::kMix);
  if (words.size() < 2) {
    return out;
  }
  if (words[1] == "bus" || words[1] == "buses") {
    out.pattern = 1;
    return out;
  }
  return error("mix: " + std::string{words[1]} + " is not a page; try: mix, or mix bus");
}

// `gain 3 -6` moves pad 3's fader; `gain bus a -6` moves bus A's.
//
// The bus form is spelled out rather than overloading the first argument,
// because "a" and "3" would otherwise both be targets and a typo in either one
// would silently address the wrong thing.
[[nodiscard]] Command parse_gain(const std::vector<std::string_view>& words) {
  Command out = command_of(CommandKind::kStripGain);

  std::size_t value_index = 2;
  if (words.size() >= 2 && words[1] == "bus") {
    if (words.size() < 4) {
      return error("gain bus needs a bus and a level, e.g. gain bus a -6");
    }
    std::uint8_t bus = 0;
    if (!parse_bus_letter(words[2], bus)) {
      return error("gain: " + std::string{words[2]} + " is not a bus; try a, b, c or d");
    }
    out.bus = bus;
    out.bus_target = true;
    value_index = 3;
  } else {
    if (words.size() < 3) {
      return error("gain needs a pad and a level, e.g. gain 3 -6, or gain bus a -6");
    }
    std::size_t pad = 0;
    if (!parse_pad_number(words[1], pad)) {
      return error("gain: " + std::string{words[1]} + " is not a pad, 1 to 16");
    }
    out.pad = pad;
  }

  if (!parse_decimal(words[value_index], out.decibels)) {
    return error("gain: " + std::string{words[value_index]} + " is not a level in dB");
  }
  // The fader's range, from rt::kMaxStripGain (+12 dB) down to silence. Named
  // in the message rather than clamped, so a mistyped -60 is a refusal instead
  // of a strip that quietly went quiet.
  if (out.decibels > 12.0F || out.decibels < -60.0F) {
    return error("gain: " + std::string{words[value_index]} + " is outside -60 to +12 dB");
  }
  return out;
}

[[nodiscard]] Command parse_pan(const std::vector<std::string_view>& words) {
  if (words.size() < 3) {
    return error("pan needs a pad and a position, e.g. pan 3 -50 or pan 3 c");
  }
  Command out = command_of(CommandKind::kStripPan);
  std::size_t pad = 0;
  if (!parse_pad_number(words[1], pad)) {
    return error("pan: " + std::string{words[1]} + " is not a pad, 1 to 16");
  }
  out.pad = pad;

  // `c` for centre, because that is what the strip reads back and typing 0 to
  // mean centre is a guess the readout would not confirm.
  if (words[2] == "c" || words[2] == "C") {
    out.pan_percent = 0;
    return out;
  }
  float percent = 0.0F;
  if (!parse_decimal(words[2], percent)) {
    return error("pan: " + std::string{words[2]} + " is not a position; try -100 to 100, or c");
  }
  if (percent < -100.0F || percent > 100.0F) {
    return error("pan: " + std::string{words[2]} + " is outside -100 to 100");
  }
  out.pan_percent = static_cast<int>(percent);
  return out;
}

// `mute 3`, `mute 3 on`, `mute 3 off`. Bare flips it, the same tri-state
// `metro` uses and for the same reason: the parser does not know the current
// setting and must not guess.
[[nodiscard]] Command parse_strip_switch(const std::vector<std::string_view>& words,
                                         CommandKind kind, std::string_view verb) {
  if (words.size() < 2) {
    return error(std::string{verb} + " needs a pad, e.g. " + std::string{verb} + " 3");
  }
  Command out = command_of(kind);
  std::size_t pad = 0;
  if (!parse_pad_number(words[1], pad)) {
    return error(std::string{verb} + ": " + std::string{words[1]} + " is not a pad, 1 to 16");
  }
  out.pad = pad;
  if (words.size() < 3) {
    return out;
  }
  if (words[2] == "on") {
    out.toggle = Switch::kOn;
    return out;
  }
  if (words[2] == "off") {
    out.toggle = Switch::kOff;
    return out;
  }
  return error(std::string{verb} + ": " + std::string{words[2]} + " is not on or off");
}

[[nodiscard]] Command parse_bus(const std::vector<std::string_view>& words) {
  if (words.size() < 3) {
    return error("bus needs a pad and a bus, e.g. bus 3 a");
  }
  Command out = command_of(CommandKind::kStripBus);
  std::size_t pad = 0;
  if (!parse_pad_number(words[1], pad)) {
    return error("bus: " + std::string{words[1]} + " is not a pad, 1 to 16");
  }
  std::uint8_t bus = 0;
  if (!parse_bus_letter(words[2], bus)) {
    return error("bus: " + std::string{words[2]} + " is not a bus; try a, b, c or d");
  }
  out.pad = pad;
  out.bus = bus;
  return out;
}

// `eq 3 2 off` bypasses band 2; `eq 3 2 800 -4 1.2` sets it.
//
// Frequency, gain, shape -- in that order because it is the order the cookbook
// and docs/MIXER.md state them in, and an EQ whose arguments are in a different
// order from its specification is one nobody can check against it.
[[nodiscard]] Command parse_eq(const std::vector<std::string_view>& words) {
  if (words.size() < 3) {
    return error("eq needs a pad and a band, e.g. eq 3 2 800 -4 1.2, or eq 3 2 off");
  }
  Command out = command_of(CommandKind::kStripEq);
  std::size_t pad = 0;
  if (!parse_pad_number(words[1], pad)) {
    return error("eq: " + std::string{words[1]} + " is not a pad, 1 to 16");
  }
  std::size_t band = 0;
  if (!parse_number(words[2], band) || band < 1 || band > rt::kEqBands) {
    return error("eq: " + std::string{words[2]} + " is not a band, 1 to 4");
  }
  out.pad = pad;
  out.band = static_cast<std::uint8_t>(band);

  if (words.size() >= 4 && words[3] == "off") {
    out.toggle = Switch::kOff;
    return out;
  }
  if (words.size() < 6) {
    return error("eq needs frequency, gain and Q, e.g. eq 3 2 800 -4 1.2 (or eq 3 2 off)");
  }
  if (!parse_decimal(words[3], out.frequency) || out.frequency <= 0.0F) {
    return error("eq: " + std::string{words[3]} + " is not a frequency in Hz");
  }
  if (!parse_decimal(words[4], out.decibels)) {
    return error("eq: " + std::string{words[4]} + " is not a gain in dB");
  }
  if (out.decibels < -24.0F || out.decibels > 24.0F) {
    return error("eq: " + std::string{words[4]} + " is outside -24 to +24 dB");
  }
  if (!parse_decimal(words[5], out.shape) || out.shape <= 0.0F) {
    return error("eq: " + std::string{words[5]} + " is not a Q or shelf slope");
  }
  out.toggle = Switch::kOn;
  return out;
}

// `comp 3 off`, or `comp 3 -18 4` with optional knee, makeup, attack, release.
[[nodiscard]] Command parse_comp(const std::vector<std::string_view>& words) {
  if (words.size() < 3) {
    return error("comp needs a pad and a threshold, e.g. comp 3 -18 4, or comp 3 off");
  }
  Command out = command_of(CommandKind::kStripComp);
  std::size_t pad = 0;
  if (!parse_pad_number(words[1], pad)) {
    return error("comp: " + std::string{words[1]} + " is not a pad, 1 to 16");
  }
  out.pad = pad;

  if (words[2] == "off") {
    out.toggle = Switch::kOff;
    return out;
  }
  if (words.size() < 4) {
    return error("comp needs a threshold and a ratio, e.g. comp 3 -18 4");
  }
  if (!parse_decimal(words[2], out.decibels)) {
    return error("comp: " + std::string{words[2]} + " is not a threshold in dB");
  }
  if (out.decibels < -60.0F || out.decibels > 0.0F) {
    return error("comp: " + std::string{words[2]} + " is outside -60 to 0 dB");
  }
  if (!parse_decimal(words[3], out.ratio)) {
    return error("comp: " + std::string{words[3]} + " is not a ratio");
  }
  if (out.ratio < 1.0F || out.ratio > 20.0F) {
    return error("comp: " + std::string{words[3]} + " is outside 1 to 20");
  }

  // Defaults for what was not typed: a soft-ish knee, no makeup, and times that
  // catch a drum transient without pumping. Stated here rather than left to the
  // engine so that `comp 3 -18 4` produces the same compressor every time.
  out.knee_db = 6.0F;
  out.makeup_db = 0.0F;
  out.attack_ms = 5.0F;
  out.release_ms = 120.0F;

  if (words.size() >= 5 && !parse_decimal(words[4], out.knee_db)) {
    return error("comp: " + std::string{words[4]} + " is not a knee in dB");
  }
  if (words.size() >= 6 && !parse_decimal(words[5], out.makeup_db)) {
    return error("comp: " + std::string{words[5]} + " is not a makeup gain in dB");
  }
  if (words.size() >= 7 && !parse_decimal(words[6], out.attack_ms)) {
    return error("comp: " + std::string{words[6]} + " is not an attack in ms");
  }
  if (words.size() >= 8 && !parse_decimal(words[7], out.release_ms)) {
    return error("comp: " + std::string{words[7]} + " is not a release in ms");
  }
  if (out.knee_db < 0.0F || out.knee_db > 24.0F) {
    return error("comp: knee is outside 0 to 24 dB");
  }
  if (out.makeup_db < 0.0F || out.makeup_db > 24.0F) {
    return error("comp: makeup is outside 0 to 24 dB");
  }
  if (out.attack_ms < 0.0F || out.release_ms < 0.0F) {
    return error("comp: times cannot be negative");
  }
  out.toggle = Switch::kOn;
  return out;
}

// `limit`, `limit on`, `limit off`, `limit on -1.0`.
//
// Here rather than left unreachable: T6 built the limiter OFF by default and
// nothing could turn it on, which is a feature that exists only in the tests.
[[nodiscard]] Command parse_limit(const std::vector<std::string_view>& words) {
  Command out = command_of(CommandKind::kLimiter);
  out.decibels = rt::kDefaultCeilingDb;
  if (words.size() < 2) {
    return out;  // bare `limit` flips it
  }
  if (words[1] == "off") {
    out.toggle = Switch::kOff;
    return out;
  }
  if (words[1] != "on") {
    return error("limit: " + std::string{words[1]} + " is not on or off");
  }
  out.toggle = Switch::kOn;
  if (words.size() >= 3) {
    if (!parse_decimal(words[2], out.decibels)) {
      return error("limit: " + std::string{words[2]} + " is not a ceiling in dB");
    }
    if (out.decibels > 0.0F || out.decibels < -24.0F) {
      return error("limit: " + std::string{words[2]} + " is outside -24 to 0 dB");
    }
  }
  return out;
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
  if (verb == "stop") {
    return parse_stop(words);
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
  if (verb == "mix") {
    return parse_mix(words);
  }
  if (verb == "gain") {
    return parse_gain(words);
  }
  if (verb == "pan") {
    return parse_pan(words);
  }
  if (verb == "mute") {
    return parse_strip_switch(words, CommandKind::kStripMute, "mute");
  }
  if (verb == "solo") {
    return parse_strip_switch(words, CommandKind::kStripSolo, "solo");
  }
  if (verb == "bus") {
    return parse_bus(words);
  }
  if (verb == "eq") {
    return parse_eq(words);
  }
  if (verb == "comp") {
    return parse_comp(words);
  }
  if (verb == "limit") {
    return parse_limit(words);
  }
  if (verb == "q" || verb == "quit") {
    return command_of(CommandKind::kQuit);
  }

  return error("unknown command: " + std::string{verb});
}

}  // namespace tui
