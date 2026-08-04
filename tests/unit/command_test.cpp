#include "tui/command.hpp"

#include "rt/limiter.hpp"

#include <string>
#include <string_view>

#include <catch2/catch_approx.hpp>
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

TEST_CASE("slot assign takes ranges on either side", "[command]") {
  // `1-8 1` is the form that makes a chop land on a bank in one line. The pad is
  // a STARTING pad, so the range on the right is implied by the one on the left.
  const Command up = parse_command("slot assign 1-8 1");
  REQUIRE(up.kind == CommandKind::kSlotAssign);
  CHECK(up.slice == 1);
  CHECK(up.slice_last == 8);
  CHECK(up.pad == 1);

  CHECK(parse_command("slot assign 1-8 9").pad == 9);

  // Both sides stated explicitly means the same thing.
  const Command both = parse_command("slot assign 1-8 1-8");
  REQUIRE(both.kind == CommandKind::kSlotAssign);
  CHECK(both.slice == 1);
  CHECK(both.slice_last == 8);
  CHECK(both.pad == 1);

  // A single assignment is the ONE-ELEMENT RANGE, not a separate shape -- which
  // is what stops app.cpp needing a second code path that could disagree.
  const Command one = parse_command("slot assign 5 3");
  REQUIRE(one.kind == CommandKind::kSlotAssign);
  CHECK(one.slice == 5);
  CHECK(one.slice_last == 5);
  CHECK(one.pad == 3);
}

TEST_CASE("slot assign refuses a range it cannot carry out", "[command]") {
  // A length mismatch names BOTH counts. Truncating silently would leave the
  // rest of the pads holding whatever they held, which reads as the command
  // half-working rather than as it being wrong.
  const Command mismatch = parse_command("slot assign 1-8 1-4");
  REQUIRE(mismatch.kind == CommandKind::kError);
  CHECK(mismatch.message.find('8') != std::string::npos);
  CHECK(mismatch.message.find('4') != std::string::npos);

  // Reversed is refused rather than normalised: `8-1` is far more likely to be a
  // typo than a request to assign backwards, and quietly reversing it would put
  // slice 8 on pad 1 while the line says the opposite.
  CHECK(parse_command("slot assign 8-1 1").kind == CommandKind::kError);

  for (const std::string_view line :
       {"slot assign 0-8 1", "slot assign 1-0 1", "slot assign 1- 1", "slot assign -8 1",
        "slot assign 1-x 1", "slot assign 1-8 0", "slot assign 1-8 2-x"}) {
    INFO("line: " << line);
    CHECK(parse_command(line).kind == CommandKind::kError);
  }
}

TEST_CASE("a slot range is left to the caller to bound", "[command]") {
  // Same split `slot assign 900 5` already documents: how many slices exist is
  // program state the parser must not need, so this is well-formed and app.cpp
  // is what refuses it -- naming how many there actually are when it does.
  const Command command = parse_command("slot assign 1-99 1");
  REQUIRE(command.kind == CommandKind::kSlotAssign);
  CHECK(command.slice_last == 99);
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
  CHECK(parse_command("chop reset now").kind == CommandKind::kChopReset);
  CHECK(parse_command("slot assign 5 3 please").kind == CommandKind::kSlotAssign);

  // AFTER the arguments, not instead of one. `chop transient now` USED to be
  // this case and is not any more: the word after `transient` is the density,
  // so `now` is a bad argument rather than trailing junk -- the same way
  // `chop grid blah` has always been a bad count rather than an ignorable word.
  // Junk after the density is still junk.
  CHECK(parse_command("chop transient now").kind == CommandKind::kError);
  CHECK(parse_command("chop transient beat please").kind == CommandKind::kChopTransient);
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

// -- the mixer ---------------------------------------------------------------

TEST_CASE("mix opens the screen, and names a page", "[command]") {
  const Command open = parse_command("mix");
  REQUIRE(open.kind == CommandKind::kMix);
  CHECK(open.pattern == 0);

  const Command buses = parse_command("mix bus");
  REQUIRE(buses.kind == CommandKind::kMix);
  CHECK(buses.pattern == 1);
  CHECK(parse_command("mix buses").pattern == 1);

  CHECK(parse_command("mix sideways").kind == CommandKind::kError);
}

TEST_CASE("gain takes a pad or a bus", "[command]") {
  const Command pad = parse_command("gain 3 -6");
  REQUIRE(pad.kind == CommandKind::kStripGain);
  CHECK(pad.pad == 3);
  CHECK_FALSE(pad.bus_target);
  CHECK(pad.decibels == -6.0F);

  // Fractions survive: a mixer that could only move in whole decibels would be
  // a mixer nobody could balance two similar sounds with.
  CHECK(parse_command("gain 3 -6.5").decibels == -6.5F);
  CHECK(parse_command("gain 3 +2.25").decibels == 2.25F);
  CHECK(parse_command("gain 3 0").decibels == 0.0F);

  const Command bus = parse_command("gain bus c -3");
  REQUIRE(bus.kind == CommandKind::kStripGain);
  CHECK(bus.bus_target);
  CHECK(bus.bus == 2);
  CHECK(bus.decibels == -3.0F);
  CHECK(parse_command("gain bus A 0").bus == 0);

  // The bus form is SPELLED OUT rather than overloading the first argument. If
  // `gain a -3` were legal, a typo in a pad number would silently address a bus.
  CHECK(parse_command("gain a -3").kind == CommandKind::kError);

  // Out of range is refused with the range named, not clamped: a mistyped -600
  // that quietly became -60 would look like the fader had a mind of its own.
  const Command loud = parse_command("gain 3 20");
  REQUIRE(loud.kind == CommandKind::kError);
  CHECK(loud.message.find("+12") != std::string::npos);
  CHECK(parse_command("gain 3 -600").kind == CommandKind::kError);
  CHECK(parse_command("gain 0 -6").kind == CommandKind::kError);
  CHECK(parse_command("gain 17 -6").kind == CommandKind::kError);
  CHECK(parse_command("gain 3 loud").kind == CommandKind::kError);
  CHECK(parse_command("gain 3").kind == CommandKind::kError);
  CHECK(parse_command("gain bus e 0").kind == CommandKind::kError);
}

TEST_CASE("pan takes a percentage, or c for centre", "[command]") {
  const Command left = parse_command("pan 4 -50");
  REQUIRE(left.kind == CommandKind::kStripPan);
  CHECK(left.pad == 4);
  CHECK(left.pan_percent == -50);

  CHECK(parse_command("pan 4 100").pan_percent == 100);

  // `c` rather than 0, because "centre" is what the strip reads back and typing
  // 0 to mean it is a guess the readout would not confirm.
  const Command centre = parse_command("pan 4 c");
  REQUIRE(centre.kind == CommandKind::kStripPan);
  CHECK(centre.pan_percent == 0);

  CHECK(parse_command("pan 4 -101").kind == CommandKind::kError);
  CHECK(parse_command("pan 4 hard").kind == CommandKind::kError);
  CHECK(parse_command("pan 4").kind == CommandKind::kError);
}

TEST_CASE("mute and solo flip by default and can be stated", "[command]") {
  // Bare flips it -- the same tri-state `metro` uses, and for the same reason:
  // the parser does not know the current setting and must not guess.
  const Command flip = parse_command("mute 5");
  REQUIRE(flip.kind == CommandKind::kStripMute);
  CHECK(flip.pad == 5);
  CHECK(flip.toggle == tui::Switch::kToggle);

  CHECK(parse_command("mute 5 on").toggle == tui::Switch::kOn);
  CHECK(parse_command("mute 5 off").toggle == tui::Switch::kOff);

  const Command solo = parse_command("solo 12 on");
  REQUIRE(solo.kind == CommandKind::kStripSolo);
  CHECK(solo.pad == 12);
  CHECK(solo.toggle == tui::Switch::kOn);

  CHECK(parse_command("mute").kind == CommandKind::kError);
  CHECK(parse_command("mute 0").kind == CommandKind::kError);
  CHECK(parse_command("solo 5 maybe").kind == CommandKind::kError);
}

TEST_CASE("bus routes a pad", "[command]") {
  const Command route = parse_command("bus 7 d");
  REQUIRE(route.kind == CommandKind::kStripBus);
  CHECK(route.pad == 7);
  CHECK(route.bus == 3);

  CHECK(parse_command("bus 7 B").bus == 1);
  CHECK(parse_command("bus 7 e").kind == CommandKind::kError);
  CHECK(parse_command("bus 7").kind == CommandKind::kError);
  CHECK(parse_command("bus 0 a").kind == CommandKind::kError);
}

TEST_CASE("eq sets a band or switches it off", "[command]") {
  const Command band = parse_command("eq 3 2 800 -4 1.2");
  REQUIRE(band.kind == CommandKind::kStripEq);
  CHECK(band.pad == 3);
  CHECK(band.band == 2);
  CHECK(band.frequency == 800.0F);
  CHECK(band.decibels == -4.0F);
  CHECK(band.shape == 1.2F);
  CHECK(band.toggle == tui::Switch::kOn);

  const Command off = parse_command("eq 3 2 off");
  REQUIRE(off.kind == CommandKind::kStripEq);
  CHECK(off.band == 2);
  CHECK(off.toggle == tui::Switch::kOff);

  // The band range is the fixed four of docs/MIXER.md, not an arbitrary count.
  CHECK(parse_command("eq 3 0 800 -4 1.2").kind == CommandKind::kError);
  CHECK(parse_command("eq 3 5 800 -4 1.2").kind == CommandKind::kError);

  // Gain is bounded by rt::kMaxEqGainDb, and the message says so.
  const Command loud = parse_command("eq 3 2 800 40 1.2");
  REQUIRE(loud.kind == CommandKind::kError);
  CHECK(loud.message.find("24") != std::string::npos);

  CHECK(parse_command("eq 3 2 800 -4").kind == CommandKind::kError);
  CHECK(parse_command("eq 3 2 0 -4 1.2").kind == CommandKind::kError);
  CHECK(parse_command("eq 3 2 800 -4 0").kind == CommandKind::kError);
}

TEST_CASE("comp takes a threshold and ratio, with defaults for the rest", "[command]") {
  const Command basic = parse_command("comp 3 -18 4");
  REQUIRE(basic.kind == CommandKind::kStripComp);
  CHECK(basic.pad == 3);
  CHECK(basic.decibels == -18.0F);
  CHECK(basic.ratio == 4.0F);
  CHECK(basic.toggle == tui::Switch::kOn);

  // Defaults are STATED by the parser rather than left to the engine, so that
  // `comp 3 -18 4` builds the same compressor every time it is typed.
  CHECK(basic.knee_db == 6.0F);
  CHECK(basic.makeup_db == 0.0F);
  CHECK(basic.attack_ms == 5.0F);
  CHECK(basic.release_ms == 120.0F);

  const Command full = parse_command("comp 3 -18 4 3 6 1 200");
  CHECK(full.knee_db == 3.0F);
  CHECK(full.makeup_db == 6.0F);
  CHECK(full.attack_ms == 1.0F);
  CHECK(full.release_ms == 200.0F);

  CHECK(parse_command("comp 3 off").toggle == tui::Switch::kOff);

  // The ranges are MIXER.md's, and refused rather than clamped.
  CHECK(parse_command("comp 3 -18 30").kind == CommandKind::kError);
  CHECK(parse_command("comp 3 -18 0.5").kind == CommandKind::kError);
  CHECK(parse_command("comp 3 5 4").kind == CommandKind::kError);
  CHECK(parse_command("comp 3 -70 4").kind == CommandKind::kError);
  CHECK(parse_command("comp 3 -18").kind == CommandKind::kError);
}

TEST_CASE("limit reaches the master limiter", "[command]") {
  // Without this verb the limiter T6 built is unreachable -- off by default and
  // with nothing able to turn it on, which is a feature that exists only in the
  // tests.
  const Command flip = parse_command("limit");
  REQUIRE(flip.kind == CommandKind::kLimiter);
  CHECK(flip.toggle == tui::Switch::kToggle);
  CHECK(flip.decibels == rt::kDefaultCeilingDb);

  CHECK(parse_command("limit off").toggle == tui::Switch::kOff);

  const Command on = parse_command("limit on -1.5");
  REQUIRE(on.kind == CommandKind::kLimiter);
  CHECK(on.toggle == tui::Switch::kOn);
  CHECK(on.decibels == -1.5F);

  // A ceiling above 0 dBFS is not a ceiling.
  CHECK(parse_command("limit on 3").kind == CommandKind::kError);
  CHECK(parse_command("limit maybe").kind == CommandKind::kError);
}

TEST_CASE("the mixer verbs do not collide with what was already there", "[command]") {
  // `bus` and `mix` are new words, but `stop`, `pad` and `slot` were not -- and
  // a parser that started matching a prefix would break them silently.
  CHECK(parse_command("stop").kind == CommandKind::kStop);
  CHECK(parse_command("pad gate 3").kind == CommandKind::kPadGate);
  CHECK(parse_command("metro on").kind == CommandKind::kMetronome);
  CHECK(parse_command("perform").kind == CommandKind::kPerform);

  // And an unknown verb is still refused rather than falling into a mixer verb.
  CHECK(parse_command("mixer").kind == CommandKind::kError);
  CHECK(parse_command("gains 3 -6").kind == CommandKind::kError);
}

// -- the crate ---------------------------------------------------------------

TEST_CASE("load takes the rest of the line, spaces and all", "[command]") {
  const Command simple = parse_command("load /crate/amen.wav");
  REQUIRE(simple.kind == CommandKind::kLoadFile);
  CHECK(simple.text == "/crate/amen.wav");

  // A PATH IS NOT A WORD. `~/Music/my breaks/loop 3.wav` is perfectly ordinary,
  // and a parser that split it on spaces would turn one argument into four and a
  // useful error into a baffling one.
  const Command spaced = parse_command("load ~/Music/my breaks/loop 3.wav");
  REQUIRE(spaced.kind == CommandKind::kLoadFile);
  CHECK(spaced.text == "~/Music/my breaks/loop 3.wav");

  // Trailing whitespace is trimmed -- typing a space before Enter is not a
  // request for a different file.
  CHECK(parse_command("load  /crate/a.wav   ").text == "/crate/a.wav");

  // Nothing else is interpreted. The parser has no filesystem, so expanding `~`
  // or a glob here would be guessing on behalf of a caller that can look.
  CHECK(parse_command("load ~/a.wav").text == "~/a.wav");
  CHECK(parse_command("load *.wav").text == "*.wav");

  CHECK(parse_command("load").kind == CommandKind::kError);
  CHECK(parse_command("load   ").kind == CommandKind::kError);
}

TEST_CASE("the crate can be listed, switched and unloaded", "[command]") {
  CHECK(parse_command("files").kind == CommandKind::kListFiles);
  CHECK(parse_command("pool").kind == CommandKind::kListFiles);

  const Command pick = parse_command("file 2");
  REQUIRE(pick.kind == CommandKind::kSelectFile);
  CHECK(pick.file == 2);

  CHECK(parse_command("file").kind == CommandKind::kError);
  CHECK(parse_command("file 0").kind == CommandKind::kError);
  CHECK(parse_command("file x").kind == CommandKind::kError);

  // Bare `unload` drops whichever file is showing, which is what you want after
  // looking at the wrong one. Zero is the sentinel for that and cannot be typed.
  const Command showing = parse_command("unload");
  REQUIRE(showing.kind == CommandKind::kUnloadFile);
  CHECK(showing.file == 0);

  CHECK(parse_command("unload 3").file == 3);
  CHECK(parse_command("unload 0").kind == CommandKind::kError);
  CHECK(parse_command("unload x").kind == CommandKind::kError);
}

TEST_CASE("the crate verbs do not collide with what was already there", "[command]") {
  // `file` is a new word next to `files`, and a parser that matched a prefix
  // would break one of them silently.
  CHECK(parse_command("files").kind == CommandKind::kListFiles);
  CHECK(parse_command("file 1").kind == CommandKind::kSelectFile);
  CHECK(parse_command("fil 1").kind == CommandKind::kError);

  // And the verbs M5 added still parse.
  CHECK(parse_command("mix").kind == CommandKind::kMix);
  CHECK(parse_command("limit off").kind == CommandKind::kLimiter);
  CHECK(parse_command("chop transient").kind == CommandKind::kChopTransient);
}

TEST_CASE("env sets one segment of a pad's envelope", "[command]") {
  // The engine has honoured PadConfig::env since M3 and nothing could set it:
  // EDIT drew four segments and every one was the default. This is the verb that
  // was missing.
  const Command attack = parse_command("env 3 a 12");
  REQUIRE(attack.kind == CommandKind::kPadEnvelope);
  CHECK(attack.pad == 3);
  CHECK(attack.text == "a");
  CHECK(attack.attack_ms == 12.0F);

  CHECK(parse_command("env 3 d 80").text == "d");
  CHECK(parse_command("env 3 r 250").attack_ms == 250.0F);

  // ONE SEGMENT AT A TIME rather than four numbers in a row: `env 3 r 120` is
  // what a person means, and a positional form makes the common case a lookup of
  // which slot is which.
  CHECK(parse_command("env 3 12 80 -6 250").kind == CommandKind::kError);

  // Sustain is DECIBELS, because that is what EDIT shows. A verb taking a linear
  // 0..1 would set a number the screen then reported differently.
  const Command sustain = parse_command("env 3 s -6");
  REQUIRE(sustain.kind == CommandKind::kPadEnvelope);
  CHECK(sustain.text == "s");
  CHECK(sustain.decibels == -6.0F);
  CHECK(parse_command("env 3 s 3").kind == CommandKind::kError);  // above 0 dBFS
  CHECK(parse_command("env 3 s -90").kind == CommandKind::kError);

  CHECK(parse_command("env 3 x 5").kind == CommandKind::kError);
  CHECK(parse_command("env 0 a 5").kind == CommandKind::kError);
  CHECK(parse_command("env 3 a").kind == CommandKind::kError);
  CHECK(parse_command("env 3 a -5").kind == CommandKind::kError);
  CHECK(parse_command("env 3 a 90000").kind == CommandKind::kError);
}

TEST_CASE("chop transient takes a density", "[command]") {
  // `:chop transient` alone is unchanged -- the finest cut, which is what the
  // verb has always done and what every committed chop was measured against.
  const tui::Command bare = tui::parse_command("chop transient");
  CHECK(bare.kind == tui::CommandKind::kChopTransient);
  CHECK(bare.density == ingest::ChopDensity::kStrum);

  for (const auto& [word, density] :
       std::initializer_list<std::pair<const char*, ingest::ChopDensity>>{
           {"strum", ingest::ChopDensity::kStrum},
           {"beat", ingest::ChopDensity::kBeat},
           {"bar", ingest::ChopDensity::kBar}}) {
    const tui::Command command = tui::parse_command(std::string{"chop transient "} + word);
    INFO("density: " << word);
    CHECK(command.kind == tui::CommandKind::kChopTransient);
    CHECK(command.density == density);
  }

  // `slice` is accepted wherever `chop` is, densities included.
  CHECK(tui::parse_command("slice transient bar").density == ingest::ChopDensity::kBar);
}

TEST_CASE("an unknown density is refused by name", "[command]") {
  // Named rather than silently ignored: a typo that quietly gave the default
  // would be a chop you thought you had asked for and had not.
  const tui::Command command = tui::parse_command("chop transient phrase");
  REQUIRE(command.kind == tui::CommandKind::kError);
  CHECK(command.message.find("phrase") != std::string::npos);
  CHECK(command.message.find("strum") != std::string::npos);

  // Junk AFTER a valid density is still ignored, per the policy above.
  CHECK(tui::parse_command("chop transient beat extra").kind == tui::CommandKind::kChopTransient);
}

TEST_CASE("pitch reads semitones and ratios", "[command]") {
  // SIGNED MEANS SEMITONES, unsigned means a ratio. The one genuinely ambiguous
  // rule in this grammar, which is why the confirmation says both units back.
  const tui::Command up = tui::parse_command("pitch 3 +7");
  REQUIRE(up.kind == tui::CommandKind::kPadPitch);
  CHECK(up.pad == 3);
  CHECK(up.decibels == Catch::Approx(1.4983F).margin(0.001));

  const tui::Command down = tui::parse_command("pitch 3 -12");
  REQUIRE(down.kind == tui::CommandKind::kPadPitch);
  CHECK(down.decibels == Catch::Approx(0.5F).margin(0.001));

  // Unsigned is a ratio, verbatim.
  const tui::Command ratio = tui::parse_command("pitch 3 1.5");
  REQUIRE(ratio.kind == tui::CommandKind::kPadPitch);
  CHECK(ratio.decibels == Catch::Approx(1.5F));

  // And the two agree on what a fifth is, which is the whole point of accepting
  // both: `+7` and `1.4983` are the same speed.
  CHECK(up.decibels == Catch::Approx(ratio.decibels).margin(0.01));
}

TEST_CASE("a pitch out of range is refused by name", "[command]") {
  CHECK(tui::parse_command("pitch 3 +99").kind == tui::CommandKind::kError);
  CHECK(tui::parse_command("pitch 3 200").kind == tui::CommandKind::kError);
  CHECK(tui::parse_command("pitch 3 0").kind == tui::CommandKind::kError);
  CHECK(tui::parse_command("pitch 3 zzz").kind == tui::CommandKind::kError);
  CHECK(tui::parse_command("pitch 99 +7").kind == tui::CommandKind::kError);
  CHECK(tui::parse_command("pitch 3").kind == tui::CommandKind::kError);

  // The refusal for an out-of-range RATIO names the other unit, because a person
  // typing `pitch 3 7` meaning seven semitones needs to know why 7 was refused.
  const tui::Command command = tui::parse_command("pitch 3 200");
  CHECK(command.message.find("semitones") != std::string::npos);
}

TEST_CASE("reverse takes a pad and an optional switch", "[command]") {
  const tui::Command bare = tui::parse_command("reverse 3");
  REQUIRE(bare.kind == tui::CommandKind::kPadReverse);
  CHECK(bare.pad == 3);
  CHECK(bare.toggle == tui::Switch::kToggle);  // bare flips, as every toggle here does

  CHECK(tui::parse_command("reverse 3 on").toggle == tui::Switch::kOn);
  CHECK(tui::parse_command("reverse 3 off").toggle == tui::Switch::kOff);

  CHECK(tui::parse_command("reverse").kind == tui::CommandKind::kError);
  CHECK(tui::parse_command("reverse 99").kind == tui::CommandKind::kError);
  CHECK(tui::parse_command("reverse 3 backwards").kind == tui::CommandKind::kError);
}
