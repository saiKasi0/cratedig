#include "tui/command.hpp"

#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

// The command line, parsed.
//
// parse_command() is a pure function, which is the whole reason these tests need
// no terminal, no engine and no file -- the same argument as render(). What the
// commands then DO is exercised by the PTY session and the e2e chop test; this
// file is about what the machine understood.

namespace {

using tui::Command;
using tui::CommandKind;
using tui::parse_command;

}  // namespace

TEST_CASE("a blank line is a cancel, not a mistake", "[command]") {
  // Pressing `:` and changing your mind is the commonest thing that happens at a
  // prompt. It has to be silent -- an error message for it would be nagging.
  for (const std::string_view line : {"", " ", "   \t "}) {
    const Command command = parse_command(line);
    CHECK(command.kind == CommandKind::kNone);
    CHECK(command.message.empty());
  }
}

TEST_CASE("chop transient", "[command]") {
  CHECK(parse_command("chop transient").kind == CommandKind::kChopTransient);

  // Leading and trailing space is what happens when you type fast.
  CHECK(parse_command("  chop   transient  ").kind == CommandKind::kChopTransient);

  // `slice` is accepted wherever `chop` is: the ROADMAP acceptance criterion
  // says `:chop transient` and the mockups' palette says `slice transient`.
  // Both work rather than one of the two documents being made wrong.
  CHECK(parse_command("slice transient").kind == CommandKind::kChopTransient);
}

TEST_CASE("chop grid carries its count", "[command]") {
  const Command command = parse_command("chop grid 16");
  REQUIRE(command.kind == CommandKind::kChopGrid);
  CHECK(command.count == 16);

  CHECK(parse_command("slice grid 8").count == 8);
  CHECK(parse_command("chop grid 1").count == 1);
  CHECK(parse_command("chop grid 512").count == 512);
}

TEST_CASE("chop reset", "[command]") {
  CHECK(parse_command("chop reset").kind == CommandKind::kChopReset);
  CHECK(parse_command("slice reset").kind == CommandKind::kChopReset);
}

TEST_CASE("slot assign carries both numbers, as typed", "[command]") {
  const Command command = parse_command("slot assign 5 3");
  REQUIRE(command.kind == CommandKind::kSlotAssign);

  // ONE-BASED, exactly as the player typed them. Converting to zero-based here
  // would mean the error messages have to convert back to say which number was
  // rejected, and one of the two conversions would eventually be forgotten.
  CHECK(command.slice == 5);
  CHECK(command.pad == 3);
}

TEST_CASE("pad gate and pad oneshot, with the number optional", "[command]") {
  // The number is optional and absent means ALL, because gate is a way of
  // playing rather than a property of one chop: someone who wants held pads
  // wants them on the bank.
  const Command all = parse_command("pad gate");
  REQUIRE(all.kind == CommandKind::kPadGate);
  CHECK(all.pad == 0);

  const Command one = parse_command("pad gate 3");
  REQUIRE(one.kind == CommandKind::kPadGate);
  CHECK(one.pad == 3);

  CHECK(parse_command("pad oneshot").kind == CommandKind::kPadOneShot);
  CHECK(parse_command("pad oneshot 16").pad == 16);
}

TEST_CASE("pad refuses what it cannot mean", "[command]") {
  // 0 is a mistake even though "no number" means all sixteen: the two have to
  // stay distinguishable, or a typed `pad gate 0` silently gates the bank.
  CHECK(parse_command("pad gate 0").kind == CommandKind::kError);
  CHECK(parse_command("pad gate x").kind == CommandKind::kError);
  CHECK(parse_command("pad").message.find("gate") != std::string::npos);

  const Command wrong = parse_command("pad loud");
  REQUIRE(wrong.kind == CommandKind::kError);
  CHECK(wrong.message.find("loud") != std::string::npos);
  CHECK(wrong.message.find("oneshot") != std::string::npos);
}

TEST_CASE("quit", "[command]") {
  CHECK(parse_command("q").kind == CommandKind::kQuit);
  CHECK(parse_command("quit").kind == CommandKind::kQuit);

  // Not a prefix match: `qu` is a typo, and quitting on a typo loses work.
  CHECK(parse_command("qu").kind == CommandKind::kError);
  CHECK(parse_command("quits").kind == CommandKind::kError);
}

TEST_CASE("an unknown verb names itself", "[command]") {
  const Command command = parse_command("frobnicate the widget");
  REQUIRE(command.kind == CommandKind::kError);

  // The message has to contain what was typed. "unknown command" on its own
  // does not tell a player whether the machine misread them or they misread the
  // manual.
  CHECK(command.message.find("frobnicate") != std::string::npos);
}

TEST_CASE("an incomplete command says what is missing", "[command]") {
  struct Case {
    std::string_view line;
    std::string_view expect;
  };

  // Each message names the correct spelling rather than only refusing, because
  // the mode line is the only documentation visible at the moment of the
  // mistake.
  const Case cases[] = {
      {"chop", "transient"},     {"chop grid", "grid"},       {"slot", "assign"},
      {"slot assign", "assign"}, {"slot assign 5", "assign"},
  };

  for (const Case& item : cases) {
    const Command command = parse_command(item.line);
    INFO("line: " << item.line);
    REQUIRE(command.kind == CommandKind::kError);
    CHECK(command.message.find(item.expect) != std::string::npos);
  }
}

TEST_CASE("numbers that are not numbers are refused", "[command]") {
  for (const std::string_view line :
       {"chop grid x", "chop grid 16x", "chop grid -4", "chop grid 1.5", "chop grid +4"}) {
    INFO("line: " << line);
    CHECK(parse_command(line).kind == CommandKind::kError);
  }

  // "16x" specifically: strtol-style parsing reads 16 and stops, which would
  // silently do something the player did not ask for. from_chars is required to
  // consume the WHOLE word, and this is the test that says so.
  const Command command = parse_command("chop grid 16x");
  REQUIRE(command.kind == CommandKind::kError);
  CHECK(command.message.find("16x") != std::string::npos);
}

TEST_CASE("zero is not a count, a slice or a pad", "[command]") {
  // Slices and pads are 1-based on screen, so `0` is always a mistake -- and
  // accepting it would underflow the conversion to an index.
  CHECK(parse_command("chop grid 0").kind == CommandKind::kError);
  CHECK(parse_command("slot assign 0 3").kind == CommandKind::kError);
  CHECK(parse_command("slot assign 5 0").kind == CommandKind::kError);
}

TEST_CASE("an absurd grid count is refused with the limit named", "[command]") {
  const Command command = parse_command("chop grid 5000");
  REQUIRE(command.kind == CommandKind::kError);

  // Refused rather than clamped: a typed extra zero should be visible, not
  // quietly turned into a different chop. The message says what the limit is so
  // the retry is one edit away.
  CHECK(command.message.find("512") != std::string::npos);

  // And an overflowing number is a refusal too, not a wrap.
  CHECK(parse_command("chop grid 99999999999999999999").kind == CommandKind::kError);
}

TEST_CASE("a huge slot number parses and is left to the caller", "[command]") {
  // The parser does NOT know how many slices exist or how many pads there are.
  // Range-checking here would mean parsing depended on program state, which is
  // exactly what keeps this function testable without one. `slot assign 900 5`
  // is well-formed; app.cpp is what refuses it, and says how many slices there
  // actually are when it does.
  const Command command = parse_command("slot assign 900 5");
  REQUIRE(command.kind == CommandKind::kSlotAssign);
  CHECK(command.slice == 900);
}

TEST_CASE("extra words are ignored rather than fatal", "[command]") {
  // Trailing junk after a complete command is accepted. The alternative --
  // refusing -- means a stray keystroke at the end of a line throws away a
  // command that was otherwise perfectly clear.
  CHECK(parse_command("chop transient now").kind == CommandKind::kChopTransient);
  CHECK(parse_command("slot assign 5 3 please").kind == CommandKind::kSlotAssign);
}

TEST_CASE("case matters", "[command]") {
  // Deliberate, and worth writing down: the pad map is lower-case and so is
  // every command. Accepting `CHOP` would invite `Chop`, and then the question
  // of whether `Slot Assign` works becomes a real one to answer.
  CHECK(parse_command("CHOP transient").kind == CommandKind::kError);
  CHECK(parse_command("chop TRANSIENT").kind == CommandKind::kError);
}

// -- the sequencer verbs, M4 ---------------------------------------------------

TEST_CASE("bpm is fixed point, so what was typed is what is stored", "[command]") {
  // The whole reason tempo is carried as hundredths rather than a float: 92.5
  // has to come back out as 92.5 and not 92.49999. Asserted on the integer,
  // which is the only place that can be exact.
  CHECK(tui::parse_command("bpm 120").bpm_x100 == 12'000);
  CHECK(tui::parse_command("bpm 92.5").bpm_x100 == 9'250);
  CHECK(tui::parse_command("bpm 89.53").bpm_x100 == 8'953);
  CHECK(tui::parse_command("bpm 120.0").bpm_x100 == 12'000);

  // `.5` is five TENTHS. Reading the digits as written would make 92.5 mean
  // 92.05 -- a different tempo, arrived at silently.
  CHECK(tui::parse_command("bpm 92.5").bpm_x100 != tui::parse_command("bpm 92.05").bpm_x100);
  CHECK(tui::parse_command("bpm 92.05").bpm_x100 == 9'205);

  CHECK(tui::parse_command("bpm 120").kind == CommandKind::kBpm);
}

TEST_CASE("bpm refuses what it cannot store exactly", "[command]") {
  // Refused rather than rounded. A tempo quietly changed to a neighbouring one
  // shows up as drift against another machine, minutes later, with nothing on
  // screen to connect it to.
  const Command precise = parse_command("bpm 92.505");
  REQUIRE(precise.kind == CommandKind::kError);
  CHECK(precise.message.find("92.505") != std::string::npos);

  for (const std::string_view line :
       {"bpm", "bpm x", "bpm 92.", "bpm .5", "bpm 92.5.5", "bpm -4"}) {
    INFO("line: " << line);
    CHECK(parse_command(line).kind == CommandKind::kError);
  }
}

TEST_CASE("bpm names its bounds when it refuses one", "[command]") {
  // The bounds are rt::kMinBpmX100 / kMaxBpmX100 and the message is built from
  // them, so it cannot outlive them. 20 and 300 are what those are today.
  const Command slow = parse_command("bpm 19.99");
  REQUIRE(slow.kind == CommandKind::kError);
  CHECK(slow.message.find("20.00") != std::string::npos);
  CHECK(slow.message.find("300.00") != std::string::npos);

  CHECK(parse_command("bpm 300.01").kind == CommandKind::kError);
  CHECK(parse_command("bpm 99999999999999999999").kind == CommandKind::kError);

  // And the edges themselves are accepted, not off by one.
  CHECK(parse_command("bpm 20").kind == CommandKind::kBpm);
  CHECK(parse_command("bpm 300").kind == CommandKind::kBpm);
}

TEST_CASE("a tempo formats back the way it was typed", "[command]") {
  // The round trip, which is the reason format_bpm lives beside the parser.
  CHECK(tui::format_bpm(9'250) == "92.50");
  CHECK(tui::format_bpm(9'205) == "92.05");
  CHECK(tui::format_bpm(12'000) == "120.00");
  CHECK(tui::format_bpm(tui::parse_command("bpm 89.53").bpm_x100) == "89.53");
}

TEST_CASE("swing takes a percentage, and zero is one of them", "[command]") {
  const Command command = parse_command("swing 58");
  REQUIRE(command.kind == CommandKind::kSwing);
  CHECK(command.swing == 58);

  // ZERO IS VALID, unlike every count and index in this file. Straight is a
  // setting rather than an absent one, and `swing 0` is how you take it off.
  const Command straight = parse_command("swing 0");
  REQUIRE(straight.kind == CommandKind::kSwing);
  CHECK(straight.swing == 0);

  CHECK(parse_command("swing 75").kind == CommandKind::kSwing);  // rt::kMaxSwingPercent
}

TEST_CASE("swing refuses a shift that would reorder the steps", "[command]") {
  // Past 100% an odd step lands on top of its neighbour and step positions stop
  // being monotonic, which the audio thread's block scan relies on. The cap is
  // 75 and the message says so rather than clamping in silence.
  const Command command = parse_command("swing 90");
  REQUIRE(command.kind == CommandKind::kError);
  CHECK(command.message.find("75") != std::string::npos);

  CHECK(parse_command("swing").kind == CommandKind::kError);
  CHECK(parse_command("swing x").kind == CommandKind::kError);
  CHECK(parse_command("swing 58%").kind == CommandKind::kError);
}

TEST_CASE("pattern selects, resizes, or clears", "[command]") {
  const Command select = parse_command("pattern 3");
  REQUIRE(select.kind == CommandKind::kPatternSelect);
  CHECK(select.pattern == 3);  // 1-based, exactly as typed

  const Command length = parse_command("pattern length 32");
  REQUIRE(length.kind == CommandKind::kPatternLength);
  CHECK(length.count == 32);

  CHECK(parse_command("pattern clear").kind == CommandKind::kPatternClear);
  CHECK(parse_command("pattern 1").kind == CommandKind::kPatternSelect);
  CHECK(parse_command("pattern 16").kind == CommandKind::kPatternSelect);
}

TEST_CASE("pattern is range-checked where slot assign is not", "[command]") {
  // The difference is what the limit depends on. How many slices exist is
  // program state the parser must not need; how many patterns there are is a
  // compile-time constant, so checking it here keeps the function pure AND
  // lets the message name the number.
  const Command high = parse_command("pattern 17");
  REQUIRE(high.kind == CommandKind::kError);
  CHECK(high.message.find("16") != std::string::npos);

  CHECK(parse_command("pattern 0").kind == CommandKind::kError);
  CHECK(parse_command("pattern x").kind == CommandKind::kError);
  CHECK(parse_command("pattern").message.find("clear") != std::string::npos);

  const Command over = parse_command("pattern length 33");
  REQUIRE(over.kind == CommandKind::kError);
  CHECK(over.message.find("32") != std::string::npos);
  CHECK(parse_command("pattern length 0").kind == CommandKind::kError);
  CHECK(parse_command("pattern length").kind == CommandKind::kError);
}

TEST_CASE("song is a list of patterns, in play order", "[command]") {
  const Command command = parse_command("song 1 2 3 1");
  REQUIRE(command.kind == CommandKind::kSong);
  REQUIRE(command.song.size() == 4);
  CHECK(command.song[0] == 1);
  CHECK(command.song[3] == 1);  // repeats are the point of a chain

  const Command one = parse_command("song 2");
  REQUIRE(one.kind == CommandKind::kSong);
  CHECK(one.song.size() == 1);

  // `song clear` is its own kind rather than an empty list, so "no song" cannot
  // be confused with "a song nobody filled in".
  const Command cleared = parse_command("song clear");
  CHECK(cleared.kind == CommandKind::kSongClear);
  CHECK(cleared.song.empty());
}

TEST_CASE("song is the one verb where a trailing word is fatal", "[command]") {
  // Everywhere else an extra word is ignored, because the command was already
  // complete. Here every word IS an argument, so `song 1 2 x` would silently
  // become a two-slot song rather than the longer one being typed.
  const Command command = parse_command("song 1 2 x");
  REQUIRE(command.kind == CommandKind::kError);
  CHECK(command.message.find('x') != std::string::npos);

  CHECK(parse_command("song 1 0 2").kind == CommandKind::kError);
  CHECK(parse_command("song 1 17").kind == CommandKind::kError);
  CHECK(parse_command("song").message.find("clear") != std::string::npos);
}

TEST_CASE("a song longer than the song can hold is refused", "[command]") {
  std::string line = "song";
  for (int slot = 0; slot < 65; ++slot) {  // rt::kMaxSongSlots is 64
    line += " 1";
  }
  const Command command = parse_command(line);
  REQUIRE(command.kind == CommandKind::kError);
  CHECK(command.message.find("64") != std::string::npos);

  std::string full = "song";
  for (int slot = 0; slot < 64; ++slot) {
    full += " 1";
  }
  const Command exact = parse_command(full);
  REQUIRE(exact.kind == CommandKind::kSong);
  CHECK(exact.song.size() == 64);
}

TEST_CASE("metro flips, or is told which way", "[command]") {
  // Bare `metro` cannot know what it is now, so it carries kToggle and app.cpp
  // resolves it against the state the parser deliberately does not have.
  const Command bare = parse_command("metro");
  REQUIRE(bare.kind == CommandKind::kMetronome);
  CHECK(bare.toggle == tui::Switch::kToggle);

  CHECK(parse_command("metro on").toggle == tui::Switch::kOn);
  CHECK(parse_command("metro off").toggle == tui::Switch::kOff);

  const Command wrong = parse_command("metro loud");
  REQUIRE(wrong.kind == CommandKind::kError);
  CHECK(wrong.message.find("loud") != std::string::npos);
}

TEST_CASE("stop takes a pad, or everything", "[command]") {
  // Bare `stop` is the panic: everything, and the transport with it. A pad
  // number is the surgical version and leaves the transport alone -- the
  // asymmetry is deliberate and app.cpp is where it happens, so all the parser
  // carries is which of the two was asked for.
  const Command all = parse_command("stop");
  REQUIRE(all.kind == CommandKind::kStop);
  CHECK(all.pad == 0);

  const Command one = parse_command("stop 3");
  REQUIRE(one.kind == CommandKind::kStop);
  CHECK(one.pad == 3);  // 1-based, as typed

  CHECK(parse_command("stop 16").pad == 16);
}

TEST_CASE("stop refuses a pad it cannot mean", "[command]") {
  // 0 is a mistake even though "no number" means all of them -- the same rule
  // `pad gate 0` follows, and for the same reason: the two must stay
  // distinguishable or a typed `stop 0` silences the room.
  CHECK(parse_command("stop 0").kind == CommandKind::kError);
  CHECK(parse_command("stop x").kind == CommandKind::kError);

  const Command high = parse_command("stop 17");
  REQUIRE(high.kind == CommandKind::kError);
  CHECK(high.message.find("16") != std::string::npos);
}

TEST_CASE("edit, with the slice number optional", "[command]") {
  // No number means "the slice already selected", which is what you want after
  // stepping to it; with one, it jumps.
  const Command current = parse_command("edit");
  REQUIRE(current.kind == CommandKind::kEdit);
  CHECK(current.slice == 0);

  const Command jump = parse_command("edit 7");
  REQUIRE(jump.kind == CommandKind::kEdit);
  CHECK(jump.slice == 7);

  CHECK(parse_command("edit 0").kind == CommandKind::kError);
  CHECK(parse_command("edit x").kind == CommandKind::kError);
  CHECK(parse_command("perform").kind == CommandKind::kPerform);
}
