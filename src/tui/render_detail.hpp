#ifndef CRATEDIG_TUI_RENDER_DETAIL_HPP
#define CRATEDIG_TUI_RENDER_DETAIL_HPP

#include "tui/keys.hpp"
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

// The pad map the caption row prints and the slice table prints in its `pad`
// column. Re-exported rather than redefined: there is ONE table, in keys.hpp,
// and this name exists so the renderers do not have to know that the map is a
// keyboard fact rather than a layout one.
using tui::kPadKeys;

[[nodiscard]] std::string with_precision(double value, int digits);

// Precision follows the zoom: at four minutes on screen, milliseconds are noise;
// at forty milliseconds, they are the only thing that varies.
[[nodiscard]] std::string format_time(double seconds, double span_seconds);

[[nodiscard]] std::string format_dbfs(float linear);

// The other direction, for the mixer's `:` verbs.
//
// BESIDE format_dbfs and not somewhere in rt/, because docs/MIXER.md is explicit
// that the audio path is linear everywhere and dB exists only at the interface
// boundary -- this IS that boundary. Keeping both directions in one file is the
// same reasoning format_bpm gives for living beside its parser: two conversions
// that must agree should not be able to drift apart.
//
// 0 dB returns exactly 1.0f rather than pow(10, 0), so a fader typed back to
// unity is bit-transparent again -- the transparency requirement the whole
// mixer rests on (docs/MIXER.md).
[[nodiscard]] float db_to_linear(float decibels);

// And back, for a fader the keys nudge: the engine stores linear, the key steps
// in dB, so the round trip has to exist. Silence returns the bottom of the
// fader rather than -infinity, which is what a clamped nudge needs.
[[nodiscard]] float linear_to_db(float linear);

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

// Writes `text` into `row` at `column`, in cells, OVERWRITING what is there and
// clipping at the end of the row rather than lengthening it.
//
// splice_at() above is the one-character version and it *grows* the string when
// handed something longer, because it removes one character and inserts several.
// A row assembled left to right that way still LOOKS right -- each write pushes
// only the spaces after it -- while being much wider than the panel it is drawn
// in, which FTXUI silently truncates.
//
// It stops looking right the moment such a row is split into an hbox, as the
// pattern lane's cursor does: FTXUI shrinks an over-wide hbox by taking cells
// from every child in proportion rather than truncating the last one, so a cell
// disappears from the LEFT of the row and everything after it slides. Rows that
// have to stay exactly as wide as their panel use this instead.
[[nodiscard]] std::string paint_at(std::string_view row, std::size_t column, std::string_view text);

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
