#include "tui/render.hpp"
#include "tui/render_detail.hpp"
#include "tui/theme.hpp"
#include "tui/ui_state.hpp"
#include "tui/waveform.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// CHOP: turning the knob while watching what it does.
//
// A fifth pure function of the same UiState, beside PERFORM, EDIT, MIX and
// BROWSE.
//
// A DEPARTURE THE MOCKUPS DO NOT COVER: they draw no chop-tuning screen. It is
// its own screen rather than a panel in EDIT for a reason that is about
// attention rather than space -- EDIT is for moving ONE boundary, and this is
// for moving ALL of them at once. The two want opposite things from the same
// waveform, and a mode inside EDIT would make `h` mean "nudge this slice" or
// "make every slice different" depending on a state you cannot see.
//
// The detection function is drawn because docs/ROADMAP.md asks for it and
// because it is the only thing that explains a bad chop: a peak below the line
// is a hit that was not taken, and a stretch of line sitting above every peak is
// a passage the detector has given up on. A slice count alone tells you it is
// wrong without telling you why.

namespace tui {
namespace {

using ftxui::Element;
using ftxui::Elements;

using detail::paint_at;
using detail::utf8_cells;
using detail::with_precision;

// The panel columns at the 100-column design grid: the terminal less its border.
[[nodiscard]] std::size_t inner_columns(std::size_t terminal_columns) {
  return terminal_columns > 2 ? terminal_columns - 2 : 1;
}

[[nodiscard]] std::string pad_to(std::string text, std::size_t cells) {
  while (utf8_cells(text) < cells) {
    text.push_back(' ');
  }
  return text;
}

// One column per cell, `│` where a slice starts.
//
// UNDER the waveform rather than drawn into it, the rule EDIT's amplitude
// gutter and BROWSE's playhead both follow: a mark painted over the trace
// replaces audio with a cursor.
[[nodiscard]] std::string boundary_row(const ChopState& chop, std::size_t cells) {
  std::string row(cells, ' ');
  if (chop.frames == 0) {
    return row;
  }
  for (const std::size_t frame : chop.boundaries) {
    const std::size_t column = std::min(cells - 1, frame * cells / chop.frames);
    row = paint_at(row, column, "│");
  }
  return row;
}

// The detection function, and the threshold that was applied to it.
//
// TWO CURVES IN ONE BAND, which needs them to be distinguishable without colour
// -- a 16-colour terminal has one accent. The flux is FILLED from the bottom
// (envelope_rows, which exists for exactly this shape) and the threshold is a
// single row of marks at its own height, so one reads as terrain and the other
// as a waterline.
[[nodiscard]] Elements detection_rows(const ChopState& chop, std::size_t cells, std::size_t rows) {
  Elements out;
  if (chop.flux.empty() || rows == 0) {
    for (std::size_t row = 0; row < rows; ++row) {
      out.push_back(ftxui::text(std::string(cells, ' ')));
    }
    return out;
  }

  std::vector<std::string> filled = envelope_rows(chop.flux, rows);

  // The waterline, painted per CHARACTER column at the threshold's height. Rows
  // count from the top, so a high threshold is a low row index.
  //
  // Stepping by kDotColumnsPerCell because the curve beneath it is stored per
  // dot column and this is drawn per cell.
  for (std::size_t column = 0; column < cells; ++column) {
    const std::size_t level_at = column * kDotColumnsPerCell;
    if (level_at >= chop.threshold.size()) {
      break;
    }
    const float level = std::clamp(chop.threshold[level_at], 0.0F, 1.0F);
    const auto from_bottom = static_cast<std::size_t>(level * static_cast<float>(rows));
    const std::size_t row = rows - std::min(rows, from_bottom + 1);
    if (row < filled.size()) {
      filled[row] = paint_at(filled[row], column, "·");
    }
  }

  for (std::string& row : filled) {
    out.push_back(ftxui::text(pad_to(std::move(row), cells)) | ftxui::color(theme::muted()));
  }
  return out;
}

struct FieldLine {
  std::string name;
  std::string value;
  std::string note;
};

[[nodiscard]] FieldLine field_line(const ChopState& chop, ChopState::Field field) {
  switch (field) {
    case ChopState::Field::kSensitivity:
      return FieldLine{.name = "sensitivity",
                       .value = with_precision(static_cast<double>(chop.lambda), 2),
                       // Said backwards from the number on purpose: the field is a threshold
                       // multiplier, so LOWER is more sensitive, and a row that showed only
                       // the number would have half its readers turning it the wrong way.
                       .note = "lower cuts more"};

    case ChopState::Field::kGap:
      return FieldLine{.name = "minimum gap",
                       .value = with_precision(chop.gap_seconds * 1'000.0, 0) + " ms",
                       .note = "no two cuts closer than this"};

    case ChopState::Field::kLowCut:
      return FieldLine{.name = "low cut",
                       .value = with_precision(static_cast<double>(chop.low_cut) * 100.0, 0) + "%",
                       .note = "ignore the bottom of the spectrum"};

    case ChopState::Field::kCount:
      break;
  }
  return FieldLine{};
}

[[nodiscard]] Element parameters_panel(const ChopState& chop, std::size_t width) {
  const std::size_t inner = inner_columns(width);
  Elements lines;

  for (std::size_t index = 0; index < static_cast<std::size_t>(ChopState::Field::kCount); ++index) {
    const auto field = static_cast<ChopState::Field>(index);
    const FieldLine line = field_line(chop, field);
    const bool selected = field == chop.field;

    std::string row(inner, ' ');
    row = paint_at(row, 1, selected ? "▸" : " ");
    row = paint_at(row, 3, line.name);
    row = paint_at(row, 18, line.value);
    row = paint_at(row, 30, line.note);

    Element drawn = ftxui::text(row);
    lines.push_back(selected ? drawn | ftxui::color(theme::accent()) | ftxui::bold
                             : drawn | ftxui::color(theme::muted()));
  }

  return ftxui::vbox(std::move(lines)) | ftxui::border |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(width));
}

}  // namespace

Element render_chop(const UiState& state, std::size_t terminal_columns, std::size_t terminal_rows) {
  const ChopState& chop = state.chop;
  const std::size_t cells = inner_columns(terminal_columns);

  // Header, blank, wave panel, detection panel, parameters, filler, mode line.
  // The two drawn panels share what is left after the fixed chrome, with the
  // waveform taking the larger share: it is the thing being cut, and the
  // detection function is the explanation.
  constexpr std::size_t kChrome = 5 + 5;  // header/blank/mode line, plus 5 param rows
  const std::size_t spare = terminal_rows > kChrome + 6 ? terminal_rows - kChrome : 6;
  const std::size_t wave_rows = std::max<std::size_t>(3, (spare * 3 / 5) - 3);
  const std::size_t flux_rows = std::max<std::size_t>(2, spare - wave_rows - 4);

  Element header = ftxui::hbox({
      ftxui::text("cratedig ") | ftxui::color(theme::bright()),
      ftxui::text(state.version + "  ") | ftxui::color(theme::muted()),
      ftxui::text("chop") | ftxui::color(theme::label()),
      ftxui::filler(),
      ftxui::text(chop.name) | ftxui::color(theme::muted()),
  });

  // The waveform, and the cuts under it.
  Elements wave;
  for (std::string& row :
       waveform_rows(state.bins, WaveformGeometry{.rows = wave_rows, .gain = 1.0F})) {
    wave.push_back(ftxui::text(pad_to(std::move(row), cells)) | ftxui::color(theme::text()));
  }
  wave.push_back(ftxui::text(boundary_row(chop, cells)) | ftxui::color(theme::accent()));

  const std::string count = std::to_string(chop.boundaries.size()) +
                            (chop.boundaries.size() == 1 ? " slice " : " slices ");
  Element wave_title = ftxui::hbox({
                           ftxui::text(" cutting ") | ftxui::color(theme::label()),
                           ftxui::filler(),
                           ftxui::text(count) | ftxui::color(theme::accent()) | ftxui::bold,
                       }) |
                       ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(terminal_columns));

  Element flux_title = ftxui::hbox({
                           ftxui::text(" detection ") | ftxui::color(theme::label()),
                           ftxui::filler(),
                           ftxui::text("· is the threshold ") | ftxui::color(theme::muted()),
                       }) |
                       ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(terminal_columns));

  std::vector<std::string> facts;
  facts.push_back(std::to_string(chop.boundaries.size()) + " slices");

  static constexpr std::array<std::string_view, 3> kHints{
      "jk pick · hl adjust · HL coarse · enter apply · esc cancel",
      "jk pick · hl adjust · enter apply · esc cancel",
      "hl adjust · enter apply",
  };

  return ftxui::vbox({
      header,
      ftxui::text(""),
      ftxui::window(std::move(wave_title), ftxui::vbox(std::move(wave))) |
          ftxui::color(theme::structure()),
      ftxui::window(std::move(flux_title), ftxui::vbox(detection_rows(chop, cells, flux_rows))) |
          ftxui::color(theme::structure()),
      parameters_panel(chop, terminal_columns),
      ftxui::filler(),
      detail::mode_line(state, terminal_columns, "  chop      ", facts, kHints, 20),
  });
}

}  // namespace tui
