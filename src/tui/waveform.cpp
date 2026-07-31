#include "tui/waveform.hpp"

#include "ingest/peak_pyramid.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tui {
namespace {

constexpr char32_t kBrailleBase = 0x2800;

// Maps an amplitude in [-1, 1] onto a dot row, 0 at the top.
//
// The +1 -> 0 direction is the whole point: screen coordinates grow downward and
// audio does not, and getting this backwards produces a waveform that is subtly,
// unfalsifiably upside down. There is a test for it.
[[nodiscard]] std::size_t dot_row_for(float value, std::size_t dot_rows) noexcept {
  const float clamped = std::clamp(value, -1.0F, 1.0F);
  const auto span = static_cast<float>(dot_rows - 1);
  const float position = (1.0F - clamped) * 0.5F * span;
  const long rounded = std::lround(position);
  return static_cast<std::size_t>(std::clamp<long>(rounded, 0, static_cast<long>(dot_rows - 1)));
}

}  // namespace

std::string braille_glyph(std::uint8_t dots) {
  if (dots == 0) {
    return " ";
  }
  const char32_t code = kBrailleBase + dots;

  // U+2800..U+28FF is always three bytes, so this needs none of the general
  // UTF-8 machinery.
  std::string glyph;
  glyph.reserve(3);
  glyph.push_back(static_cast<char>(0xE0U | static_cast<unsigned>(code >> 12U)));
  glyph.push_back(static_cast<char>(0x80U | (static_cast<unsigned>(code >> 6U) & 0x3FU)));
  glyph.push_back(static_cast<char>(0x80U | (static_cast<unsigned>(code) & 0x3FU)));
  return glyph;
}

std::vector<std::string> waveform_rows(std::span<const ingest::PeakBin> bins,
                                       const WaveformGeometry& geometry) {
  const std::size_t rows = geometry.rows;
  if (rows == 0) {
    return {};
  }

  const std::size_t columns = bins.size() / kDotColumnsPerCell;
  const std::size_t dot_rows = rows * kDotRowsPerCell;

  // One mask per character cell, laid out row-major. Built first, encoded after,
  // so the dot arithmetic never has to think about UTF-8.
  std::vector<std::uint8_t> masks(rows * columns, 0);

  for (std::size_t column = 0; column < columns; ++column) {
    for (std::size_t dot_column = 0; dot_column < kDotColumnsPerCell; ++dot_column) {
      const std::size_t bin_index = (column * kDotColumnsPerCell) + dot_column;
      const ingest::PeakBin& bin = bins[bin_index];

      const float scaled_max = bin.max * geometry.gain;
      const float scaled_min = bin.min * geometry.gain;

      // top is the row for the maximum because the axis is inverted; if a bin
      // somehow arrives with min > max, std::minmax keeps the span non-empty
      // rather than drawing nothing.
      std::size_t top = dot_row_for(scaled_max, dot_rows);
      std::size_t bottom = dot_row_for(scaled_min, dot_rows);
      if (top > bottom) {
        std::swap(top, bottom);
      }

      for (std::size_t dot_row = top; dot_row <= bottom; ++dot_row) {
        const std::size_t row = dot_row / kDotRowsPerCell;
        const std::size_t row_dot = dot_row % kDotRowsPerCell;
        masks[(row * columns) + column] |= kBrailleDotBits[row_dot][dot_column];
      }
    }
  }

  std::vector<std::string> lines;
  lines.reserve(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    std::string line;
    line.reserve(columns * 3);
    for (std::size_t column = 0; column < columns; ++column) {
      line += braille_glyph(masks[(row * columns) + column]);
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

std::vector<std::string> envelope_rows(std::span<const float> levels, std::size_t rows) {
  const std::size_t columns = levels.size() / kDotColumnsPerCell;
  if (rows == 0 || columns == 0) {
    return {};
  }
  const std::size_t dot_rows = rows * kDotRowsPerCell;

  std::vector<std::string> lines;
  lines.reserve(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    std::string line;
    line.reserve(columns * 3);
    for (std::size_t column = 0; column < columns; ++column) {
      std::uint8_t dots = 0;
      for (std::size_t dot_column = 0; dot_column < kDotColumnsPerCell; ++dot_column) {
        const float level =
            std::clamp(levels[(column * kDotColumnsPerCell) + dot_column], 0.0F, 1.0F);
        // How many dot rows up from the baseline are filled. Rounded rather
        // than truncated, so a level of exactly one fills the top dot and a
        // level just under it does not sit a whole dot low.
        const auto filled =
            static_cast<std::size_t>(std::lround(level * static_cast<float>(dot_rows)));
        for (std::size_t dot_row = 0; dot_row < kDotRowsPerCell; ++dot_row) {
          const std::size_t from_bottom = dot_rows - ((row * kDotRowsPerCell) + dot_row);
          if (from_bottom <= filled) {
            dots |= kBrailleDotBits[dot_row][dot_column];
          }
        }
      }
      line += braille_glyph(dots);
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

}  // namespace tui
