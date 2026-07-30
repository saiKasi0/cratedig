#ifndef CRATEDIG_TUI_RENDER_HPP
#define CRATEDIG_TUI_RENDER_HPP

#include "tui/ui_state.hpp"

#include <ftxui/dom/elements.hpp>

#include <cstddef>

namespace tui {

// The interface, as a pure function of UiState.
//
// The terminal size is a parameter rather than read from ftxui::Terminal::Size()
// on purpose: that is global state, and a renderer that consults it cannot be
// snapshot-tested at three sizes in one process. Everything the layout decides
// is decided from these two numbers.

// Below this the layout stops being an interface and starts being a mess, so it
// is replaced by a legible message saying what is needed. The design grid is
// 100x30 (docs/design); 60x20 is roughly where the pad grid and the waveform can
// still both be read.
inline constexpr std::size_t kMinColumns = 60;
inline constexpr std::size_t kMinRows = 20;

// Character columns the waveform occupies at a given terminal width -- the panel
// minus its two border columns. Callers size their peak-bin buffer from this via
// bins_for_columns() so that what was summarised is what gets drawn.
[[nodiscard]] std::size_t wave_columns_for(std::size_t terminal_columns) noexcept;

[[nodiscard]] ftxui::Element render(const UiState& state, std::size_t terminal_columns,
                                    std::size_t terminal_rows);

}  // namespace tui

#endif  // CRATEDIG_TUI_RENDER_HPP
