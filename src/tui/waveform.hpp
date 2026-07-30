#ifndef CRATEDIG_TUI_WAVEFORM_HPP
#define CRATEDIG_TUI_WAVEFORM_HPP

#include "ingest/peak_pyramid.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tui {

// Braille waveform rendering, as plain strings.
//
// Deliberately free of FTXUI: the interesting part is the dot arithmetic, and
// a function from peak bins to strings can be tested against literal glyphs.
// The render layer wraps the result in text() elements and colours them; it
// adds no geometry of its own.

// A braille cell is a 2x4 grid of dots, encoded as U+2800 plus an 8-bit mask.
// Two dot columns per character is what buys the waveform twice the horizontal
// resolution of the character grid -- 192 sample columns across a 96-column
// panel.
inline constexpr std::size_t kDotRowsPerCell = 4;
inline constexpr std::size_t kDotColumnsPerCell = 2;

// The dot-to-bit map, which is NOT in reading order: braille numbers dots
// 1,2,3 down the left column, 4,5,6 down the right, then 7 and 8 as a later
// addition at the bottom of each. Hence the jump to 0x40/0x80 on the last row.
//
// Indexed [dot_row][dot_column], dot_row 0 at the top.
inline constexpr std::array<std::array<std::uint8_t, kDotColumnsPerCell>, kDotRowsPerCell>
    kBrailleDotBits{{
        {0x01, 0x08},
        {0x02, 0x10},
        {0x04, 0x20},
        {0x40, 0x80},
    }};

// UTF-8 for U+2800 + dots. Mask 0 renders as a space rather than U+2800: both
// are one cell wide, but a real space keeps the committed snapshots readable and
// avoids terminals that treat blank braille as zero-width.
[[nodiscard]] std::string braille_glyph(std::uint8_t dots);

// Peak bins needed to fill `columns` character columns.
[[nodiscard]] constexpr std::size_t bins_for_columns(std::size_t columns) noexcept {
  return columns * kDotColumnsPerCell;
}

struct WaveformGeometry {
  // Character rows the waveform occupies. The centre line sits in the middle
  // one, so an odd row count looks symmetrical and an even one does not.
  std::size_t rows = 5;

  // Linear amplitude scale applied before mapping to dots. Values beyond full
  // scale clamp to the outermost dot rather than wrapping.
  float gain = 1.0F;
};

// One braille string per character row, top row first, each
// bins.size() / kDotColumnsPerCell glyphs wide.
//
// Positive amplitude is up. Silence still draws the centre line: an empty
// waveform panel and a panel showing a silent file must not look identical.
[[nodiscard]] std::vector<std::string> waveform_rows(std::span<const ingest::PeakBin> bins,
                                                     const WaveformGeometry& geometry);

}  // namespace tui

#endif  // CRATEDIG_TUI_WAVEFORM_HPP
