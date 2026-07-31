#ifndef CRATEDIG_TUI_RENDER_DETAIL_HPP
#define CRATEDIG_TUI_RENDER_DETAIL_HPP

#include "tui/ui_state.hpp"

#include <ftxui/dom/elements.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shared between the screens, and between nothing else.
//
// PERFORM and EDIT are two pure functions of the same UiState, in two
// translation units, and they format the same things -- times, decibels, and
// UTF-8 measured in columns rather than bytes. This header exists so there is
// one copy of that rather than two that drift; it is internal to src/tui/ and is
// not part of the render interface, which is still render.hpp.

namespace tui::detail {

// The QWERTY map the pad grid prints in its corners and the slice table prints
// in its `pad` column. Grid order, so index == pad number - 1.
inline constexpr std::string_view kPadKeys[] = {"q", "w", "e", "r", "a", "s", "d", "f",
                                                "z", "x", "c", "v", "1", "2", "3", "4"};

[[nodiscard]] std::string with_precision(double value, int digits);

// Precision follows the zoom: at four minutes on screen, milliseconds are noise;
// at forty milliseconds, they are the only thing that varies.
[[nodiscard]] std::string format_time(double seconds, double span_seconds);

[[nodiscard]] std::string format_dbfs(float linear);

// Cells, not bytes. Every waveform row is UTF-8 with a mix of one-byte spaces
// and three-byte braille, so byte offsets are not column offsets.
[[nodiscard]] std::size_t utf8_cells(std::string_view text);

[[nodiscard]] std::pair<std::string, std::string> utf8_split(std::string_view text,
                                                             std::size_t cells);

// Advances one whole UTF-8 character from the front.
[[nodiscard]] std::pair<std::string, std::string> utf8_take_one(std::string_view text);

// Replaces the character at `column` with `glyph`, in cells rather than bytes,
// and leaves the string alone if the column is past its end.
//
// Splicing rather than indexing is the whole point: a braille row is three bytes
// per column, so `row[column] = '|'` would corrupt one glyph and shift every
// column after it.
[[nodiscard]] std::string splice_at(std::string_view text, std::size_t column,
                                    std::string_view glyph);

// The mode line, minus the parts that make it PERFORM's or EDIT's.
//
// The `:` prompt and the message belong to NEITHER screen: a command works
// everywhere and so does its answer, and either of them takes the whole line
// when it is up. Only the mode name, the live counters and the keymap differ
// between screens, so only those are parameters -- which is what stops the two
// screens growing two subtly different prompts.
//
// `facts` are in priority order and are dropped from the end when the line runs
// out of room; `hint_tiers` are longest first and the longest that still leaves
// `min_fact_cells` for the facts wins.
[[nodiscard]] ftxui::Element mode_line(const UiState& state, std::size_t columns,
                                       std::string_view prefix,
                                       const std::vector<std::string>& facts,
                                       std::span<const std::string_view> hint_tiers,
                                       std::size_t min_fact_cells);

}  // namespace tui::detail

#endif  // CRATEDIG_TUI_RENDER_DETAIL_HPP
