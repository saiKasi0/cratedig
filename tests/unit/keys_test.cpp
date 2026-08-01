#include "tui/keys.hpp"

#include "rt/pad_event.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

// The Kitty keyboard decoder.
//
// This is the file that stands between a terminal quirk and an unusable
// keyboard. Once the protocol is enabled EVERY keystroke arrives through it --
// FTXUI's own Event::Character never fires again -- so a decoder that rejects
// one form a real terminal sends does not degrade, it locks the user out. That
// is why the coverage here is about MALFORMED and OPTIONAL input rather than
// about the happy path, and why it needs no terminal to run.

namespace {

using tui::KeyAction;
using tui::KeyEvent;
using tui::parse_key;
using tui::parse_kitty_flags;

// Decodes, and fails the test rather than the process if it cannot.
//
// Dereferencing the optional inline would turn a decoder regression into a
// crash instead of a named failure, which is a worse way to find out -- and
// clang-tidy's bugprone-unchecked-optional-access is right to say so.
[[nodiscard]] KeyEvent decoded(std::string_view sequence) {
  const std::optional<KeyEvent> key = parse_key(sequence);

  // A plain `if` rather than REQUIRE(key.has_value()), because clang-tidy's
  // dataflow analysis cannot see through a Catch2 macro and would read the
  // dereference below as unchecked. FAIL() throws, so the return after it never
  // runs -- it is there to make the guard visible to the analysis as well as to
  // the reader.
  if (!key.has_value()) {
    FAIL("did not decode: " << std::string{sequence}.substr(1));
    return KeyEvent{};
  }
  return *key;
}

}  // namespace

TEST_CASE("the four spellings of a plain keypress all mean the same thing", "[unit][keys]") {
  // Every field after the key code is optional and every one of them defaults.
  // Which spelling arrives depends on the terminal and on the flags in force,
  // so all four have to decode identically or the pad map works on some
  // emulators and not others.
  for (const std::string_view sequence :
       {"\x1b[113u", "\x1b[113;1u", "\x1b[113;1:1u", "\x1b[113;;113u"}) {
    INFO("sequence: " << std::string{sequence}.substr(1));
    const KeyEvent key = decoded(sequence);
    CHECK(key.key == 'q');
    CHECK(key.modifiers == 0);
    CHECK(key.action == KeyAction::kPress);
    CHECK(key.character() == 'q');
  }
}

TEST_CASE("press, repeat and release are told apart", "[unit][keys]") {
  // The whole reason the protocol is negotiated at all. Without event types
  // there is no difference between tapping a pad and holding it.
  CHECK(decoded("\x1b[113;1:1u").action == KeyAction::kPress);
  CHECK(decoded("\x1b[113;1:2u").action == KeyAction::kRepeat);
  CHECK(decoded("\x1b[113;1:3u").action == KeyAction::kRelease);

  // A release still names the key, which is what lets it find the pad again.
  CHECK(decoded("\x1b[113;1:3u").character() == 'q');
}

TEST_CASE("the key code is the unshifted key, and the text is what was typed", "[unit][keys]") {
  // THE REASON "report associated text" IS REQUESTED. Typing a colon is
  // Shift+semicolon on a US layout, and the protocol reports the UNSHIFTED key:
  // without the text field the command line could not be opened by the key that
  // opens it.
  const KeyEvent colon = decoded("\x1b[59;2;58u");
  CHECK(colon.key == ';');
  CHECK(colon.text == ':');
  CHECK(colon.character() == ':');
  CHECK(colon.modifiers == tui::kModShift);

  // Same for a capital letter, which the view keys bind separately from the
  // lower case one.
  const KeyEvent shifted = decoded("\x1b[104;2;72u");
  CHECK(shifted.key == 'h');
  CHECK(shifted.character() == 'H');
}

TEST_CASE("modifiers have the protocol's +1 removed", "[unit][keys]") {
  CHECK(decoded("\x1b[113;1u").modifiers == 0);
  CHECK(decoded("\x1b[113;2u").modifiers == tui::kModShift);
  CHECK(decoded("\x1b[113;5u").modifiers == tui::kModCtrl);
  CHECK(decoded("\x1b[113;6u").modifiers == (tui::kModShift | tui::kModCtrl));
  CHECK(decoded("\x1b[113;9u").modifiers == tui::kModSuper);

  // A literal 0 is out of spec -- the encoding has no way to express it. Read
  // as "no modifiers" rather than subtracted, which would wrap to 255 and make
  // every modifier appear to be held at once.
  CHECK(decoded("\x1b[113;0u").modifiers == 0);
}

TEST_CASE("Escape, Enter, Tab and Backspace arrive as ordinary key codes", "[unit][keys]") {
  // With "report all keys as escape codes" these stop being control bytes and
  // become CSI-u like everything else. Their codes are the legacy ones.
  CHECK(decoded("\x1b[27u").key == 27);
  CHECK(decoded("\x1b[13u").key == 13);
  CHECK(decoded("\x1b[9u").key == 9);
  CHECK(decoded("\x1b[127u").key == 127);
  CHECK(decoded("\x1b[32u").key == ' ');
}

TEST_CASE("arrows keep their legacy form and get codes of our own", "[unit][keys]") {
  // The protocol leaves arrows as CSI A/B/C/D with the same parameter layout,
  // so they need decoding too or scrolling stops working the moment the
  // protocol is enabled.
  CHECK(decoded("\x1b[1;1A").key == tui::kKeyArrowUp);
  CHECK(decoded("\x1b[1;1B").key == tui::kKeyArrowDown);
  CHECK(decoded("\x1b[1;1C").key == tui::kKeyArrowRight);
  CHECK(decoded("\x1b[1;1D").key == tui::kKeyArrowLeft);

  // Modifiers and event types decode the same way as for `u`.
  CHECK(decoded("\x1b[1;1:3D").action == KeyAction::kRelease);

  // The codes are above the largest Unicode codepoint, so they cannot collide
  // with a character a terminal actually sent.
  CHECK(tui::kKeyArrowUp > 0x10'FFFF);
}

TEST_CASE("a key code in the functional range is refused", "[unit][keys]") {
  // Otherwise a terminal claiming key 0x110001 would silently arrive as an
  // arrow. The reserved range is ours precisely because nothing real is in it.
  CHECK_FALSE(parse_key("\x1b[1114113u").has_value());
  CHECK_FALSE(parse_key("\x1b[1114112u").has_value());

  // The codepoint immediately below it is still a key, absurd as it is.
  CHECK(parse_key("\x1b[1114111u").has_value());
}

TEST_CASE("the capability reply is not mistaken for a keystroke", "[unit][keys]") {
  // `CSI ? 0 u` ends in `u` and is full of digits, exactly like a key event.
  // Reading it as one would trigger whatever pad happens to be at that code
  // every time the terminal answers the query.
  CHECK_FALSE(parse_key("\x1b[?0u").has_value());
  CHECK_FALSE(parse_key("\x1b[?27u").has_value());

  CHECK(parse_kitty_flags("\x1b[?0u") == 0U);
  CHECK(parse_kitty_flags("\x1b[?27u") == 27U);
  CHECK(parse_kitty_flags("\x1b[?1u") == 1U);
}

TEST_CASE("a keystroke is not mistaken for the capability reply", "[unit][keys]") {
  // The other direction, which matters because the reply is what turns the
  // protocol on: reading a keystroke as one would enable it against a terminal
  // that never claimed to implement it.
  CHECK_FALSE(parse_kitty_flags("\x1b[113u").has_value());
  CHECK_FALSE(parse_kitty_flags("\x1b[113;1:3u").has_value());
  CHECK_FALSE(parse_kitty_flags("\x1b[?u").has_value());  // the query itself, echoed back
}

TEST_CASE("malformed input is refused rather than guessed at", "[unit][keys]") {
  for (const std::string_view sequence : {
           "", "\x1b", "\x1b[",
           "\x1b[u",       // names no key
           "\x1b[;1:3u",   // ...even with everything else present
           "q",            // not a sequence at all
           "\x1b]113u",    // OSC, not CSI
           "\x1b[113",     // no final byte
           "\x1b[113x",    // a final byte we do not bind
           "\x1b[113;1M",  // a mouse report
           "\x1b[abcu",    // not numbers
           "\x1b[113 ;1u",
           "\x1b[>27u",  // the push we send, echoed back
           "\x1b[<u",    // the pop we send, echoed back
       }) {
    INFO("sequence: " << std::string{sequence});
    CHECK_FALSE(parse_key(sequence).has_value());
  }
}

TEST_CASE("an unknown event type is refused, not treated as a press", "[unit][keys]") {
  // A stray event decoded as a press would play a pad nobody hit, which is
  // worse than a keystroke that does nothing.
  CHECK_FALSE(parse_key("\x1b[113;1:0u").has_value());
  CHECK_FALSE(parse_key("\x1b[113;1:4u").has_value());
  CHECK_FALSE(parse_key("\x1b[113;1:99u").has_value());
}

TEST_CASE("numbers that do not fit are refused", "[unit][keys]") {
  // from_chars reports the overflow rather than wrapping. A wrapped key code
  // would be a valid-looking key that nobody pressed.
  CHECK_FALSE(parse_key("\x1b[99999999999u").has_value());
  CHECK_FALSE(parse_key("\x1b[113;99999999999u").has_value());
  CHECK_FALSE(parse_kitty_flags("\x1b[?99999999999u").has_value());
}

TEST_CASE("alternate key codes are skipped rather than confusing the key", "[unit][keys]") {
  // A terminal with "report alternate keys" on sends `key:shifted:base`. We do
  // not ask for it, but a terminal already in that mode when we attach will
  // send it anyway, and the base key is still the first sub-parameter.
  const KeyEvent key = decoded("\x1b[104:72:104;2;72u");
  CHECK(key.key == 'h');
  CHECK(key.character() == 'H');
}

TEST_CASE("the flags we push are the flags we documented", "[unit][keys]") {
  // The push is a literal string and the flag set is a number, so they can
  // drift. 27 is 1|2|8|16: disambiguate, event types, all-keys-as-escape-codes,
  // associated text.
  CHECK(tui::kKittyFlags == 27U);
  CHECK(std::string{tui::kKittyPush} == "\x1b[>" + std::to_string(tui::kKittyFlags) + "u");
}

TEST_CASE("utf8_encode covers the four lengths and refuses what is not a character",
          "[unit][keys]") {
  CHECK(tui::utf8_encode('q') == "q");
  CHECK(tui::utf8_encode(':') == ":");
  CHECK(tui::utf8_encode(0x00E9) == "\xc3\xa9");           // e-acute, two bytes
  CHECK(tui::utf8_encode(0x00B7) == "\xc2\xb7");           // the separator the mode line uses
  CHECK(tui::utf8_encode(0x2502) == "\xe2\x94\x82");       // a box-drawing bar, three bytes
  CHECK(tui::utf8_encode(0x1F600) == "\xf0\x9f\x98\x80");  // four bytes

  // Surrogates and out-of-range values are not characters. Encoding them anyway
  // would put a byte sequence on the command line that Backspace cannot remove,
  // because it does not correspond to a glyph.
  CHECK(tui::utf8_encode(0xD800).empty());
  CHECK(tui::utf8_encode(0xDFFF).empty());
  CHECK(tui::utf8_encode(0x110000).empty());
}

TEST_CASE("utf8_decode_first is the inverse, and refuses what encode would not emit",
          "[unit][keys]") {
  for (const std::uint32_t codepoint : {0x71U, 0x3AU, 0xB7U, 0xE9U, 0x2502U, 0x1F600U}) {
    INFO("codepoint: " << codepoint);
    CHECK(tui::utf8_decode_first(tui::utf8_encode(codepoint)) == codepoint);
  }

  // Only the FIRST character, because that is what a keystroke is.
  CHECK(tui::utf8_decode_first("qwer") == 'q');

  for (const std::string_view text : {
           "",
           "\x80",                  // a continuation byte with nothing in front of it
           "\xc3",                  // a two-byte lead with its second byte missing
           "\xe2\x94",              // a three-byte lead one byte short
           "\xc3\x28",              // a lead followed by something that is not a continuation
           "\xc0\xaf",              // overlong: '/' encoded in two bytes
           "\xe0\x80\xaf",          // overlong again, in three
           "\xed\xa0\x80",          // a surrogate, which is not a character
           "\xf8\x88\x80\x80\x80",  // five bytes, which UTF-8 has not had since 2003
       }) {
    INFO("bytes: " << text.size());
    CHECK(tui::utf8_decode_first(text) == 0);
  }
}

// -- the pad map ---------------------------------------------------------------

TEST_CASE("the pad map is one table, and the lookup reads it", "[unit][keys]") {
  // There were TWO tables until M4.5 -- one in app.cpp deciding what a keystroke
  // played, one in render_detail.hpp deciding what the caption row printed --
  // with nothing checking they agreed. Rotating the map is exactly the change
  // that updates one and not the other, and the failure would be silent: the
  // legend on screen would name a key that plays a different pad.
  //
  // WHAT THIS TEST CAN AND CANNOT DO. It asserts the lookup reads the table; it
  // cannot assert that nobody adds a second table somewhere else, because a
  // table inside app.cpp's anonymous namespace is not reachable from here. The
  // end-to-end version of that claim is in tests/e2e/pty_sequencer_session.py,
  // which presses the keys the caption row prints and checks which lane row
  // lights up -- the only place where "the legend and the keyboard agree" is a
  // statement about the whole program.
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    const std::string_view key = tui::kPadKeys[pad];
    INFO("pad " << pad + 1 << " is " << key);
    REQUIRE(key.size() == 1);  // pad_for_key compares one codepoint
    CHECK(tui::pad_for_key(static_cast<std::uint32_t>(key[0])) == pad);
  }
}

TEST_CASE("the pad map runs in pad order, number row first", "[unit][keys]") {
  // The rotation itself, stated as the thing a player sees: the top row of the
  // grid is the top row of the keyboard. It read `qwer asdf zxcv 1234` until
  // M4.5, which put pads 13-16 above pads 1-4 under the hand.
  const std::array<std::string_view, rt::kNumPads> expected{"1", "2", "3", "4", "q", "w", "e", "r",
                                                            "a", "s", "d", "f", "z", "x", "c", "v"};
  CHECK(tui::kPadKeys == expected);

  // Spot-checked from the player's end as well, so a wholesale re-ordering that
  // also updated the line above would still have to be deliberate.
  CHECK(tui::pad_for_key('1') == 0);
  CHECK(tui::pad_for_key('q') == 4);
  CHECK(tui::pad_for_key('z') == 12);
  CHECK(tui::pad_for_key('v') == 15);
}

TEST_CASE("a key that is not on the pad map plays nothing", "[unit][keys]") {
  // kNumPads is the "no pad" answer rather than -1, so a caller that forgets to
  // check reads past the end of a sixteen-element array instead of indexing with
  // a negative number -- both are bugs, but the first is the one ASan catches.
  for (const std::uint32_t code :
       {std::uint32_t{'p'}, std::uint32_t{'t'}, std::uint32_t{'['}, std::uint32_t{' '},
        std::uint32_t{'.'}, std::uint32_t{0}, tui::kKeyArrowLeft, std::uint32_t{0x1F600}}) {
    INFO("code " << code);
    CHECK(tui::pad_for_key(code) == rt::kNumPads);
  }

  // Every pad key is distinct: a duplicate would make one pad unreachable and
  // the caption row would still print it.
  std::array<std::string_view, rt::kNumPads> sorted = tui::kPadKeys;
  std::sort(sorted.begin(), sorted.end());
  CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
}
