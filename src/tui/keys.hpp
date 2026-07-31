#ifndef CRATEDIG_TUI_KEYS_HPP
#define CRATEDIG_TUI_KEYS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tui {

// The keyboard, decoded.
//
// WHY THIS FILE EXISTS
// --------------------
// A terminal cannot normally tell an application that a key was RELEASED. That
// is fine for a text editor and fatal for a sampler: without release there is no
// difference between tapping a pad and holding it, so a gate pad can never let
// go. The Kitty keyboard protocol fixes it, and CRATEDIG negotiates it -- but
// only after the terminal has said it implements it (docs/ARCHITECTURE.md).
//
// The catch is that the fix is all-or-nothing. Asking for key release on text
// keys requires "report all keys as escape codes", which routes EVERY keystroke
// through CSI-u; the terminal stops sending plain text and FTXUI's own
// Event::Character never fires again. So enabling the protocol means owning the
// decoder. That is what this file is.
//
// Everything here is a pure string -> struct function, tested without a
// terminal. A quirk in one emulator must not be able to take the keyboard down
// untested, and the only way to get there is for the parsing to be separable
// from the loop it feeds.

// What happened to the key. Values are the protocol's own, so the decoder does
// not have to remap them and a reader can check them against the spec.
enum class KeyAction : std::uint8_t {
  kPress = 1,
  kRepeat = 2,
  kRelease = 3,
};

// Modifier bits, with the protocol's +1 already removed -- it encodes "no
// modifiers" as 1 so that an omitted field can default to it, which is an
// encoding detail rather than something the rest of the program should carry.
inline constexpr std::uint8_t kModShift = 1;
inline constexpr std::uint8_t kModAlt = 2;
inline constexpr std::uint8_t kModCtrl = 4;
inline constexpr std::uint8_t kModSuper = 8;

// Keys that are not characters, given codes ABOVE the largest Unicode codepoint
// (0x10FFFF) so they can never collide with something a terminal actually sent.
// The decoder enforces that: a key code at or above kFirstFunctionalKey coming
// off the wire is rejected rather than trusted.
inline constexpr std::uint32_t kFirstFunctionalKey = 0x11'0000;
inline constexpr std::uint32_t kKeyArrowUp = kFirstFunctionalKey + 1;
inline constexpr std::uint32_t kKeyArrowDown = kFirstFunctionalKey + 2;
inline constexpr std::uint32_t kKeyArrowRight = kFirstFunctionalKey + 3;
inline constexpr std::uint32_t kKeyArrowLeft = kFirstFunctionalKey + 4;

// One keystroke, from either input path.
//
// The legacy path fills this in too -- always kPress, because that is all it can
// know -- so the bindings are written once against this struct rather than twice
// against two event vocabularies. That is the whole point of having it.
struct KeyEvent {
  // The UNSHIFTED key, which is what the protocol reports: pressing Shift and
  // semicolon to type a colon gives key == ';', not ':'. Bindings that care
  // about the physical key use this.
  std::uint32_t key = 0;

  // The character the keystroke produced, or 0 when it produced none -- which
  // is every release event, and every press of a key that types nothing.
  //
  // This is why the protocol's "report associated text" flag is requested. A
  // command line driven off `key` alone could not type a colon, which is the
  // character that opens it.
  std::uint32_t text = 0;

  std::uint8_t modifiers = 0;
  KeyAction action = KeyAction::kPress;

  // What this keystroke means as a character: the text if it produced any, and
  // otherwise the key itself. Release events fall back to the key, which is
  // what lets "release of q" find pad 1 again.
  [[nodiscard]] std::uint32_t character() const noexcept { return text != 0 ? text : key; }

  friend bool operator==(const KeyEvent&, const KeyEvent&) = default;
};

// Decodes one key event from a CSI sequence, or nothing if it is not one.
//
// Handles the `u` form (every character key, plus Escape, Enter, Tab and
// Backspace once "report all keys as escape codes" is on) and the legacy arrow
// forms, which the protocol keeps as CSI A/B/C/D with the same parameter
// layout. Nothing else is bound, so nothing else is decoded -- an unrecognised
// sequence returns nothing and falls through to FTXUI's own parsing.
//
//   CSI key[:shifted[:base]] ; modifiers[:event-type] ; text[:more] u
//
// Every field is optional and defaults are as the spec gives them, so a plain
// press of `q` may arrive as any of `CSI 113u`, `CSI 113;1u`, `CSI 113;1:1u` or
// `CSI 113;;113u` depending on the terminal and the flags in force. All four
// mean the same thing and all four decode identically.
[[nodiscard]] std::optional<KeyEvent> parse_key(std::string_view sequence);

// Decodes the reply to the capability query: `CSI ? flags u`, giving the flags
// currently in force. A terminal that does not implement the protocol never
// sends this, which is exactly how its absence is detected -- there is no
// negative reply to wait for, so nothing is ever enabled on a guess.
[[nodiscard]] std::optional<std::uint32_t> parse_kitty_flags(std::string_view sequence);

// UTF-8 for one codepoint. Surrogates and out-of-range values give an empty
// string rather than a malformed sequence, because a malformed sequence on the
// command line is a glyph that Backspace cannot remove.
[[nodiscard]] std::string utf8_encode(std::uint32_t codepoint);

// The first codepoint of a UTF-8 string, or 0 if it does not start with one.
//
// The inverse of utf8_encode, and the other half of the same job: the legacy
// input path hands over characters as text, and the bindings are written
// against codepoints so that both paths reach them.
[[nodiscard]] std::uint32_t utf8_decode_first(std::string_view text);

// -- the negotiation, as literal bytes ---------------------------------------

// "What do you support?" A terminal that implements the protocol answers with
// its current flags; one that does not is silent, and silence is the answer.
inline constexpr std::string_view kKittyQuery = "\x1b[?u";

// 1  disambiguate escape codes  -- Escape arrives as `CSI 27u` rather than as a
//                                  bare ESC that has to be told apart from the
//                                  start of every other sequence by a timeout.
// 2  report event types         -- press, repeat and release. The reason we are
//                                  here at all.
// 8  report all keys as escape codes -- required for 2 to apply to text keys.
//                                  The spec is explicit that without it, keys
//                                  that produce text are reported as plain text
//                                  and have no release event.
// 16 report associated text     -- because the key code is always the UNSHIFTED
//                                  key. Without this, typing `:` reports `;`
//                                  and the command line cannot be opened.
//
// 4 (report alternate keys) is deliberately not requested: 16 already gives the
// character, and asking for a second spelling of it would be a second thing to
// keep consistent.
inline constexpr std::uint32_t kKittyFlags = 1U | 2U | 8U | 16U;
inline constexpr std::string_view kKittyPush = "\x1b[>27u";

// Pops one entry off the stack, undoing the push above.
//
// The stack is PER-SCREEN: the spec requires terminals to keep separate stacks
// for the main and alternate screens, so leaving the alternate screen restores
// whatever the shell had set even if this is never sent. That is what makes the
// feature safe to ship -- a crash cannot leave a user's keyboard in a state
// their shell does not understand. Sending it anyway is the tidy path.
inline constexpr std::string_view kKittyPop = "\x1b[<u";

}  // namespace tui

#endif  // CRATEDIG_TUI_KEYS_HPP
