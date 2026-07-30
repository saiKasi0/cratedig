#include "tui/render.hpp"

#include "tui/theme.hpp"
#include "tui/ui_state.hpp"
#include "tui/waveform.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tui {
namespace {

using ftxui::Element;
using ftxui::Elements;

// -- layout constants, all traceable to the 100x30 design grid ----------------

constexpr std::size_t kWaveRowsMax = 5;
constexpr std::size_t kWaveRowsMin = 3;
constexpr std::size_t kWavePanelMax = 10;
constexpr std::size_t kPadsHeightFull = 13;  // 4 rows of two-line cells + separators
constexpr std::size_t kPadsHeightTight = 9;  // the same grid without the meters
constexpr std::size_t kPadColumns = 4;
constexpr std::size_t kPadRows = 4;
constexpr std::size_t kMeterCells = 8;

// From the mockup grid: the pad panel occupies columns 1-45 and the right panel
// 47-99. Pinned rather than flex-split because four pad cells of unequal width
// put the grid's separators in different places on every row, which reads as a
// broken table rather than as a grid.
constexpr std::size_t kPadPanelColumns = 45;
constexpr std::size_t kRightPanelMinColumns = 34;
constexpr std::size_t kBothPanelsMinColumns = kPadPanelColumns + 1 + kRightPanelMinColumns;

// The rows the layout cannot give up: header, the blank under it, the blank
// under the wave panel, the panel captions, and the mode line. Getting this
// count wrong is invisible at the design size and pushes the mode line off the
// bottom of a short terminal, which is how it was found.
constexpr std::size_t kFixedRows = 5;

// Vertical block characters, the meter vocabulary from docs/design/DESIGN_BRIEF.
constexpr std::string_view kMeterBlocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

// The QWERTY map the mockups print in the pad corners. Wiring all sixteen is
// M3's job; showing them now is layout, and it is what the grid is for.
constexpr std::string_view kPadKeys[] = {"q", "w", "e", "r", "a", "s", "d", "f",
                                         "z", "x", "c", "v", "1", "2", "3", "4"};

struct Layout {
  std::size_t wave_panel_height = kWavePanelMax;
  std::size_t wave_rows = kWaveRowsMax;
  bool wave_ruler = true;
  std::size_t pads_height = kPadsHeightFull;
  bool pad_meters = true;
};

[[nodiscard]] Layout layout_for(std::size_t rows) noexcept {
  Layout out;

  // The pad meters are the first thing to go: the grid still says which pads
  // hold what, which is the information the panel exists for.
  out.pad_meters = rows >= 27;
  out.pads_height = out.pad_meters ? kPadsHeightFull : kPadsHeightTight;

  const std::size_t available = rows > (kFixedRows + out.pads_height)
                                    ? rows - kFixedRows - out.pads_height
                                    : kWaveRowsMin + 2;
  out.wave_panel_height = std::clamp(available, kWaveRowsMin + 2, kWavePanelMax);

  // Two border rows, then the ruler if there is room for it and still three
  // rows of waveform left over. A two-row time ruler above a three-row waveform
  // is a ruler with nothing to measure.
  const std::size_t inside = out.wave_panel_height - 2;
  out.wave_ruler = inside >= kWaveRowsMin + 2;
  out.wave_rows = std::clamp(inside - (out.wave_ruler ? 2U : 0U), kWaveRowsMin, kWaveRowsMax);
  return out;
}

// -- small formatting helpers -------------------------------------------------

[[nodiscard]] std::string with_precision(double value, int digits) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(digits) << value;
  return out.str();
}

// Precision follows the zoom: at four minutes on screen, milliseconds are noise;
// at forty milliseconds, they are the only thing that varies.
[[nodiscard]] std::string format_time(double seconds, double span_seconds) {
  if (seconds < 0.0) {
    seconds = 0.0;
  }
  if (span_seconds >= 120.0) {
    const auto minutes = static_cast<int>(seconds / 60.0);
    return std::to_string(minutes) + ":" + (seconds - (minutes * 60.0) < 10.0 ? "0" : "") +
           with_precision(seconds - (minutes * 60.0), 0);
  }
  if (span_seconds >= 10.0) {
    return with_precision(seconds, 1) + "s";
  }
  if (span_seconds >= 1.0) {
    return with_precision(seconds, 2) + "s";
  }
  return with_precision(seconds * 1000.0, 1) + "ms";
}

[[nodiscard]] std::string format_dbfs(float linear) {
  if (linear <= 0.0F) {
    return "-inf";
  }
  return with_precision(20.0 * std::log10(static_cast<double>(linear)), 1);
}

// Cells, not bytes. Every waveform row is UTF-8 with a mix of one-byte spaces
// and three-byte braille, so byte offsets are not column offsets.
[[nodiscard]] std::size_t utf8_cells(std::string_view text) {
  std::size_t cells = 0;
  for (const char byte : text) {
    if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U) {
      ++cells;
    }
  }
  return cells;
}

[[nodiscard]] std::pair<std::string, std::string> utf8_split(std::string_view text,
                                                             std::size_t cells) {
  std::size_t seen = 0;
  std::size_t offset = 0;
  while (offset < text.size()) {
    if ((static_cast<unsigned char>(text[offset]) & 0xC0U) != 0x80U) {
      if (seen == cells) {
        break;
      }
      ++seen;
    }
    ++offset;
  }
  // Walk to the end of the character we stopped on.
  while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) {
    ++offset;
  }
  return {std::string{text.substr(0, offset)}, std::string{text.substr(offset)}};
}

// Advances one whole UTF-8 character from the front.
[[nodiscard]] std::pair<std::string, std::string> utf8_take_one(std::string_view text) {
  return utf8_split(text, 1);
}

[[nodiscard]] std::string meter_bar(float level, std::size_t cells) {
  std::string bar;
  const double scaled =
      std::clamp(static_cast<double>(level), 0.0, 1.0) * static_cast<double>(cells);
  for (std::size_t cell = 0; cell < cells; ++cell) {
    const double filled = scaled - static_cast<double>(cell);
    if (filled >= 1.0) {
      bar += kMeterBlocks[7];
    } else if (filled <= 0.0) {
      bar += kMeterBlocks[0];
    } else {
      // The fractional cell, using the same eight-step block vocabulary rather
      // than rounding to full-or-empty -- a meter that only moves in eighths
      // looks broken at low levels, which is where most material lives.
      const auto step = static_cast<std::size_t>(filled * 8.0);
      bar += kMeterBlocks[std::min<std::size_t>(step, 7)];
    }
  }
  return bar;
}

// -- pieces of the screen -----------------------------------------------------

[[nodiscard]] std::string sample_summary(const UiState& state, std::size_t columns) {
  if (!state.has_sample()) {
    return "no sample loaded";
  }
  const double seconds = static_cast<double>(state.sample_frames) /
                         static_cast<double>(std::max(state.sample_rate, 1U));
  const std::string format = std::to_string(state.sample_rate) + " Hz · " +
                             (state.sample_channels == 1 ? "mono" : "stereo") + " · " +
                             with_precision(seconds, 2) + "s";

  // " cratedig  0.0.1  perform" is 25 columns; the rest is what the summary has
  // to live within. Narrow terminals get the filename alone, truncated -- which
  // is the part that identifies the thing on screen.
  const std::size_t room = columns > 27 ? columns - 27 : 0;
  std::string full = state.sample_name + " · " + format;
  if (utf8_cells(full) <= room) {
    return full;
  }
  if (utf8_cells(state.sample_name) <= room) {
    return state.sample_name;
  }
  return utf8_split(state.sample_name, room).first;
}

[[nodiscard]] Element header(const UiState& state, std::size_t columns) {
  return ftxui::hbox({
      ftxui::text(" cratedig") | ftxui::bold | ftxui::color(theme::bright()),
      ftxui::text("  " + state.version + "  ") | ftxui::color(theme::muted()),
      ftxui::text("perform") | ftxui::color(theme::label()),
      ftxui::filler(),
      ftxui::text(sample_summary(state, columns)) | ftxui::color(theme::muted()),
      ftxui::text(" "),
  });
}

// The waveform rows, with the playhead spliced through them.
[[nodiscard]] Elements wave_body(const UiState& state, const Layout& layout, std::size_t columns) {
  if (!state.has_sample() || state.bins.empty()) {
    Elements empty;
    empty.push_back(ftxui::filler());
    empty.push_back(ftxui::hbox({ftxui::filler(),
                                 ftxui::text("no sample loaded") | ftxui::color(theme::muted()),
                                 ftxui::filler()}));
    empty.push_back(
        ftxui::hbox({ftxui::filler(), ftxui::text("cratedig <file>") | ftxui::color(theme::label()),
                     ftxui::filler()}));
    empty.push_back(ftxui::filler());
    return empty;
  }

  const std::vector<std::string> rows =
      waveform_rows(state.bins, WaveformGeometry{.rows = layout.wave_rows, .gain = 1.0F});

  const std::size_t playhead_column =
      state.playing ? state.view.column_of(state.playhead_frame, columns) : columns;

  Elements body;
  body.reserve(rows.size());
  for (const std::string& row : rows) {
    if (playhead_column >= columns || playhead_column >= utf8_cells(row)) {
      body.push_back(ftxui::text(row) | ftxui::color(theme::text()));
      continue;
    }
    // The one accented element on the screen (DESIGN_BRIEF: "the orange used
    // sparingly -- one glowing element per screen"). It replaces the waveform
    // character under it rather than blending, exactly as the mockups draw it.
    auto [before, rest] = utf8_split(row, playhead_column);
    auto [under, after] = utf8_take_one(rest);
    body.push_back(ftxui::hbox({
        ftxui::text(before) | ftxui::color(theme::text()),
        ftxui::text("│") | ftxui::color(theme::accent()),
        ftxui::text(after) | ftxui::color(theme::text()),
    }));
    static_cast<void>(under);
  }
  return body;
}

// A time ruler where the mockups put slice markers. M3 replaces this row pair
// with numbered slice boundaries; until there are slices, elapsed time is the
// thing a listener actually wants to read off a waveform.
[[nodiscard]] Elements wave_ruler(const UiState& state, std::size_t columns) {
  const double rate = static_cast<double>(std::max(state.sample_rate, 1U));
  const double span_seconds = static_cast<double>(state.view.frames_visible) / rate;

  // A tick roughly every fourteen columns: close enough to read a position off,
  // far enough apart that the labels never collide.
  const std::size_t ticks = std::max<std::size_t>(columns / 14, 2);

  std::string tick_row(columns, ' ');
  std::string label_row(columns, ' ');
  for (std::size_t tick = 0; tick <= ticks; ++tick) {
    const std::size_t column = (tick * (columns - 1)) / ticks;
    tick_row[column] = '|';

    const double seconds = (static_cast<double>(state.view.first_frame) +
                            ((static_cast<double>(tick) / static_cast<double>(ticks)) *
                             static_cast<double>(state.view.frames_visible))) /
                           rate;
    const std::string label = format_time(seconds, span_seconds);

    // Right-align the last label against the edge, left-align the first, centre
    // the rest -- otherwise the end label runs off the panel.
    std::size_t start = column;
    if (tick == ticks) {
      start = column >= label.size() ? column - label.size() + 1 : 0;
    } else if (tick > 0) {
      start = column >= (label.size() / 2) ? column - (label.size() / 2) : 0;
    }
    if (start + label.size() <= label_row.size()) {
      label_row.replace(start, label.size(), label);
    }
  }

  // '|' placeholders become the box-drawing tick only now, so the string above
  // could stay a plain single-byte buffer while positions were computed.
  std::string drawn;
  for (const char cell : tick_row) {
    drawn += (cell == '|') ? "┬" : "─";
  }

  return Elements{
      ftxui::text(drawn) | ftxui::color(theme::structure()),
      ftxui::text(label_row) | ftxui::color(theme::muted()),
  };
}

[[nodiscard]] Element wave_panel(const UiState& state, const Layout& layout, std::size_t columns) {
  const double rate = static_cast<double>(std::max(state.sample_rate, 1U));
  std::string right_info;
  if (state.has_sample()) {
    const double span = static_cast<double>(state.view.frames_visible) / rate;
    right_info = " view " + format_time(static_cast<double>(state.view.first_frame) / rate, span) +
                 " + " + format_time(span, span);
    if (state.playing) {
      right_info += " · at " + format_time(static_cast<double>(state.playhead_frame) / rate, span);
    }
    right_info += " ";
  }

  Elements content;
  if (layout.wave_rows + (layout.wave_ruler ? 2U : 0U) + 1 <= layout.wave_panel_height - 2) {
    content.push_back(ftxui::text(""));  // the breathing room the mockups leave
  }
  for (Element& row : wave_body(state, layout, columns)) {
    content.push_back(std::move(row));
  }
  content.push_back(ftxui::filler());
  if (layout.wave_ruler && state.has_sample()) {
    for (Element& row : wave_ruler(state, columns)) {
      content.push_back(std::move(row));
    }
  }

  // Sized to the panel, otherwise window() shrink-wraps the title element, the
  // filler collapses to nothing, and the right-hand info ends up jammed against
  // the word "wave" instead of against the far border.
  auto title = ftxui::hbox({
                   ftxui::text(" wave ") | ftxui::color(theme::label()),
                   ftxui::filler(),
                   ftxui::text(right_info) | ftxui::color(theme::muted()),
               }) |
               ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(columns));

  return ftxui::window(std::move(title), ftxui::vbox(std::move(content))) |
         ftxui::color(theme::structure()) |
         ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, static_cast<int>(layout.wave_panel_height));
}

[[nodiscard]] Element pad_cell(const UiState& state, const Layout& layout, std::size_t index,
                               std::size_t cell_width) {
  const PadState& pad = state.pads[index];
  const bool selected = index == state.selected_pad;
  const bool sounding = state.playing && state.playhead_pad == index;

  std::string number = index < 9 ? "0" + std::to_string(index + 1) : std::to_string(index + 1);

  // Truncated rather than allowed to push the cell wider: an over-long name on
  // one pad would otherwise shift that column's separator and break the grid.
  const std::size_t name_room = cell_width > 5 ? cell_width - 5 : 1;
  std::string name = pad.loaded ? pad.name : "--";
  if (utf8_cells(name) > name_room) {
    name = utf8_split(name, name_room).first;
  }

  auto number_element = ftxui::text(number + " ");
  if (sounding) {
    number_element = std::move(number_element) | ftxui::color(theme::accent());
  } else if (pad.loaded) {
    number_element = std::move(number_element) | ftxui::color(theme::label());
  } else {
    number_element = std::move(number_element) | ftxui::color(theme::muted());
  }

  auto name_element = ftxui::text(name);
  if (pad.loaded) {
    name_element =
        std::move(name_element) | ftxui::color(selected ? theme::bright() : theme::text());
  } else {
    name_element = std::move(name_element) | ftxui::color(theme::muted());
  }

  Elements lines;
  lines.push_back(ftxui::hbox({
      ftxui::text(" "),
      std::move(number_element),
      std::move(name_element) | ftxui::flex,
  }));

  if (layout.pad_meters) {
    lines.push_back(ftxui::hbox({
        ftxui::text(" "),
        ftxui::text(meter_bar(pad.level, kMeterCells)) |
            ftxui::color(pad.level > 0.0F ? theme::accent() : theme::structure()),
        ftxui::filler(),
    }));
  }
  return ftxui::vbox(std::move(lines)) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(cell_width));
}

[[nodiscard]] Element pad_grid(const UiState& state, const Layout& layout, std::size_t width) {
  // Two border columns and three separators between four cells. The remainder
  // is spread over the leftmost cells rather than landing entirely on the last
  // one, which at 60 columns made the fourth pad three columns wider than its
  // neighbours.
  const std::size_t usable = width > 5 ? width - 5 : kPadColumns;
  const std::size_t base_width = usable / kPadColumns;
  const std::size_t extra_columns = usable % kPadColumns;

  Elements rows;
  for (std::size_t row = 0; row < kPadRows; ++row) {
    Elements cells;
    for (std::size_t column = 0; column < kPadColumns; ++column) {
      if (column > 0) {
        cells.push_back(ftxui::separator() | ftxui::color(theme::structure()));
      }
      cells.push_back(pad_cell(state, layout, (row * kPadColumns) + column,
                               base_width + (column < extra_columns ? 1 : 0)));
    }
    if (row > 0) {
      rows.push_back(ftxui::separator() | ftxui::color(theme::structure()));
    }
    rows.push_back(ftxui::hbox(std::move(cells)));
  }
  return ftxui::vbox(std::move(rows)) | ftxui::border | ftxui::color(theme::structure());
}

[[nodiscard]] Element field(std::string_view name, const std::string& value) {
  return ftxui::hbox({
      ftxui::text(" " + std::string{name}) | ftxui::color(theme::muted()) |
          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 12),
      ftxui::text(value) | ftxui::color(theme::text()),
  });
}

[[nodiscard]] Element sample_tab(const UiState& state, std::size_t width) {
  if (!state.has_sample()) {
    return ftxui::vbox({
        ftxui::filler(),
        ftxui::hbox({ftxui::filler(), ftxui::text("nothing loaded") | ftxui::color(theme::muted()),
                     ftxui::filler()}),
        ftxui::filler(),
    });
  }

  const double rate = static_cast<double>(std::max(state.sample_rate, 1U));
  const double duration = static_cast<double>(state.sample_frames) / rate;
  const double span = static_cast<double>(state.view.frames_visible) / rate;

  Elements lines;
  lines.push_back(field("file", state.sample_name));
  lines.push_back(field("format", std::to_string(state.sample_rate) + " Hz · " +
                                      (state.sample_channels == 1 ? "mono" : "stereo")));
  // The frame count is the first thing to go when the panel is narrow: it is
  // the detail you look up, not the one you read at a glance.
  lines.push_back(field("length", width >= 46 ? with_precision(duration, 3) + " s · " +
                                                    std::to_string(state.sample_frames) + " frames"
                                              : with_precision(duration, 3) + " s"));
  lines.push_back(ftxui::text(""));
  lines.push_back(
      field("view", format_time(static_cast<double>(state.view.first_frame) / rate, span) + " → " +
                        format_time(static_cast<double>(state.view.last_frame()) / rate, span)));
  lines.push_back(field(
      "zoom",
      with_precision(static_cast<double>(state.view.frames_visible) /
                         std::max<double>(1.0, static_cast<double>(state.sample_frames)) * 100.0,
                     1) +
          "% of file"));
  lines.push_back(
      field("playhead", state.playing
                            ? format_time(static_cast<double>(state.playhead_frame) / rate, span) +
                                  " · pad " + std::to_string(state.playhead_pad + 1)
                            : std::string{"—"}));
  lines.push_back(ftxui::text(""));
  lines.push_back(field(
      "engine", std::to_string(state.engine_rate) + " Hz · " +
                    (state.audio_api.empty()
                         ? std::string{"no audio"}
                         : state.audio_api + " · " + std::to_string(state.block_frames) + " fr")));
  lines.push_back(field("peak", format_dbfs(state.master_peak) + " dBFS"));
  lines.push_back(ftxui::filler());
  return ftxui::vbox(std::move(lines));
}

[[nodiscard]] Element pattern_tab() {
  return ftxui::vbox({
      ftxui::filler(),
      ftxui::hbox(
          {ftxui::filler(), ftxui::text("empty") | ftxui::color(theme::muted()), ftxui::filler()}),
      ftxui::hbox({ftxui::filler(),
                   ftxui::text("the sequencer arrives at M4") | ftxui::color(theme::structure()),
                   ftxui::filler()}),
      ftxui::filler(),
  });
}

[[nodiscard]] Element right_panel(const UiState& state, const Layout& layout, std::size_t width) {
  const bool on_sample = state.tab == PanelTab::kSample;

  auto tab_title =
      ftxui::hbox({
          ftxui::text(" sample ") | ftxui::color(on_sample ? theme::bright() : theme::muted()) |
              (on_sample ? ftxui::bold : ftxui::nothing),
          ftxui::text("│") | ftxui::color(theme::structure()),
          ftxui::text(" pattern ") | ftxui::color(on_sample ? theme::muted() : theme::bright()) |
              (on_sample ? ftxui::nothing : ftxui::bold),
          ftxui::filler(),
      }) |
      ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(width > 2 ? width - 2 : 1));

  return ftxui::window(std::move(tab_title), on_sample ? sample_tab(state, width) : pattern_tab()) |
         ftxui::color(theme::structure()) |
         ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, static_cast<int>(layout.pads_height));
}

[[nodiscard]] Element captions(std::size_t columns) {
  Elements parts;
  parts.push_back(ftxui::text(" pads  ") | ftxui::color(theme::label()));
  parts.push_back(ftxui::text("bank a") | ftxui::color(theme::muted()));
  if (columns >= 80) {
    // The QWERTY map lives here, as in the mockup, rather than in each cell:
    // sixteen ten-column cells cannot hold a name and a key legibly, and the row
    // it would cost is the meter.
    std::string keys;
    for (std::size_t index = 0; index < kPadRows * kPadColumns; ++index) {
      keys += kPadKeys[index];
      if (index % kPadColumns == kPadColumns - 1) {
        keys += ' ';
      }
    }
    parts.push_back(ftxui::text("       keys  " + keys) | ftxui::color(theme::structure()));
  }
  parts.push_back(ftxui::filler());
  return ftxui::hbox(std::move(parts));
}

[[nodiscard]] Element mode_line(const UiState& state, std::size_t columns) {
  constexpr std::string_view kPrefix = "  perform   ";

  // Live counters only, in priority order. The static engine and device facts
  // live in the sample panel, where they are not competing every frame with
  // numbers that actually change.
  std::vector<std::string> candidates{
      "voices " + std::to_string(state.active_voices) + "/" + std::to_string(state.max_voices),
  };
  // Faults outrank the level meter, and appear only when they have something to
  // say. A counter reading zero is noise competing with the keymap for the same
  // columns; a counter reading three is news, and it earns its place by showing
  // up. The peak is also in the sample panel, so it is the one to lose here.
  if (state.xruns > 0) {
    candidates.push_back("xruns " + std::to_string(state.xruns));
  }
  if (state.dropped > 0) {
    candidates.push_back("dropped " + std::to_string(state.dropped));
  }
  candidates.push_back("peak " + format_dbfs(state.master_peak) + " dB");
  candidates.push_back(state.audio_api.empty() ? std::string{"no audio"} : state.audio_api);

  // Hint tiers, longest first. The line is assembled facts-first and then given
  // the longest hint that still fits -- the same balance the mockups strike,
  // and the right one: the facts are what changed since the last frame.
  constexpr std::string_view kHintTiers[] = {
      "space play · hl scroll · +- zoom · f fit · tab · q quit ",
      "space play · hl scroll · +- zoom · q quit ",
      "space · hl · +- · q ",
      "q quit ",
  };

  // Room for "voices n/nn · peak -x.x dB" -- the two facts worth the width at
  // any size. The hint is chosen to leave that much, rather than facts being
  // chosen to leave room for the hint, because a keymap nobody can read is the
  // same as no keymap.
  constexpr std::size_t kMinFactCells = 26;

  // Cells, not bytes: the separator is a two-byte character one column wide, and
  // budgeting in bytes silently loses a column per fact.
  std::string_view hints = kHintTiers[std::size(kHintTiers) - 1];
  for (const std::string_view tier : kHintTiers) {
    if (utf8_cells(kPrefix) + utf8_cells(tier) + kMinFactCells <= columns) {
      hints = tier;
      break;
    }
  }

  const std::size_t reserved = utf8_cells(kPrefix) + utf8_cells(hints);
  std::size_t budget = columns > reserved ? columns - reserved : 0;

  std::string facts;
  for (const std::string& candidate : candidates) {
    // The +1 keeps at least one space between the last fact and the hint. A
    // mode line that exactly fills its width reads as two run-together words.
    const std::size_t extra = utf8_cells(candidate) + (facts.empty() ? 0 : 3) + 1;
    if (extra > budget) {
      break;
    }
    budget -= extra;
    facts += facts.empty() ? candidate : " · " + candidate;
  }

  return ftxui::hbox({
      ftxui::text(std::string{kPrefix}) | ftxui::color(theme::label()) | ftxui::bold,
      ftxui::text(facts) | ftxui::color(theme::muted()),
      ftxui::filler(),
      ftxui::text(std::string{hints}) | ftxui::color(theme::structure()),
  });
}

[[nodiscard]] Element too_small(std::size_t columns, std::size_t rows) {
  return ftxui::vbox({
      ftxui::filler(),
      ftxui::hbox({ftxui::filler(),
                   ftxui::text("cratedig needs a bigger terminal") | ftxui::bold |
                       ftxui::color(theme::bright()),
                   ftxui::filler()}),
      ftxui::hbox(
          {ftxui::filler(),
           ftxui::text("have " + std::to_string(columns) + "x" + std::to_string(rows) + ", need " +
                       std::to_string(kMinColumns) + "x" + std::to_string(kMinRows)) |
               ftxui::color(theme::muted()),
           ftxui::filler()}),
      ftxui::filler(),
  });
}

}  // namespace

std::size_t wave_columns_for(std::size_t terminal_columns) noexcept {
  return terminal_columns > 2 ? terminal_columns - 2 : 0;
}

ftxui::Element render(const UiState& state, std::size_t terminal_columns,
                      std::size_t terminal_rows) {
  if (terminal_columns < kMinColumns || terminal_rows < kMinRows) {
    return too_small(terminal_columns, terminal_rows);
  }

  const Layout layout = layout_for(terminal_rows);
  const std::size_t wave_columns = wave_columns_for(terminal_columns);

  // Both panels when there is room for the mockup's proportions; otherwise the
  // pad grid takes the width. The pad grid is what makes the thing playable, so
  // it is the one that survives.
  const bool both_panels = terminal_columns >= kBothPanelsMinColumns;
  const std::size_t pads_width = both_panels ? kPadPanelColumns : terminal_columns;
  const std::size_t right_width = both_panels ? terminal_columns - pads_width - 1 : 0;

  Elements panels;
  panels.push_back(pad_grid(state, layout, pads_width) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(pads_width)));
  if (both_panels) {
    panels.push_back(ftxui::text(" "));
    panels.push_back(right_panel(state, layout, right_width) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(right_width)));
  }

  return ftxui::vbox({
      header(state, terminal_columns),
      ftxui::text(""),
      wave_panel(state, layout, wave_columns),
      ftxui::text(""),
      captions(terminal_columns),
      ftxui::hbox(std::move(panels)) |
          ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, static_cast<int>(layout.pads_height)),
      ftxui::filler(),
      mode_line(state, terminal_columns),
  });
}

}  // namespace tui
