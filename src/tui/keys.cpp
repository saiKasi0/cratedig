#include "tui/keys.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tui {
namespace {

constexpr std::string_view kCsi = "\x1b[";

// The sub-parameter separator is `:` and the parameter separator is `;`. Both
// levels are optional and both default, which is most of what makes this
// encoding awkward to read and easy to get wrong -- so it is parsed once, here.

// ABSENT AND MALFORMED ARE DIFFERENT ANSWERS, and keeping them apart is most of
// the correctness here. An absent field takes its documented default; a
// malformed one invalidates the whole sequence. Collapsing the two would let
// `CSI 113;99999999999u` -- a modifier field that does not fit in 32 bits --
// decode as a plain unmodified press of `q`.
struct Field {
  bool present = false;
  bool valid = true;
  std::uint32_t value = 0;
};

// One `:`-separated sub-parameter, as a number.
[[nodiscard]] Field sub_param(std::string_view field, std::size_t index) {
  std::size_t start = 0;
  for (std::size_t seen = 0; seen < index; ++seen) {
    const std::size_t colon = field.find(':', start);
    if (colon == std::string_view::npos) {
      return {};  // absent, and absent is fine
    }
    start = colon + 1;
  }
  const std::size_t colon = field.find(':', start);
  const std::string_view text =
      field.substr(start, colon == std::string_view::npos ? std::string_view::npos : colon - start);
  if (text.empty()) {
    return {};
  }

  std::uint32_t value = 0;
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto [pointer, code] = std::from_chars(first, last, value);
  if (code != std::errc{} || pointer != last) {
    return Field{.present = true, .valid = false, .value = 0};
  }
  return Field{.present = true, .valid = true, .value = value};
}

// One `;`-separated parameter. Missing parameters are empty rather than an
// error, so `CSI 113u` and `CSI 113;;113u` take the same path.
[[nodiscard]] std::string_view param(std::string_view body, std::size_t index) {
  std::size_t start = 0;
  for (std::size_t seen = 0; seen < index; ++seen) {
    const std::size_t semicolon = body.find(';', start);
    if (semicolon == std::string_view::npos) {
      return {};
    }
    start = semicolon + 1;
  }
  const std::size_t semicolon = body.find(';', start);
  return body.substr(start, semicolon == std::string_view::npos ? std::string_view::npos
                                                                : semicolon - start);
}

[[nodiscard]] std::optional<std::uint32_t> arrow_for(char final_byte) {
  switch (final_byte) {
    case 'A':
      return kKeyArrowUp;
    case 'B':
      return kKeyArrowDown;
    case 'C':
      return kKeyArrowRight;
    case 'D':
      return kKeyArrowLeft;
    default:
      return std::nullopt;
  }
}

}  // namespace

std::optional<KeyEvent> parse_key(std::string_view sequence) {
  if (sequence.size() < 3 || sequence.substr(0, kCsi.size()) != kCsi) {
    return std::nullopt;
  }
  const char final_byte = sequence.back();
  const std::string_view body = sequence.substr(kCsi.size(), sequence.size() - kCsi.size() - 1);

  // `?`, `>` and `<` introduce replies and commands, not key events. Rejected
  // explicitly so that the capability reply -- which also ends in `u` -- can
  // never be mistaken for a keystroke.
  if (!body.empty() && (body.front() == '?' || body.front() == '>' || body.front() == '<')) {
    return std::nullopt;
  }
  // Nothing but digits, `;` and `:` belongs in the parameters. Checked up front
  // so a malformed sequence is rejected whole rather than parsed as far as its
  // first bad character.
  for (const char byte : body) {
    if ((byte < '0' || byte > '9') && byte != ';' && byte != ':') {
      return std::nullopt;
    }
  }

  KeyEvent out;

  if (final_byte == 'u') {
    const Field key = sub_param(param(body, 0), 0);
    if (!key.present || !key.valid) {
      return std::nullopt;  // `CSI u` names no key; there is nothing to bind
    }
    // Above this the functional-key codes live, and those are ours. A terminal
    // claiming a key code up there would silently alias an arrow.
    if (key.value >= kFirstFunctionalKey) {
      return std::nullopt;
    }
    out.key = key.value;
  } else if (const std::optional<std::uint32_t> arrow = arrow_for(final_byte); arrow.has_value()) {
    // The legacy functional form, `CSI 1 ; modifiers:event-type A`. The first
    // parameter is the vestigial "1" and carries nothing.
    out.key = *arrow;
  } else {
    return std::nullopt;
  }

  const std::string_view modifier_field = param(body, 1);

  const Field modifiers = sub_param(modifier_field, 0);
  if (!modifiers.valid) {
    return std::nullopt;
  }
  if (modifiers.present) {
    // The protocol adds one so that an omitted field can mean "none". A literal
    // 0 is out of spec; treated as none rather than wrapping to 255, which is
    // what subtracting would otherwise do.
    out.modifiers =
        modifiers.value > 0 ? static_cast<std::uint8_t>((modifiers.value - 1) & 0xFFU) : 0;
  }

  const Field action = sub_param(modifier_field, 1);
  if (!action.valid) {
    return std::nullopt;
  }
  if (action.present) {
    if (action.value < 1 || action.value > 3) {
      return std::nullopt;  // an event type we have no meaning for is not a guess to make
    }
    out.action = static_cast<KeyAction>(action.value);
  }

  const Field text = sub_param(param(body, 2), 0);
  if (!text.valid) {
    return std::nullopt;
  }
  if (text.present) {
    out.text = text.value;
  }
  return out;
}

std::optional<std::uint32_t> parse_kitty_flags(std::string_view sequence) {
  if (sequence.size() < 4 || sequence.substr(0, kCsi.size()) != kCsi || sequence.back() != 'u') {
    return std::nullopt;
  }
  const std::string_view body = sequence.substr(kCsi.size(), sequence.size() - kCsi.size() - 1);
  if (body.empty() || body.front() != '?') {
    return std::nullopt;
  }
  const Field flags = sub_param(body.substr(1), 0);
  if (!flags.present || !flags.valid) {
    return std::nullopt;
  }
  return flags.value;
}

std::string utf8_encode(std::uint32_t codepoint) {
  // Surrogates are not characters; a terminal reporting one is reporting a bug,
  // and encoding it would put an invalid sequence on the command line.
  if (codepoint > 0x10'FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    return {};
  }

  std::string out;
  if (codepoint < 0x80) {
    out += static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    out += static_cast<char>(0xC0U | (codepoint >> 6U));
    out += static_cast<char>(0x80U | (codepoint & 0x3FU));
  } else if (codepoint < 0x1'0000) {
    out += static_cast<char>(0xE0U | (codepoint >> 12U));
    out += static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (codepoint & 0x3FU));
  } else {
    out += static_cast<char>(0xF0U | (codepoint >> 18U));
    out += static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU));
    out += static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
    out += static_cast<char>(0x80U | (codepoint & 0x3FU));
  }
  return out;
}

std::uint32_t utf8_decode_first(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  const auto lead = static_cast<unsigned char>(text[0]);

  std::size_t length = 0;
  std::uint32_t codepoint = 0;
  if (lead < 0x80) {
    return lead;
  }
  if ((lead & 0xE0U) == 0xC0U) {
    length = 2;
    codepoint = lead & 0x1FU;
  } else if ((lead & 0xF0U) == 0xE0U) {
    length = 3;
    codepoint = lead & 0x0FU;
  } else if ((lead & 0xF8U) == 0xF0U) {
    length = 4;
    codepoint = lead & 0x07U;
  } else {
    return 0;  // a continuation byte, or 0xF8+, neither of which starts a character
  }

  if (text.size() < length) {
    return 0;
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if ((byte & 0xC0U) != 0x80U) {
      return 0;  // truncated: the sequence promised more than it delivered
    }
    codepoint = (codepoint << 6U) | (byte & 0x3FU);
  }

  // Overlong encodings and surrogates decode to something that looks valid and
  // is not. Rejected rather than passed on, so what comes out of here is always
  // a character that utf8_encode would produce.
  static constexpr std::uint32_t kMinimum[] = {0, 0, 0x80, 0x800, 0x1'0000};
  if (codepoint < kMinimum[length] || codepoint > 0x10'FFFF ||
      (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    return 0;
  }
  return codepoint;
}

}  // namespace tui
