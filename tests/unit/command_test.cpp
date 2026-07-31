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
