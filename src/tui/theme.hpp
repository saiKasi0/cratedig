#ifndef CRATEDIG_TUI_THEME_HPP
#define CRATEDIG_TUI_THEME_HPP

#include <ftxui/screen/color.hpp>

namespace tui::theme {

// The colour roles from docs/design/DESIGN_BRIEF.md: near-black ground, off-white
// text, and exactly one accent used sparingly -- "one glowing element per
// screen".
//
// Roles rather than colours at the call site, so that a palette change is one
// edit here and not a search through the layout. The mockups' CSS variables map
// onto them as: --cA/--cC -> structure and muted, --cD -> label, --cF -> text,
// --cG -> bright.
//
// FUNCTIONS, not constants. FTXUI 7.0.1 fixed a crash where a Color::RGB
// constructed at static-initialisation time ran before the library's own
// statics; returning by value from an inline function sidesteps the ordering
// question entirely rather than relying on that fix.
//
// The mockups' near-black background (#0c0c0b) is deliberately NOT reproduced.
// Repainting a user's whole terminal background is the kind of un-terminal
// flourish CLAUDE.md says to drop silently -- structure comes from the roles
// below, on whatever ground the terminal already has.

// Panel borders and rules: present, never competing with content.
[[nodiscard]] inline ftxui::Color structure() noexcept {
  return ftxui::Color::GrayDark;
}

// Values that are real but not the point right now.
[[nodiscard]] inline ftxui::Color muted() noexcept {
  return ftxui::Color::GrayDark;
}

// Field names, key hints, units.
[[nodiscard]] inline ftxui::Color label() noexcept {
  return ftxui::Color::GrayLight;
}

// Body text. The terminal's own foreground, so a user's colour scheme still
// looks like theirs.
[[nodiscard]] inline ftxui::Color text() noexcept {
  return ftxui::Color::Default;
}

// The one thing on the row that matters.
[[nodiscard]] inline ftxui::Color bright() noexcept {
  return ftxui::Color::White;
}

// TE orange. Given as RGB rather than palette index 202 so a truecolor terminal
// shows the real thing; FTXUI degrades it on 256- and 16-colour terminals, which
// is what keeps the brief's "max 16 colours" constraint honest.
[[nodiscard]] inline ftxui::Color accent() noexcept {
  return ftxui::Color::RGB(255, 79, 0);
}

}  // namespace tui::theme

#endif  // CRATEDIG_TUI_THEME_HPP
