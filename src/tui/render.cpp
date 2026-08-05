#include "tui/render.hpp"

#include "engine/take.hpp"
#include "tui/command.hpp"
#include "tui/render_detail.hpp"
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

// The formatting and UTF-8 helpers both screens use, and the pad key map.
using detail::format_dbfs;
using detail::format_time;
using detail::kPadKeys;
using detail::splice_at;
using detail::utf8_cells;
using detail::utf8_split;
using detail::utf8_take_one;
using detail::with_precision;

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

// Numbered slice boundaries, as the mockups draw them: a tick row and a number
// row.
//
// Only slices INSIDE the current view get a tick, and a number is only drawn
// when there is room for it between this boundary and the next -- at full
// zoom-out on a densely chopped file the boundaries are a couple of columns
// apart, and printing "12" across three of them turns the row into noise.
[[nodiscard]] Elements slice_ruler(const UiState& state, std::size_t columns) {
  std::string tick_row(columns, ' ');
  std::string label_row(columns, ' ');

  std::size_t previous_label_end = 0;
  bool any_visible = false;

  for (std::size_t index = 0; index < state.slices.size(); ++index) {
    const std::size_t column = state.view.column_of(state.slices[index].start_frame, columns);
    if (column >= columns) {
      continue;  // outside the view
    }
    any_visible = true;
    tick_row[column] = '|';

    // Slice numbers are 1-based on screen, as everything else the player counts
    // is: pads, bars, channels.
    const std::string number =
        index < 9 ? "0" + std::to_string(index + 1) : std::to_string(index + 1);
    const std::size_t start = column + 1;
    if (start >= previous_label_end && start + number.size() <= columns) {
      label_row.replace(start, number.size(), number);
      previous_label_end = start + number.size() + 1;
    }
  }

  if (!any_visible) {
    return {};
  }

  std::string drawn;
  for (const char cell : tick_row) {
    drawn += (cell == '|') ? "┬" : "─";
  }

  return Elements{
      ftxui::text(drawn) | ftxui::color(theme::label()),
      ftxui::text(label_row) | ftxui::color(theme::muted()),
  };
}

// A time ruler, for when there are no slices yet.
//
// The mockups draw slice markers in this row pair and slice_ruler() above does
// that -- but before anything is chopped there are none, and elapsed time is
// what a listener actually wants to read off a waveform in the meantime.
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
    // Slice markers take the row pair the moment there are slices; the time
    // ruler is what stands there until then. slice_ruler() returns nothing when
    // no boundary falls inside the view, which is a real state -- zoomed into
    // the middle of one long slice -- and the time ruler is the right answer
    // there too.
    Elements ruler = state.slices.empty() ? Elements{} : slice_ruler(state, columns);
    if (ruler.empty()) {
      ruler = wave_ruler(state, columns);
    }
    for (Element& row : ruler) {
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

// The glow ramp: four steps of intensity and weight, not four colours.
//
// In sixteen colours there is no glow -- a terminal that has one orange has one
// orange, and fading it is not available. What IS available on every terminal is
// bold, dim, and inverse, so the ramp is built from those and reads the same on
// a 16-colour console as on a truecolor one. The brightest step inverts the
// cell, which is the only way to make a character genuinely brighter than its
// neighbours without a colour to spend.
enum class GlowStep : std::uint8_t { kOff = 0, kDim, kLit, kHot };

[[nodiscard]] GlowStep glow_step(const PadState& pad) noexcept {
  const float intensity = glow_intensity(pad);
  if (intensity <= 0.0F) {
    return GlowStep::kOff;
  }
  if (intensity < 0.25F) {
    return GlowStep::kDim;
  }
  if (intensity < 0.6F) {
    return GlowStep::kLit;
  }
  return GlowStep::kHot;
}

// A SEQUENCED HIT NEVER INVERTS, however hard it was.
//
// Inversion is the loudest thing this interface can do to a cell, and it reads
// as a strike -- something hit just now, by you. The machine playing a pattern
// is a different event and should look like one: lit rather than struck. Same
// ramp otherwise, so a quiet sequenced hit is still visible; only the top step
// differs, which is where the two are actually confusable.
//
// Weight rather than colour, for the reason the ramp exists at all: in sixteen
// colours there is no glow.
[[nodiscard]] Element with_glow(Element element, GlowStep step, bool sequenced) {
  switch (step) {
    case GlowStep::kHot:
      if (sequenced) {
        return std::move(element) | ftxui::color(theme::accent()) | ftxui::bold;
      }
      return std::move(element) | ftxui::color(theme::accent()) | ftxui::bold | ftxui::inverted;
    case GlowStep::kLit:
      return std::move(element) | ftxui::color(theme::accent()) | ftxui::bold;
    case GlowStep::kDim:
      return std::move(element) | ftxui::color(theme::accent());
    case GlowStep::kOff:
      break;
  }
  return element;
}

[[nodiscard]] Element pad_cell(const UiState& state, const Layout& layout, std::size_t index,
                               std::size_t cell_width) {
  const PadState& pad = state.pads[index];
  const bool selected = index == state.selected_pad;
  const GlowStep glow = glow_step(pad);

  std::string number = index < 9 ? "0" + std::to_string(index + 1) : std::to_string(index + 1);

  // Truncated rather than allowed to push the cell wider: an over-long name on
  // one pad would otherwise shift that column's separator and break the grid.
  const std::size_t name_room = cell_width > 5 ? cell_width - 5 : 1;
  std::string name = pad.loaded ? pad.name : "--";
  if (utf8_cells(name) > name_room) {
    name = utf8_split(name, name_room).first;
  }

  // The NUMBER carries the glow, not the name. It is the fixed part of the cell,
  // so the flash is a constant two-character shape in a constant place --
  // sixteen of those read as a grid lighting up, which is the point. Glowing the
  // name instead would make each pad flash a different width.
  auto number_element = ftxui::text(number + " ");
  if (glow != GlowStep::kOff) {
    number_element = with_glow(std::move(number_element), glow, pad.glow_sequenced);
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

// The pattern lane.
//
// TWO COLUMNS OF EIGHT, where the mockup draws one column of eight. The mockup
// was drawn for a machine showing eight rows; this grid has sixteen pads, and a
// lane that showed half of them would be a lane you cannot trust -- pad 12 being
// absent looks exactly like pad 12 being empty. The panel is twelve rows tall, so
// one column of sixteen plus a ruler does not fit and two columns is the only
// layout that shows every pad at once.
//
// THE PAD NAMES ARE DROPPED, which the mockup has. They cost nine columns per
// row to repeat something already on screen six columns to the left, in the pad
// grid, at the same moment. The number is the identifier a grid is read by.
//
// Velocity is not drawn. The lane answers "which steps are on"; a shade ramp
// across four levels of block is hard to read at a glance and would trade the
// question the lane exists for against one a readout can answer better.

// A step and the space between groups: step i sits at i + i/4, so a bar of
// sixteen is nineteen columns wide.
constexpr std::size_t kStepsPerGroup = 4;

[[nodiscard]] std::size_t step_column(std::size_t step) noexcept {
  return step + (step / kStepsPerGroup);
}

// How many steps the lane can show. Longer patterns are a VIEWPORT and the
// caption says so -- silently drawing the first half of a 32-step pattern would
// make the second half look empty.
constexpr std::size_t kLaneSteps = 16;
constexpr std::size_t kLaneRows = 8;  // pads per column

// Two digits, matching the pad grid's own labels so the same pad reads the same
// way in both places.
[[nodiscard]] std::string lane_number(std::size_t index) {
  return index < 9 ? "0" + std::to_string(index + 1) : std::to_string(index + 1);
}

[[nodiscard]] std::string lane_row(const PatternView& pattern, std::size_t pad, std::size_t first,
                                   std::size_t visible) {
  std::string cells;
  cells.reserve(visible + (visible / kStepsPerGroup));
  for (std::size_t step = 0; step < visible; ++step) {
    if (step > 0 && step % kStepsPerGroup == 0) {
      cells += ' ';
    }
    cells += pattern.rows[pad].on[first + step] ? "\u2588" : "\u00b7";
  }
  return cells;
}

[[nodiscard]] Element pattern_tab(const UiState& state, std::size_t outer_width) {
  const PatternView& pattern = state.pattern;

  // The window's borders take a column each. Laying out against the OUTER width
  // puts every row two columns too wide, which FTXUI then clips -- silently for
  // the padded rows, and visibly for the caption, which came out reading
  // "... slot" as though a value had failed to render.
  const std::size_t width = outer_width > 2 ? outer_width - 2 : 0;
  const std::size_t length = std::clamp<std::size_t>(pattern.length, 1, rt::kMaxSteps);

  // THE WINDOW FOLLOWS THE CURSOR, in whole pages. A 32-step pattern is two
  // pages of sixteen and moving the cursor past the sixteenth turns to the
  // second -- which is what makes those steps reachable at all rather than
  // merely storable. Whole pages rather than scrolling one step at a time,
  // because a bar that slides under the beat ruler is a bar you cannot count.
  const std::size_t cursor = std::min(pattern.cursor_step, length - 1);
  const std::size_t first = (cursor / kLaneSteps) * kLaneSteps;
  const std::size_t visible = std::min(length - first, kLaneSteps);
  const std::size_t bar_cells = visible + ((visible - 1) / kStepsPerGroup);

  // Left column at 1, right column past the left one's bar with a gap. Computed
  // rather than fixed so a narrower panel degrades to one column instead of
  // overlapping two.
  constexpr std::size_t kLabel = 3;  // "01 "
  const std::size_t left = 1;
  const std::size_t right = left + kLabel + bar_cells + 4;
  const bool two_columns = right + kLabel + bar_cells <= width;

  std::vector<Element> lines;

  // Caption: what is playing and how long it is. `1/16` is the step resolution,
  // fixed at sixteenths (rt::kStepsPerBeat) and stated because a lane with no
  // unit is a grid of unknown speed.
  //
  // ASSEMBLED AS CLAUSES AND DROPPED WHOLE, never cut. A caption trimmed by
  // character ends in "... · slot", which reads as a value that failed to
  // render. Same fix the M3 header needed, for the same reason.
  //
  // "showing 1-N" is the one clause that is never dropped. Every other clause
  // is a detail; that one is what stops the lane claiming to be the whole
  // pattern when it is showing half of it.
  std::vector<std::string> clauses;
  clauses.push_back("pattern " + lane_number(pattern.pattern));
  const bool windowed = length > visible;
  const std::size_t truncation_clause = windowed ? clauses.size() + 1 : 0;
  clauses.push_back(std::to_string(length) + " steps");
  if (windowed) {
    clauses.push_back("showing " + std::to_string(first + 1) + "-" +
                      std::to_string(first + visible));
  }
  clauses.emplace_back("1/16");
  if (pattern.song) {
    clauses.push_back("slot " + std::to_string(pattern.slot + 1));
  }
  // Ahead of swing, because the drop loop below takes from the end and these
  // two are not equally droppable: swing is audible as a feel, while a click
  // left running is audible as a click -- and the surprising one is the one
  // worth the columns.
  if (pattern.metronome) {
    clauses.emplace_back("metro");
  }
  if (pattern.swing > 0) {
    clauses.push_back("swing " + std::to_string(pattern.swing) + "%");
  }

  const auto joined = [](const std::vector<std::string>& parts) {
    std::string text;
    for (const std::string& part : parts) {
      if (!text.empty()) {
        text += " \u00b7 ";
      }
      text += part;
    }
    return text;
  };

  // Dropped from the end, which is also least-important-first by construction --
  // swing, then slot, then the resolution. `pattern` and the truncation notice
  // are at the front and survive.
  while (clauses.size() > 1 && detail::utf8_cells(joined(clauses)) + 1 > width) {
    std::size_t victim = clauses.size() - 1;
    if (victim == truncation_clause && clauses.size() > 2) {
      victim = clauses.size() - 2;  // step over the notice rather than dropping it
    }
    clauses.erase(clauses.begin() + static_cast<std::ptrdiff_t>(victim));
  }
  lines.push_back(ftxui::text(" " + joined(clauses)) | ftxui::color(theme::label()));

  // The playhead, above the grid exactly as the mockup has it. Only when the
  // transport is on THIS pattern and inside the visible page: a marker anywhere
  // else claims a position on a grid it does not belong to, which is worse than
  // no marker at all.
  {
    std::string row(width, ' ');
    if (pattern.playhead && pattern.step >= first && pattern.step < first + visible) {
      const std::size_t at = step_column(pattern.step - first);
      if (left + kLabel + at < row.size()) {
        row = detail::paint_at(row, left + kLabel + at, "\u252f");
      }
      if (two_columns && right + kLabel + at < row.size()) {
        row = detail::paint_at(row, right + kLabel + at, "\u252f");
      }
    }
    lines.push_back(ftxui::text(row) | ftxui::color(theme::accent()));
  }

  // Where the next step edit lands, drawn as a block cursor.
  //
  // INVERTED, which the pad grid reserves for a live hit -- but this is a
  // different panel answering a different question, and a block cursor is the
  // one idiom every terminal user already reads without being told. A lane you
  // can type into and cannot see the caret of is a lane that writes to the
  // wrong step.
  //
  // Only when its pad is actually on screen. In one-column mode the lane shows
  // the first eight pads, so a cursor on pad 12 has nowhere to be drawn and
  // drawing it anyway would put it on the wrong row.
  const std::size_t cursor_pad = std::min<std::size_t>(state.selected_pad, rt::kNumPads - 1);
  const std::size_t cursor_row = cursor_pad % kLaneRows;
  const bool cursor_shown = two_columns || cursor_pad < kLaneRows;
  const std::size_t cursor_base = cursor_pad < kLaneRows ? left : right;
  const std::size_t cursor_column = cursor_base + kLabel + step_column(cursor - first);

  const std::size_t rows = two_columns ? kLaneRows : std::min<std::size_t>(kLaneRows, rt::kNumPads);
  for (std::size_t row_index = 0; row_index < rows; ++row_index) {
    std::string row(width, ' ');

    row = detail::paint_at(row, left, lane_number(row_index));
    row = detail::paint_at(row, left + kLabel, lane_row(pattern, row_index, first, visible));

    if (two_columns) {
      const std::size_t second = row_index + kLaneRows;
      if (second < rt::kNumPads) {
        row = detail::paint_at(row, right, lane_number(second));
        row = detail::paint_at(row, right + kLabel, lane_row(pattern, second, first, visible));
      }
    }

    if (cursor_shown && row_index == cursor_row && cursor_column < utf8_cells(row)) {
      auto [before, rest] = utf8_split(row, cursor_column);
      auto [under, after] = detail::utf8_take_one(rest);
      lines.push_back(ftxui::hbox({
          ftxui::text(before) | ftxui::color(theme::label()),
          ftxui::text(under) | ftxui::color(theme::accent()) | ftxui::inverted,
          ftxui::text(after) | ftxui::color(theme::label()),
      }));
      continue;
    }
    lines.push_back(ftxui::text(row) | ftxui::color(theme::label()));
  }

  // The beat ruler, under each group of four.
  {
    std::string row(width, ' ');
    // Numbered from the START OF THE PATTERN, not of the page: on the second
    // page of a 32-step pattern these are beats 5 to 8, and restarting them at
    // 1 would make the two pages indistinguishable in a screenshot.
    for (std::size_t beat = 0; beat * kStepsPerGroup < visible; ++beat) {
      const std::string label = std::to_string((first / kStepsPerGroup) + beat + 1);
      row = detail::paint_at(row, left + kLabel + step_column(beat * kStepsPerGroup), label);
      if (two_columns) {
        row = detail::paint_at(row, right + kLabel + step_column(beat * kStepsPerGroup), label);
      }
    }
    lines.push_back(ftxui::text(row) | ftxui::color(theme::muted()));
  }

  lines.push_back(ftxui::filler());
  return ftxui::vbox(std::move(lines));
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

  return ftxui::window(std::move(tab_title),
                       on_sample ? sample_tab(state, width) : pattern_tab(state, width)) |
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
  // Live counters only, in priority order. The static engine and device facts
  // live in the sample panel, where they are not competing every frame with
  // numbers that actually change.
  // The transport leads, which it could not before M4: the design README's note
  // that the mode line shows no BPM or transport "until M4 -- advertising a key
  // that does nothing is worse than not mentioning it" is now spent. Both are
  // ahead of `voices` because they are what a sequencer is read for, and the
  // one drops off the end are the ones the sample panel repeats anyway.
  std::vector<std::string> facts{
      format_bpm(state.pattern.bpm_x100) + " bpm",
      state.pattern.transport_running ? std::string{"play"} : std::string{"stop"},
  };

  // RECORD, straight after the transport and only when armed.
  //
  // Ahead of the voice count because it is the difference between playing and
  // writing, and absent otherwise for the same reason the fault counters are:
  // a line that says "not recording" every frame is spending columns to say
  // nothing. The resolution rides along because it is the one setting that
  // changes what a take comes out as, and the moment to know it is while your
  // hands are on the pads rather than afterwards.
  if (state.take.armed) {
    facts.push_back("rec 1/" + std::to_string(static_cast<int>(
                                   engine::quantise_denominator(state.take.quantise_steps))));

    // Its own cell, and only when set. Replace throws the pattern away when the
    // take starts; a destructive mode earns a column of its own rather than a
    // character somebody has to already know the meaning of.
    if (state.take.replace) {
      facts.emplace_back("replace");
    }
  }

  // THE FACTS THAT MUST SURVIVE at any width: the tempo, the transport, and the
  // record state when there is one. Everything after this point is welcome to
  // be dropped by the packer.
  //
  // Measured with the SAME arithmetic detail::mode_line packs with, per-fact
  // trailing space included. An approximation here is a fact that fits by this
  // budget and does not fit by the packer, and that is not hypothetical: the
  // first version of this was one cell short and dropped "replace" -- the
  // destructive mode -- off a 100-column terminal, which is the M4.5 lesson
  // repeating. Found by the PTY session, then by this snapshot.
  std::size_t essential = 0;
  for (const std::string& fact : facts) {
    essential += utf8_cells(fact) + (essential == 0 ? 0 : 3) + 1;
  }

  facts.push_back("voices " + std::to_string(state.active_voices) + "/" +
                  std::to_string(state.max_voices));
  // Faults outrank the level meter, and appear only when they have something to
  // say. A counter reading zero is noise competing with the keymap for the same
  // columns; a counter reading three is news, and it earns its place by showing
  // up. The peak is also in the sample panel, so it is the one to lose here.
  if (state.xruns > 0) {
    facts.push_back("xruns " + std::to_string(state.xruns));
  }
  if (state.dropped > 0) {
    facts.push_back("dropped " + std::to_string(state.dropped));
  }
  facts.push_back("peak " + format_dbfs(state.master_peak) + " dB");
  facts.push_back(state.audio_api.empty() ? std::string{"no audio"} : state.audio_api);

  // Hint tiers, longest first. The line is assembled facts-first and then given
  // the longest hint that still fits -- the same balance the mockups strike,
  // and the right one: the facts are what changed since the last frame.
  static constexpr std::string_view kHintTiers[] = {
      "1234/qwer/asdf/zxcv pads · space play · [] t step · hl scroll · +- zoom · : cmd · esc quit ",
      "1234.. pads · space play · [] t step · hl scroll · +- zoom · : cmd · esc quit ",
      // The view keys lose their WORDS before the step keys lose theirs: `hl`
      // and `+-` are two keys you can try, while `t` without "step" says
      // nothing about what it edits -- and writing a pattern is the thing M4
      // added, with no other way at all to reach it.
      "1234.. pads · space play · [] t step · hl · +- · : · esc quit ",
      "1234.. pads · space play · t step · hl +- · : · esc quit ",
      // What the 100-column design grid gets. The view keys go before `:` does,
      // because chopping is what the machine is FOR and `:` is the only way to
      // reach it, while scroll and zoom are discoverable by pressing an arrow.
      "1234.. pads · space play · t step · : · esc quit ",
      "1234.. pads · space play · t step · esc ",
      // The pad keys survive further down than anything else. They are what
      // makes the thing playable; scroll, zoom and the transport are
      // discoverable by pressing something, and a pad map is not.
      "pads 1234.. · esc ",
      "1234.. · esc ",
      "esc quit ",
  };

  // Room for "120.00 bpm · stop · voices 0/16" -- the facts worth the width at
  // any size. The hint is chosen to leave that much, rather than facts being
  // chosen to leave room for the hint, because a keymap nobody can read is the
  // same as no keymap.
  //
  // It was 26, sized for "voices n/nn · peak -x.x dB", and left at 26 the two
  // M4 facts pushed the voice count off the design grid entirely: the tempo and
  // the transport fitted, and then there was no room for anything else. The
  // peak is the one that gives way now, and it can -- the sample panel carries
  // it too, which was already the argument for it being last in this list.
  //
  // Measured against a THREE-DIGIT tempo, which is the default: at 33 it fitted
  // 92.60 bpm and not 120.00, so the voice count appeared and disappeared
  // depending on how fast the pattern was.
  constexpr std::size_t kMinFactCells = 34;

  // While a take is armed, the record state DISPLACES the voice count rather
  // than joining it -- which is why `essential` above stops before the voices
  // are added. Reserving room for both instead took 60 cells of a 100-column
  // line and collapsed the hint all the way to `pads · esc`, taking "space
  // play" off the screen of somebody who needs the transport to record anything
  // at all. The voice count gives way here, exactly as the peak gave way to the
  // transport in M4.
  //
  // Un-armed, `essential` is well under kMinFactCells and no screen moves.
  return detail::mode_line(state, columns, "  perform   ", facts, kHintTiers,
                           std::max(kMinFactCells, essential));
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

namespace {

// Every screen, before the completion menu is accounted for. See render().
[[nodiscard]] ftxui::Element render_screen(const UiState& state, std::size_t terminal_columns,
                                           std::size_t terminal_rows);

}  // namespace

// THE MENU IS SUBTRACTED FROM THE BUDGET, not drawn on top of it.
//
// Every screen builds `vbox({header, ..., filler(), mode_line})` sized to the
// terminal, so rows appended below the mode line do not push the panels up --
// they run off the bottom, and the first thing to go is the menu's own caption.
// Taking the height off the screen first is what makes the panels shrink
// instead, which is what a person expects: the waveform gets shorter while the
// menu is up and comes back when it closes.
ftxui::Element render(const UiState& state, std::size_t terminal_columns,
                      std::size_t terminal_rows) {
  // What the screen can spare: everything above the floor it needs to stay
  // legible. The menu fits itself into this rather than the screen giving way.
  const std::size_t spare = terminal_rows > kMinRows ? terminal_rows - kMinRows : 0;
  const std::size_t menu = detail::completion_rows(state.completion, spare);
  if (menu == 0) {
    return render_screen(state, terminal_columns, terminal_rows);
  }
  // PINNED TO AN EXACT HEIGHT, which the subtraction alone does not achieve.
  // Every screen ends in a `filler()`, and a filler is greedy: handed to an
  // outer vbox it expands to whatever space is going and pushes the menu off
  // the bottom of the terminal -- which looked exactly like the clipping this
  // wrapper was written to fix, one layer further out.
  return ftxui::vbox({
      render_screen(state, terminal_columns, terminal_rows - menu) |
          ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, static_cast<int>(terminal_rows - menu)),
      detail::completion_menu(state.completion, terminal_columns, spare),
  });
}

namespace {

ftxui::Element render_screen(const UiState& state, std::size_t terminal_columns,
                             std::size_t terminal_rows) {
  // The size floor is checked here and only here, so both screens get the same
  // legible message rather than one of them getting a mess.
  if (terminal_columns < kMinColumns || terminal_rows < kMinRows) {
    return too_small(terminal_columns, terminal_rows);
  }
  if (state.screen == Screen::kEdit) {
    return render_edit(state, terminal_columns, terminal_rows);
  }
  if (state.screen == Screen::kMix) {
    return render_mix(state, terminal_columns, terminal_rows);
  }
  if (state.screen == Screen::kBrowse) {
    return render_browse(state, terminal_columns, terminal_rows);
  }
  if (state.screen == Screen::kChop) {
    return render_chop(state, terminal_columns, terminal_rows);
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

}  // namespace

}  // namespace tui
