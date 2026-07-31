#include "tui/render.hpp"
#include "tui/render_detail.hpp"
#include "tui/theme.hpp"
#include "tui/ui_state.hpp"
#include "tui/waveform.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// EDIT: one slice, up close.
//
// A second pure function of the same UiState, chosen by UiState::screen. Two
// screens rather than two states, because the pad levels, the message line and
// the transport are the same facts on both, and keeping two copies of them in
// step would be a job nobody signed up for.
//
// WHERE THIS DELIBERATELY DIFFERS FROM THE MOCKUP: the amplitude labels get
// their own gutter instead of being painted over the waveform. The mockup runs
// braille behind `+1.0` and `0.0`, which hides six columns of audio while
// appearing to show them. Six columns of width is the same price, paid
// somewhere honest.

namespace tui {
namespace {

using ftxui::Element;
using ftxui::Elements;

using detail::format_dbfs;
using detail::format_time;
using detail::kPadKeys;
using detail::splice_at;
using detail::utf8_cells;
using detail::utf8_split;
using detail::utf8_take_one;
using detail::with_precision;

// The amplitude gutter: " +1.0" and one space of air before the waveform.
constexpr std::size_t kGutter = 6;

// The rows inside the panel that are not waveform: the boundary ticks above it,
// then the handle row, the times, the zero-cross ruler and the snap readout.
constexpr std::size_t kPanelExtraRows = 5;

constexpr std::size_t kWaveRowsMax = 9;
constexpr std::size_t kWaveRowsMin = 3;

// The four parameter rows plus the row of segment letters. Below this the
// envelope block cannot be a diagram, so it stops trying to be one.
constexpr std::size_t kEnvelopeMinRows = 5;

// Header, the blank under it, the blank under the panel, the two caption rows,
// two more blanks and the mode line. Everything else is the slice panel and the
// table, which share what is left.
constexpr std::size_t kFixedRows = 8;

// The slice table's own width, which is fixed rather than shared: five aligned
// number columns cannot flex, and a table that reflowed with the terminal would
// put its decimal points somewhere different at every size.
constexpr std::size_t kTableColumns = 34;

// Below this the envelope block and the slice table cannot stand side by side,
// and the table is the one to lose: it is a navigation aid, and `[`/`]` plus the
// mode line's `slice 06/16` already say where you are. The envelope has nowhere
// else to be shown at all.
constexpr std::size_t kTableMinColumns = 80;

struct Layout {
  std::size_t wave_rows = kWaveRowsMax;
  std::size_t body_rows = 6;
  bool show_table = true;
};

[[nodiscard]] Layout layout_for(std::size_t columns, std::size_t rows) noexcept {
  Layout out;
  // What the slice panel and the lower block divide between them, once the
  // panel's own five non-waveform rows and two borders are taken out. Three
  // fifths to the waveform, which reproduces the mockup exactly at thirty rows
  // (9 and 6) and degrades in the right order below it.
  const std::size_t fixed = kFixedRows + kPanelExtraRows + 2;
  const std::size_t budget = rows > fixed ? rows - fixed : kWaveRowsMin + 1;
  out.wave_rows = std::clamp(budget * 3 / 5, kWaveRowsMin, kWaveRowsMax);
  out.body_rows = budget > out.wave_rows ? budget - out.wave_rows : 1;

  // The waveform keeps an ODD number of rows, so the centre line has a row of
  // its own. With an even count the zero line falls between two rows and the
  // picture is visibly lopsided -- which is the same reason WaveformGeometry
  // says so. The spare row goes to the envelope, which does not care.
  if (out.wave_rows % 2 == 0) {
    --out.wave_rows;
    ++out.body_rows;
  }

  out.show_table = columns >= kTableMinColumns;
  return out;
}

// -- small formatters --------------------------------------------------------

[[nodiscard]] std::string two_digit(std::size_t number) {
  return number < 10 ? "0" + std::to_string(number) : std::to_string(number);
}

[[nodiscard]] std::string pad_left(std::string text, std::size_t width) {
  while (utf8_cells(text) < width) {
    text.insert(text.begin(), ' ');
  }
  return text;
}

[[nodiscard]] std::string pad_right(std::string text, std::size_t width) {
  while (utf8_cells(text) < width) {
    text += ' ';
  }
  return text;
}

// A signed frame offset, as the EDIT screen says it: "-3 smp", "+11 smp".
[[nodiscard]] std::string signed_frames(std::ptrdiff_t frames) {
  return (frames > 0 ? "+" : "") + std::to_string(frames) + " smp";
}

[[nodiscard]] std::string milliseconds(float value) {
  const auto exact = static_cast<double>(value);
  if (value >= 100.0F) {
    return with_precision(exact, 0) + " ms";
  }
  if (value >= 10.0F) {
    return with_precision(exact, 1) + " ms";
  }
  return with_precision(exact, 2) + " ms";
}

// Semitones from a playback ratio: 12 * log2(r), which is the definition rather
// than an approximation of it.
[[nodiscard]] std::string semitones(float ratio) {
  if (ratio <= 0.0F) {
    return "0.00 st";
  }
  const double steps = 12.0 * std::log2(static_cast<double>(ratio));
  return (steps > 0.0 ? "+" : "") + with_precision(steps, 2) + " st";
}

// Which pad key plays a slice, or empty. Scanned rather than stored, because
// the pads already know and a second copy would be a second thing to keep true.
[[nodiscard]] std::string key_for_slice(const UiState& state, std::size_t slice) {
  for (std::size_t pad = 0; pad < state.pads.size(); ++pad) {
    if (state.pads[pad].has_slice && state.pads[pad].slice_index == slice) {
      return std::string{kPadKeys[pad]};
    }
  }
  return "";
}

// -- the slice panel ---------------------------------------------------------

[[nodiscard]] std::size_t panel_wave_columns(std::size_t terminal_columns) noexcept {
  const std::size_t interior = terminal_columns > 2 ? terminal_columns - 2 : 1;
  return interior > kGutter ? interior - kGutter : 1;
}

// A row with the boundary rules accented and the audio not.
//
// THE ONE GLOWING THING ON THIS SCREEN is the slice being edited, drawn as its
// two rules (DESIGN_BRIEF: one accented element per screen). Splitting the row
// rather than colouring the whole of it is what keeps the accent on the two
// columns that are an edit and off the ninety that are material.
[[nodiscard]] Element with_rules(std::string_view row, std::size_t first, std::size_t second,
                                 std::string_view glyph = "│") {
  const std::size_t cells = utf8_cells(row);
  std::array<std::size_t, 2> rules{std::min(first, second), std::max(first, second)};

  Elements out;
  std::string rest{row};
  std::size_t cursor = 0;
  for (const std::size_t rule : rules) {
    if (rule >= cells || rule < cursor) {
      continue;  // outside the view, or the same column as the previous rule
    }
    auto [before, tail] = utf8_split(rest, rule - cursor);
    auto [under, after] = utf8_take_one(tail);
    static_cast<void>(under);  // replaced, not blended, as the mockups draw it
    out.push_back(ftxui::text(before) | ftxui::color(theme::text()));
    out.push_back(ftxui::text(std::string{glyph}) | ftxui::color(theme::accent()));
    rest = after;
    cursor = rule + 1;
  }
  out.push_back(ftxui::text(rest) | ftxui::color(theme::text()));
  return ftxui::hbox(std::move(out));
}

[[nodiscard]] Elements slice_body(const UiState& state, const Layout& layout, std::size_t columns,
                                  std::size_t start_column, std::size_t end_column) {
  const std::size_t wide = panel_wave_columns(columns);

  std::vector<std::string> rows;
  if (state.bins.empty()) {
    rows.assign(layout.wave_rows, std::string(wide, ' '));
  } else {
    rows = waveform_rows(state.bins, WaveformGeometry{.rows = layout.wave_rows, .gain = 1.0F});
  }

  // Labels on the top row, the middle one and the bottom one, which is where the
  // amplitudes they name actually are. An even row count has no middle row and
  // gets no zero label rather than one that is half a row out.
  const bool has_middle = (rows.size() % 2) == 1;
  const std::size_t middle = rows.size() / 2;

  Elements body;
  body.reserve(rows.size());
  for (std::size_t index = 0; index < rows.size(); ++index) {
    std::string label = "     ";
    if (index == 0) {
      label = " +1.0";
    } else if (has_middle && index == middle) {
      label = "  0.0";
    } else if (index + 1 == rows.size()) {
      label = " -1.0";
    }
    body.push_back(ftxui::hbox({
        ftxui::text(label) | ftxui::color(theme::label()),
        ftxui::text(" "),
        with_rules(rows[index], start_column, end_column),
    }));
  }
  return body;
}

// Writes `text` into `row` so that its cell `anchor` lands on `column`, sliding
// it back inside the row when that would hang off an edge.
//
// CLAMPED, not dropped. A boundary at frame 0 sits in column 0, and a label that
// disappears exactly when the boundary reaches the edge of the view is missing
// at the moment it is most likely to be looked at.
void place_at(std::string& row, std::size_t column, std::string_view text, std::size_t anchor) {
  const std::size_t width = utf8_cells(text);
  const std::size_t cells = utf8_cells(row);
  if (column >= cells || width > cells) {
    return;
  }
  const std::size_t wanted = column > anchor ? column - anchor : 0;
  const std::size_t start = std::min(wanted, cells - width);

  auto [before, rest] = utf8_split(row, start);
  auto [covered, after] = utf8_split(rest, width);
  static_cast<void>(covered);
  row = before + std::string{text} + after;
}

// `h -1 ┻ +1 l`, with the `┻` exactly on the boundary and the labels trimmed
// when there is not room for them.
//
// The keys are printed where they act, which is the mockup's idea and a good
// one: a nudge key means nothing without knowing which end it moves. The `┻` is
// the part that must not move -- it is what says WHICH boundary -- so it is the
// labels either side that give way at the edges of the panel.
void place_handle(std::string& row, std::size_t column, std::string_view left,
                  std::string_view right) {
  const std::size_t cells = utf8_cells(row);
  if (column >= cells) {
    return;
  }

  std::string head{left};
  if (utf8_cells(head) > column) {
    head = utf8_split(head, utf8_cells(head) - column).second;
  }
  std::string tail{right};
  const std::size_t room = cells - column - 1;
  if (utf8_cells(tail) > room) {
    tail = utf8_split(tail, room).first;
  }

  const std::string run = head + "┻" + tail;
  const std::size_t start = column - utf8_cells(head);
  auto [before, rest] = utf8_split(row, start);
  auto [covered, after] = utf8_split(rest, utf8_cells(run));
  static_cast<void>(covered);
  row = before + run + after;
}

[[nodiscard]] std::string handle_row(std::size_t wide, std::size_t start_column,
                                     std::size_t end_column) {
  std::string row(wide, ' ');
  place_handle(row, start_column, "h -1 ", " +1 l");
  place_handle(row, end_column, "H -1 ", " +1 L");
  return row;
}

[[nodiscard]] std::string times_row(const UiState& state, std::size_t wide,
                                    std::size_t start_column, std::size_t end_column) {
  const SliceMark& slice = state.slices[state.edit.slice];
  const auto rate = static_cast<double>(std::max(state.sample_rate, 1U));

  // Three decimals of seconds, fixed, rather than format_time's zoom-dependent
  // precision. These are POSITIONS in a file, and the two of them are read
  // against each other and against the slice table below -- a unit that changes
  // with the zoom would make the same boundary read differently at two zooms.
  const std::string start = "start " + with_precision(
                                           static_cast<double>(slice.start_frame) / rate, 3) +
                            "s";
  const std::string end =
      "end " + with_precision(static_cast<double>(slice.end_frame) / rate, 3) + "s";

  std::string row(wide, ' ');
  place_at(row, start_column, start, utf8_cells(start) / 2);
  place_at(row, end_column, end, utf8_cells(end) / 2);
  return row;
}

// `┼` at every zero crossing in view, `┃` where one is a boundary -- which is
// the whole question this row answers: did the snap land on a crossing.
[[nodiscard]] std::string zero_cross_row(const UiState& state, std::size_t wide,
                                         std::size_t start_column, std::size_t end_column) {
  std::vector<bool> tick(wide, false);
  for (const std::size_t frame : state.edit.zero_crossings) {
    const std::size_t column = state.edit.view.column_of(frame, wide);
    if (column < wide) {
      tick[column] = true;
    }
  }

  std::string row;
  for (std::size_t column = 0; column < wide; ++column) {
    if (!tick[column]) {
      row += " ";
    } else if (column == start_column || column == end_column) {
      row += "┃";
    } else {
      row += "┼";
    }
  }
  return row;
}

[[nodiscard]] std::string snap_readout(const UiState& state) {
  if (!state.edit.snap_enabled) {
    return "snap off · boundaries as the detector left them";
  }
  const SliceMark& slice = state.slices[state.edit.slice];
  // "free" rather than "0 smp": a boundary the snap did not need to move and one
  // it could not move are the same outcome, and both mean "this is where the
  // detector put it".
  const std::string start =
      slice.start_snap == 0 ? "start free" : "start snapped " + signed_frames(slice.start_snap);
  const std::string end =
      slice.end_snap == 0 ? "end free" : "end snapped " + signed_frames(slice.end_snap);
  return "zero-cross · " + start + " · " + end;
}

[[nodiscard]] Element slice_panel(const UiState& state, const Layout& layout,
                                  std::size_t columns) {
  const std::size_t wide = panel_wave_columns(columns);
  const SliceMark& slice = state.slices[state.edit.slice];

  const std::size_t start_column = state.edit.view.column_of(slice.start_frame, wide);
  const std::size_t end_column = state.edit.view.column_of(slice.end_frame, wide);

  const auto gutter = [](const std::string& text) {
    return ftxui::hbox({ftxui::text(std::string(kGutter, ' ')), ftxui::text(text)});
  };

  // The ticks go through with_rules for the same reason the waveform does: it
  // accents the two glyphs rather than the row they sit in. Colouring the whole
  // row instead would put the accent on ninety cells of empty space -- invisible, but
  // it is the accent budget, and a budget you cannot measure is not one.
  Elements inside;
  inside.push_back(ftxui::hbox({ftxui::text(std::string(kGutter, ' ')),
                                with_rules(std::string(wide, ' '), start_column, end_column,
                                           "┯")}));
  for (Element& row : slice_body(state, layout, columns, start_column, end_column)) {
    inside.push_back(std::move(row));
  }
  inside.push_back(gutter(handle_row(wide, start_column, end_column)) |
                   ftxui::color(theme::label()));
  inside.push_back(gutter(times_row(state, wide, start_column, end_column)) |
                   ftxui::color(theme::muted()));
  inside.push_back(gutter(zero_cross_row(state, wide, start_column, end_column)) |
                   ftxui::color(theme::structure()));
  inside.push_back(ftxui::hbox({
      ftxui::text("       "),
      ftxui::text(snap_readout(state)) | ftxui::color(theme::muted()),
      ftxui::filler(),
  }));

  // Frames per column is the number that says whether a single-sample nudge is
  // visible yet. The mockup's decorative "zoom 1:8" is the same fact said twice,
  // so it is said once.
  const std::size_t per_column = std::max<std::size_t>(state.edit.view.frames_visible / wide, 1);
  auto title = ftxui::hbox({
                   ftxui::text(" slice " + two_digit(state.edit.slice + 1) + " ") |
                       ftxui::color(theme::bright()) | ftxui::bold,
                   ftxui::filler(),
                   ftxui::text((state.edit.snap_enabled ? std::string{" snap zero-cross · "}
                                                        : std::string{" snap off · "}) +
                               std::to_string(per_column) + " smp/col ") |
                       ftxui::color(theme::muted()),
               }) |
               ftxui::size(ftxui::WIDTH, ftxui::EQUAL,
                           static_cast<int>(columns > 2 ? columns - 2 : 1));

  return ftxui::window(std::move(title), ftxui::vbox(std::move(inside))) |
         ftxui::color(theme::structure()) |
         ftxui::size(ftxui::HEIGHT, ftxui::EQUAL,
                     static_cast<int>(layout.wave_rows + kPanelExtraRows + 2));
}

// -- the envelope block ------------------------------------------------------

// The ADSR as a level per dot column, plus where each segment's letter goes so
// it can be printed under its own segment.
struct EnvelopeShape {
  std::vector<float> levels;
  std::array<std::size_t, 4> label_column{};
};

[[nodiscard]] EnvelopeShape envelope_shape(const EnvelopeView& envelope, std::size_t columns) {
  EnvelopeShape shape;
  const std::size_t dots = columns * kDotColumnsPerCell;
  shape.levels.assign(dots, 0.0F);
  if (dots < 4) {
    return shape;
  }

  // Sustain is given whatever the timed segments leave, so the shape reads as
  // ADSR rather than as a ramp with a kink. THE PICTURE IS A DIAGRAM, not a
  // measurement -- sustain has no duration of its own, and the numbers beside it
  // are what to read for the times.
  const float total = std::max(envelope.attack_ms + envelope.decay_ms + envelope.release_ms, 1e-6F);
  const auto span = [&](float value) {
    const float share = (value / total) * static_cast<float>(dots) * 0.6F;
    return std::max<std::size_t>(static_cast<std::size_t>(share), 1);
  };

  const std::size_t attack = std::min(span(envelope.attack_ms), dots - 3);
  const std::size_t decay = std::min(span(envelope.decay_ms), dots - attack - 2);
  const std::size_t release = std::min(span(envelope.release_ms), dots - attack - decay - 1);
  const std::size_t sustain = dots - attack - decay - release;

  const float held = std::clamp(envelope.sustain, 0.0F, 1.0F);
  std::size_t index = 0;
  for (std::size_t step = 0; step < attack; ++step, ++index) {
    shape.levels[index] = static_cast<float>(step + 1) / static_cast<float>(attack);
  }
  for (std::size_t step = 0; step < decay; ++step, ++index) {
    const float progress = static_cast<float>(step + 1) / static_cast<float>(decay);
    shape.levels[index] = 1.0F + ((held - 1.0F) * progress);
  }
  for (std::size_t step = 0; step < sustain; ++step, ++index) {
    shape.levels[index] = held;
  }
  for (std::size_t step = 0; step < release; ++step, ++index) {
    const float progress = static_cast<float>(step + 1) / static_cast<float>(release);
    shape.levels[index] = held * (1.0F - progress);
  }

  shape.label_column = {attack / 2, attack + (decay / 2), attack + decay + (sustain / 2),
                        attack + decay + sustain + (release / 2)};
  for (std::size_t& column : shape.label_column) {
    column /= kDotColumnsPerCell;
  }
  return shape;
}

// The parameter columns, which are fixed-width for the same reason the table is:
// a number that moves as the terminal is resized is a number you have to find
// again every time.
constexpr std::size_t kParamNameCells = 6;
constexpr std::size_t kParamValueCells = 9;
constexpr std::size_t kParamsCells = 2 * (kParamNameCells + kParamValueCells) + 4;

// The same numbers on one line each, for terminals too short for the curve.
//
// Not a truncated version of the block: showing `a` and dropping `d s r`
// because they did not fit would be worse than showing none of them, since the
// three that vanished are the ones you were about to compare it against.
[[nodiscard]] Elements envelope_lines(const EnvelopeView& envelope, std::size_t rows,
                                      std::size_t width) {
  const std::array<std::vector<std::string>, 2> clauses{
      std::vector<std::string>{
          "a " + milliseconds(envelope.attack_ms),
          "d " + milliseconds(envelope.decay_ms),
          "s " + format_dbfs(envelope.sustain) + " dB",
          "r " + milliseconds(envelope.release_ms),
      },
      std::vector<std::string>{
          std::string{"trig "} + (envelope.gate ? "gate" : "one-shot"),
          "choke " + (envelope.choke_group == 0 ? std::string{"off"}
                                                : std::to_string(envelope.choke_group)),
          "gain " + format_dbfs(envelope.gain) + " dB",
          "tune " + semitones(envelope.pitch_ratio),
      },
  };

  // Clauses are dropped from the END when the line runs out, never cut. A number
  // sliced in half reads as a different number, which is worse than a number
  // that is simply not there -- and the ones at the front are the ones being
  // compared against the curve.
  const std::size_t room = width > 4 ? width - 4 : 1;

  Elements block;
  for (std::size_t row = 0; row < rows; ++row) {
    if (row >= clauses.size()) {
      block.push_back(ftxui::text(""));
      continue;
    }
    std::string line;
    for (const std::string& clause : clauses[row]) {
      // Measured before it is appended, so a clause that does not fit is never
      // built into the line and then taken back out.
      const std::size_t added = utf8_cells(clause) + (line.empty() ? 0 : 3);
      if (utf8_cells(line) + added > room) {
        break;
      }
      if (!line.empty()) {
        line += " · ";
      }
      line += clause;
    }
    block.push_back(ftxui::hbox({ftxui::text("   "),
                                 ftxui::text(line) | ftxui::color(theme::bright()),
                                 ftxui::filler()}));
  }
  return block;
}

[[nodiscard]] Elements envelope_block(const UiState& state, std::size_t rows, std::size_t width) {
  const EnvelopeView& envelope = state.edit.envelope;
  if (rows < kEnvelopeMinRows) {
    return envelope_lines(envelope, rows, width);
  }

  // The curve gets the left of the block and the numbers the right, as the
  // mockup has them: the shape answers "what does this do" and the numbers
  // answer "by how much", and they are different questions. The numbers are
  // sized first because they cannot be abbreviated; the curve takes what is
  // left, which is what makes the block fit at every width rather than
  // overflowing into the table beside it.
  const std::size_t room = width > kParamsCells + 4 ? width - kParamsCells - 4 : 8;
  const std::size_t curve_width = std::clamp<std::size_t>(room, 8, 32);
  const EnvelopeShape shape = envelope_shape(envelope, curve_width);

  const std::size_t curve_rows = rows > 1 ? rows - 1 : 1;
  const std::vector<std::string> curve = envelope_rows(shape.levels, curve_rows);

  const std::array<std::pair<std::string, std::string>, 4> left{{
      {"a", milliseconds(envelope.attack_ms)},
      {"d", milliseconds(envelope.decay_ms)},
      {"s", format_dbfs(envelope.sustain) + " dB"},
      {"r", milliseconds(envelope.release_ms)},
  }};
  const std::array<std::pair<std::string, std::string>, 4> right{{
      {"trig ", envelope.gate ? "gate" : "one-shot"},
      {"choke", envelope.choke_group == 0 ? "off" : std::to_string(envelope.choke_group)},
      {"gain ", format_dbfs(envelope.gain) + " dB"},
      {"tune ", semitones(envelope.pitch_ratio)},
  }};

  Elements block;
  for (std::size_t index = 0; index < curve_rows; ++index) {
    Elements cells;
    cells.push_back(ftxui::text("   "));
    cells.push_back(ftxui::text(index < curve.size() ? curve[index] : std::string{}) |
                    ftxui::color(theme::text()));
    cells.push_back(ftxui::filler());
    if (index < left.size()) {
      cells.push_back(ftxui::text(pad_right(left[index].first, kParamNameCells)) |
                      ftxui::color(theme::label()));
      cells.push_back(ftxui::text(pad_left(left[index].second, kParamValueCells)) |
                      ftxui::color(theme::bright()));
      cells.push_back(ftxui::text("    "));
      cells.push_back(ftxui::text(pad_right(right[index].first, kParamNameCells)) |
                      ftxui::color(theme::label()));
      cells.push_back(ftxui::text(pad_right(right[index].second, kParamValueCells)) |
                      ftxui::color(theme::bright()));
    }
    block.push_back(ftxui::hbox(std::move(cells)));
  }

  static constexpr std::array<std::string_view, 4> kLetters{"a", "d", "s", "r"};
  std::string letters(curve_width, ' ');
  for (std::size_t slot = 0; slot < kLetters.size(); ++slot) {
    letters = splice_at(letters, shape.label_column[slot], kLetters[slot]);
  }
  block.push_back(
      ftxui::hbox({ftxui::text("   "), ftxui::text(letters) | ftxui::color(theme::label()),
                   ftxui::filler()}));
  return block;
}

// -- the slice table ---------------------------------------------------------

[[nodiscard]] Elements slice_table(const UiState& state, std::size_t rows) {
  const auto rate = static_cast<double>(std::max(state.sample_rate, 1U));

  // A window around the current slice rather than the first N: the row you are
  // editing has to be on screen, and scrolling a table nobody can scroll would
  // be a strange thing to build.
  const std::size_t total = state.slices.size();
  const std::size_t half = rows / 2;
  std::size_t first = state.edit.slice > half ? state.edit.slice - half : 0;
  if (first + rows > total) {
    first = total > rows ? total - rows : 0;
  }

  Elements out;
  for (std::size_t index = first; index < total && index < first + rows; ++index) {
    const SliceMark& slice = state.slices[index];
    const bool current = index == state.edit.slice;

    const std::string line = two_digit(index + 1) + "  " +
                             pad_left(with_precision(static_cast<double>(slice.start_frame) / rate,
                                                     3),
                                      6) +
                             "  " +
                             pad_left(with_precision(static_cast<double>(slice.end_frame) / rate, 3),
                                      6) +
                             "  " +
                             pad_left(with_precision(static_cast<double>(slice.end_frame -
                                                                        slice.start_frame) /
                                                         rate,
                                                     3),
                                      5) +
                             "  " + key_for_slice(state, index);

    // Bold, not accent: the accent is already spent on the boundary rules, and
    // a second orange thing would make neither of them the one to look at.
    out.push_back(ftxui::text(line) |
                  ftxui::color(current ? theme::bright() : theme::muted()) |
                  (current ? ftxui::bold : ftxui::nothing));
  }
  while (out.size() < rows) {
    out.push_back(ftxui::text(""));
  }
  return out;
}

// -- the screen --------------------------------------------------------------

[[nodiscard]] Element header(const UiState& state, std::size_t columns) {
  // Whole clauses are dropped, longest-first, rather than the string being cut.
  // Truncating mid-separator leaves a line ending in " · ", which reads as
  // something missing rather than as something deliberately left out.
  std::vector<std::string> candidates{state.sample_name};
  if (!state.slices.empty()) {
    candidates.push_back(state.sample_name + " · slice " + two_digit(state.edit.slice + 1));
    const std::string key = key_for_slice(state, state.edit.slice);
    if (!key.empty()) {
      candidates.push_back(candidates.back() + " · pad " + key);
    }
  }

  const std::size_t room = columns > 27 ? columns - 27 : 0;
  std::string right;
  for (const std::string& candidate : candidates) {
    if (utf8_cells(candidate) <= room) {
      right = candidate;
    }
  }
  if (right.empty() && !candidates.empty()) {
    right = utf8_split(candidates.front(), room).first;
  }

  return ftxui::hbox({
      ftxui::text(" cratedig") | ftxui::bold | ftxui::color(theme::bright()),
      ftxui::text("  " + state.version + "  ") | ftxui::color(theme::muted()),
      ftxui::text("edit") | ftxui::color(theme::label()),
      ftxui::filler(),
      ftxui::text(right) | ftxui::color(theme::muted()),
      ftxui::text(" "),
  });
}

[[nodiscard]] Element mode_line(const UiState& state, std::size_t columns) {
  std::vector<std::string> facts{
      "slice " + two_digit(state.edit.slice + 1) + "/" + two_digit(state.slices.size()),
  };
  // Undo outranks the pad letter. What you can take back is a fact about the
  // work; which key plays it is already in the header and in the table.
  if (state.edit.undo_depth > 0) {
    facts.push_back("undo " + std::to_string(state.edit.undo_depth));
  }
  const std::string key = key_for_slice(state, state.edit.slice);
  if (!key.empty()) {
    facts.push_back("pad " + key);
  }

  static constexpr std::string_view kHintTiers[] = {
      "[ ] slice · hl start · HL end · z zoom · u undo · esc back ",
      "[ ] slice · hl/HL nudge · z zoom · esc back ",
      // The nudge keys survive one tier further than the rest: they are the
      // whole reason this screen exists, and unlike zoom they are not
      // discoverable by pressing something and watching.
      "hl/HL nudge · [ ] · esc ",
      "esc back ",
  };
  constexpr std::size_t kMinFactCells = 18;

  return detail::mode_line(state, columns, "  edit      ", facts, kHintTiers, kMinFactCells);
}

[[nodiscard]] Element nothing_chopped(std::size_t rows) {
  Elements body;
  body.push_back(ftxui::filler());
  body.push_back(ftxui::hbox({ftxui::filler(),
                              ftxui::text("nothing chopped yet") | ftxui::color(theme::muted()),
                              ftxui::filler()}));
  body.push_back(ftxui::hbox({ftxui::filler(),
                              ftxui::text(":chop transient") | ftxui::color(theme::label()),
                              ftxui::filler()}));
  body.push_back(ftxui::filler());
  return ftxui::vbox(std::move(body)) |
         ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, static_cast<int>(rows));
}

}  // namespace

std::size_t edit_wave_columns_for(std::size_t terminal_columns) noexcept {
  return panel_wave_columns(terminal_columns);
}

Element render_edit(const UiState& state, std::size_t terminal_columns,
                    std::size_t terminal_rows) {
  const Layout layout = layout_for(terminal_columns, terminal_rows);

  Elements screen;
  screen.push_back(header(state, terminal_columns));
  screen.push_back(ftxui::text(""));

  if (state.slices.empty() || state.edit.slice >= state.slices.size()) {
    // EDIT with nothing to edit is reachable -- `:edit` before a chop, or a
    // `chop reset` while it is open -- and has to say so rather than crash or
    // draw an empty frame that looks like a bug.
    screen.push_back(nothing_chopped(layout.wave_rows + kPanelExtraRows + 2));
  } else {
    screen.push_back(slice_panel(state, layout, terminal_columns));
  }
  screen.push_back(ftxui::text(""));

  // The lower block is two columns of FIXED width, not two fillers. Both sides
  // are tables of numbers, and a filler between them lets whichever side has the
  // longer row that frame push the other one sideways -- which is how the
  // envelope's `tune 0.00 st` first arrived spliced into the slice table.
  const std::size_t table_width = layout.show_table ? kTableColumns : 0;
  const std::size_t envelope_width =
      terminal_columns > table_width ? terminal_columns - table_width : terminal_columns;

  const auto sized = [](Element element, std::size_t width) {
    return std::move(element) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(width));
  };

  Elements captions;
  captions.push_back(sized(ftxui::hbox({ftxui::text(" envelope") | ftxui::color(theme::label()),
                                        ftxui::filler()}),
                           envelope_width));
  if (layout.show_table) {
    captions.push_back(
        sized(ftxui::hbox({ftxui::text("slices  " + std::to_string(state.slices.size()) +
                                       (state.chop_algorithm.empty()
                                            ? ""
                                            : " · " + state.chop_algorithm)) |
                               ftxui::color(theme::label()),
                           ftxui::filler()}),
              table_width));
  }
  screen.push_back(ftxui::hbox(std::move(captions)));

  if (layout.show_table) {
    screen.push_back(ftxui::hbox({
        sized(ftxui::text(""), envelope_width),
        sized(ftxui::hbox({ftxui::text("nn   start     end    len  pad") |
                               ftxui::color(theme::structure()),
                           ftxui::filler()}),
              table_width),
    }));
  } else {
    screen.push_back(ftxui::text(""));
  }

  const Elements envelope = envelope_block(state, layout.body_rows, envelope_width);
  const Elements table = layout.show_table ? slice_table(state, layout.body_rows) : Elements{};
  for (std::size_t row = 0; row < layout.body_rows; ++row) {
    Element left = row < envelope.size() ? envelope[row] : ftxui::text("");
    if (!layout.show_table) {
      screen.push_back(std::move(left));
      continue;
    }
    screen.push_back(ftxui::hbox({
        sized(std::move(left), envelope_width),
        sized(ftxui::hbox({row < table.size() ? table[row] : ftxui::text(""), ftxui::filler()}),
              table_width),
    }));
  }

  screen.push_back(ftxui::filler());
  screen.push_back(mode_line(state, terminal_columns));
  return ftxui::vbox(std::move(screen));
}

}  // namespace tui
