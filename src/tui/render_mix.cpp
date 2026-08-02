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
#include <vector>

// MIX: the strips, the buses and the master.
//
// A third pure function of the same UiState, beside render() and render_edit().
//
// WHERE THIS DELIBERATELY DIFFERS FROM THE MOCKUP, both recorded in
// docs/design/README.md:
//
// 1. THE STRIP ROW IS A VIEWPORT. The mockup shows eight channels and master,
//    filling all hundred columns exactly, and draws no bus strips. Sixteen
//    channels plus four buses plus master is twenty-one strips, which at ten
//    columns each is two hundred and ten. It does not fit. So the row pages --
//    channels 1-8, channels 9-16, buses A-D -- with master pinned right on every
//    page, because master is the strip you always want to see.
//
// 2. THE TWO INSERT SLOTS ARE FIXED ROWS. The mockup draws free-form slots
//    ("a comp", "b eq", "a de-s") suggesting arbitrary processors in arbitrary
//    order. The chain is fixed at EQ then compressor (docs/MIXER.md), so these
//    rows name what is actually there. Arbitrary processors is M8's plugin
//    chain, which needs a swap protocol this does not.

namespace tui {
namespace {

using ftxui::Element;
using ftxui::Elements;

using detail::format_dbfs;
using detail::paint_at;
using detail::utf8_cells;
using detail::with_precision;

// A channel strip: eight columns inside a one-column border on each side.
// Master is wider, because it carries two meters rather than one.
constexpr std::size_t kStripInner = 8;
constexpr std::size_t kStripOuter = kStripInner + 2;
constexpr std::size_t kMasterInner = 10;
constexpr std::size_t kMasterOuter = kMasterInner + 2;

// One space between panels, matching the mockup.
constexpr std::size_t kPanelGap = 1;

// How many channel strips fit beside master at a given width:
//   n * kStripOuter + (n - 1) * gap + gap + kMasterOuter <= columns
// which at the 100-column design grid gives exactly the mockup's eight.
constexpr std::size_t kMaxVisibleStrips = 8;

[[nodiscard]] std::size_t visible_strips_for(std::size_t columns) {
  if (columns < kMasterOuter + kPanelGap + kStripOuter) {
    return 1;
  }
  const std::size_t room = columns - kMasterOuter - kPanelGap;
  const std::size_t count = (room + kPanelGap) / (kStripOuter + kPanelGap);
  return std::clamp(count, std::size_t{1}, kMaxVisibleStrips);
}

// The fader travel, in rows. Ten, matching the mockup, and the same number the
// meter beside it uses so the two read against each other.
constexpr std::size_t kFaderRows = 10;

// The fader's range in dB. Zero sits nine-tenths of the way up rather than at
// the top, so there is somewhere to go: rt::kMaxStripGain is +12 dB.
constexpr float kFaderTopDb = 12.0F;
constexpr float kFaderBottomDb = -60.0F;

// The EQ curve's vertical range, and it is NOT the band gain limit.
//
// Two braille rows is eight dot rows. Over rt::kMaxEqGainDb's +-24 dB that is
// 6 dB per dot, and a typical 3-6 dB move does not shift a single dot -- a
// display that shows nothing for the edits people actually make. At +-12 dB it
// is 3 dB per dot and those moves are visible. Bands beyond +-12 dB saturate the
// display, which is the honest trade: the curve says "hard boost here" and the
// chain row below it says how much.
constexpr float kCurveRangeDb = 12.0F;

// Two braille rows, four dot rows each.
constexpr std::size_t kCurveRows = 2;
constexpr std::size_t kCurveDotRows = kCurveRows * 4;

[[nodiscard]] std::string pad_to(std::string text, std::size_t cells) {
  while (utf8_cells(text) < cells) {
    text.push_back(' ');
  }
  return text;
}

// Centres within `cells`, biased left when the remainder is odd -- so a readout
// that grows a minus sign does not jump sideways.
[[nodiscard]] std::string centre(std::string_view text, std::size_t cells) {
  const std::size_t width = utf8_cells(text);
  if (width >= cells) {
    return std::string{text};
  }
  const std::size_t left = (cells - width) / 2;
  return pad_to(std::string(left, ' ') + std::string{text}, cells);
}

// Where the fader cap sits, 0 at the top of its travel.
[[nodiscard]] std::size_t fader_row_for(float gain_db) {
  if (!std::isfinite(gain_db)) {
    return kFaderRows - 1;
  }
  const float clamped = std::clamp(gain_db, kFaderBottomDb, kFaderTopDb);
  const float fraction = (kFaderTopDb - clamped) / (kFaderTopDb - kFaderBottomDb);
  const auto row = static_cast<std::size_t>(fraction * static_cast<float>(kFaderRows - 1));
  return std::min(row, kFaderRows - 1);
}

// How many of the meter's rows are lit, from the bottom.
//
// Scaled in dB rather than linearly: a linear meter spends nine of its ten rows
// on the top 20 dB and shows nothing at all for anything quiet, which on a
// ten-row meter means it reads empty most of the time.
[[nodiscard]] std::size_t meter_rows_for(float peak) {
  if (!(peak > 0.0F)) {
    return 0;
  }
  const float decibels = 20.0F * std::log10(peak);
  if (decibels <= kFaderBottomDb) {
    return 0;
  }
  const float fraction = (decibels - kFaderBottomDb) / (0.0F - kFaderBottomDb);  // 0 dBFS fills it
  const auto rows = static_cast<std::size_t>(std::ceil(fraction * static_cast<float>(kFaderRows)));
  return std::min(rows, kFaderRows);
}

// The braille EQ curve, as kCurveRows strings of `cells` characters.
//
// Built on waveform.hpp's braille_glyph() rather than on a second braille
// implementation: that primitive is deliberately independent of the PeakBin
// model, so a new small renderer over it is the intended way to draw a second
// kind of curve.
[[nodiscard]] std::vector<std::string> curve_rows(const StripView& strip, std::size_t cells) {
  std::vector<std::string> rows(kCurveRows);

  // Bypassed, or nothing published: a flat line at 0 dB. Not an empty panel --
  // "no EQ" and "an EQ that happens to be flat" look the same on the curve and
  // are told apart by the chain row below it.
  const std::size_t dot_columns = cells * kDotColumnsPerCell;
  std::vector<float> curve(dot_columns, 0.0F);
  for (std::size_t column = 0; column < dot_columns && !strip.eq_curve.empty(); ++column) {
    // Nearest sample rather than interpolated: the caller is asked for exactly
    // this many, and resampling here would hide a caller that produced the wrong
    // number.
    const std::size_t source =
        std::min(column * strip.eq_curve.size() / dot_columns, strip.eq_curve.size() - 1);
    curve[column] = strip.eq_curve[source];
  }

  for (std::size_t row = 0; row < kCurveRows; ++row) {
    std::string line;
    for (std::size_t cell = 0; cell < cells; ++cell) {
      std::uint8_t dots = 0;
      for (std::size_t half = 0; half < kDotColumnsPerCell; ++half) {
        const std::size_t column = (cell * kDotColumnsPerCell) + half;
        const float decibels = std::clamp(curve[column], -kCurveRangeDb, kCurveRangeDb);

        // 0 dB is the middle of the two rows; positive gain rises.
        const float fraction = (kCurveRangeDb - decibels) / (2.0F * kCurveRangeDb);
        auto dot_row = static_cast<std::size_t>(fraction * static_cast<float>(kCurveDotRows - 1));
        dot_row = std::min(dot_row, kCurveDotRows - 1);
        if (dot_row / 4 != row) {
          continue;
        }
        dots = static_cast<std::uint8_t>(dots | kBrailleDotBits[dot_row % 4][half]);
      }
      line.append(braille_glyph(dots));
    }
    rows[row] = line;
  }
  return rows;
}

// Balance as text rather than as an eight-cell graphic.
//
// Eight columns is eight positions of resolution, which cannot tell -20 from
// -25; "L20" can. Centre is "C" and not "0", because a balance at centre is a
// state rather than a number.
[[nodiscard]] std::string balance_text(float balance) {
  if (!std::isfinite(balance) || std::abs(balance) < 0.005F) {
    return "C";
  }
  const auto percent = static_cast<int>(std::lround(std::abs(balance) * 100.0F));
  return (balance < 0.0F ? "L" : "R") + std::to_string(percent);
}

// One strip, as the rows inside its border.
//
// `kind` decides which rows are meaningful. A bus and the master have no EQ and
// no compressor in M5, so they get blank rows rather than four dots and two
// "--"s claiming a chain they do not have. Master's chain row says what it DOES
// have, which is the limiter.
enum class StripKind : std::uint8_t { kChannel, kBus, kMaster };

[[nodiscard]] std::vector<std::string> strip_rows(const StripView& strip, std::size_t inner,
                                                  StripKind kind, bool limiter_enabled) {
  std::vector<std::string> rows;
  const bool is_channel = kind == StripKind::kChannel;
  const bool stereo_meter = kind == StripKind::kMaster;

  if (is_channel) {
    const std::vector<std::string> curve = curve_rows(strip, inner);
    rows.insert(rows.end(), curve.begin(), curve.end());

    // The band markers under the curve: where the four bands sit, left to right.
    std::string markers(inner, ' ');
    for (std::size_t band = 0; band < 4; ++band) {
      const std::size_t column = ((band * 2) + 1) * inner / 8;
      markers = paint_at(markers, std::min(column, inner - 1), "·");
    }
    rows.push_back(markers);
  } else if (kind == StripKind::kBus) {
    // A bus has no EQ in M5, so the curve's rows go to the one fact a bus does
    // have that master does not: how many channels are feeding it.
    rows.emplace_back(inner, ' ');
    rows.push_back(pad_to(std::to_string(strip.routed) + " ch in", inner));
    rows.emplace_back(inner, ' ');
  } else {
    for (std::size_t row = 0; row < kCurveRows + 1; ++row) {
      rows.emplace_back(inner, ' ');
    }
  }

  if (is_channel) {
    const auto letter = static_cast<char>('a' + std::min<std::uint8_t>(strip.bus, 3));
    rows.push_back(pad_to(std::string{"bus "} + letter, inner));
    rows.push_back(pad_to("bal " + balance_text(strip.balance), inner));
  } else {
    rows.emplace_back(inner, ' ');
    rows.emplace_back(inner, ' ');
  }
  rows.emplace_back(inner, ' ');

  // THE FIXED CHAIN, named. Not two free-form slots -- and named per node, so a
  // bus does not advertise an EQ it has not got.
  if (is_channel) {
    rows.push_back(pad_to("eq  " + strip.eq_label, inner));
    rows.push_back(pad_to("cmp " + strip.comp_label, inner));
  } else if (kind == StripKind::kMaster) {
    rows.push_back(pad_to(std::string{"lim "} + (limiter_enabled ? "on" : "off"), inner));
    rows.emplace_back(inner, ' ');
  } else {
    rows.emplace_back(inner, ' ');
    rows.emplace_back(inner, ' ');
  }
  rows.emplace_back(inner, ' ');

  // Fader and meter. The meter is two columns on a channel and two pairs on
  // master; the fader is a track with a cap on it.
  const std::size_t lit = meter_rows_for(strip.peak);
  const std::size_t cap = fader_row_for(strip.gain_db);

  // Gain reduction reads DOWNWARD from the top, because that is the direction
  // it acts -- the one thing on the strip that does.
  const std::size_t reduced =
      strip.reduction >= 1.0F
          ? 0
          : std::min(meter_rows_for(1.0F) - meter_rows_for(strip.reduction), kFaderRows);

  const std::size_t fader_column = stereo_meter ? 7 : 5;
  for (std::size_t row = 0; row < kFaderRows; ++row) {
    std::string line(inner, ' ');
    const std::size_t from_bottom = kFaderRows - row;
    if (lit >= from_bottom) {
      line = paint_at(line, 0, "██");
      if (stereo_meter) {
        line = paint_at(line, 3, "██");
      }
    }
    if (reduced > row) {
      line = paint_at(line, stereo_meter ? 5 : 2, "▔");
    }
    line = paint_at(line, fader_column, row == cap ? "━" : "┃");
    rows.push_back(line);
  }

  // The readout, with negative zero normalised away: a fader sitting exactly at
  // unity should say "0.0" whichever side it arrived from, and "-0.0" reads as a
  // bug in the mixer rather than as a property of floats.
  const double shown = strip.gain_db == 0.0F ? 0.0 : static_cast<double>(strip.gain_db);
  rows.push_back(centre(with_precision(shown, 1), inner));

  // Mute and solo, upper case when engaged so the state reads without colour.
  // Master has neither, and says so with blanks rather than with controls that
  // do nothing.
  // Only channels have them: the engine's mute and solo live in
  // rt::StripConfig, which is a per-pad thing. Drawing them on a bus would be
  // drawing a control that goes nowhere.
  std::string toggles(inner, ' ');
  if (kind == StripKind::kChannel) {
    toggles = paint_at(toggles, 1, strip.mute ? "M" : "m");
    toggles = paint_at(toggles, inner - 2, strip.solo ? "S" : "s");
  }
  rows.push_back(toggles);

  return rows;
}

[[nodiscard]] Element strip_panel(const StripView& strip, const std::string& title,
                                  std::size_t inner, StripKind kind, bool selected,
                                  bool limiter_enabled) {
  Elements lines;
  for (const std::string& row : strip_rows(strip, inner, kind, limiter_enabled)) {
    lines.push_back(ftxui::text(row));
  }

  Element body = ftxui::vbox(std::move(lines));
  Element head = ftxui::text(pad_to(title, inner));
  head = selected ? head | ftxui::color(theme::accent()) | ftxui::bold
                  : head | ftxui::color(theme::text());

  Element panel = ftxui::vbox({head, ftxui::separator(), std::move(body)}) | ftxui::border;
  panel = panel | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, static_cast<int>(inner + 2));
  return selected ? panel | ftxui::color(theme::accent())
                  : panel | ftxui::color(theme::structure());
}

[[nodiscard]] std::string page_name(MixPage page) {
  switch (page) {
    case MixPage::kChannelsLow:
      return "ch 1-8";
    case MixPage::kChannelsHigh:
      return "ch 9-16";
    case MixPage::kBuses:
      return "bus a-d";
  }
  return "ch 1-8";
}

// The strips this page shows, with the titles to put on them.
struct Page {
  std::vector<const StripView*> strips;
  std::vector<std::string> titles;
};

[[nodiscard]] Page page_for(const MixState& mix) {
  Page page;
  if (mix.page == MixPage::kBuses) {
    for (std::size_t bus = 0; bus < rt::kNumBuses; ++bus) {
      page.strips.push_back(&mix.buses[bus]);
      const auto letter = static_cast<char>('a' + bus);
      page.titles.push_back(std::string{"bus "} + letter);
    }
    return page;
  }

  const std::size_t first = mix.page == MixPage::kChannelsHigh ? 8 : 0;
  for (std::size_t offset = 0; offset < 8 && first + offset < rt::kNumPads; ++offset) {
    const std::size_t pad = first + offset;
    page.strips.push_back(&mix.strips[pad]);
    std::string title = pad < 9 ? "0" : "";
    title += std::to_string(pad + 1);
    title += " " + mix.strips[pad].name;
    page.titles.push_back(title);
  }
  return page;
}

}  // namespace

Element render_mix(const UiState& state, std::size_t terminal_columns, std::size_t terminal_rows) {
  const MixState& mix = state.mix;
  const Page page = page_for(mix);
  const std::size_t visible = std::min(visible_strips_for(terminal_columns), page.strips.size());

  Elements panels;
  for (std::size_t index = 0; index < visible; ++index) {  // NOLINT(modernize-loop-convert)
    panels.push_back(
        strip_panel(*page.strips[index], page.titles[index], kStripInner,
                    mix.page == MixPage::kBuses ? StripKind::kBus : StripKind::kChannel,
                    index == mix.cursor, mix.limiter_enabled));
    panels.push_back(ftxui::text(" "));
  }
  panels.push_back(strip_panel(mix.master, "master", kMasterInner, StripKind::kMaster, false,
                               mix.limiter_enabled));

  Element header = ftxui::hbox({
      ftxui::text("cratedig ") | ftxui::color(theme::bright()),
      ftxui::text(state.version + "  ") | ftxui::color(theme::muted()),
      ftxui::text("mix") | ftxui::color(theme::label()),
      ftxui::filler(),
      ftxui::text(
          page_name(mix.page) + " · " + std::to_string(visible) + " of " +
          std::to_string(mix.page == MixPage::kBuses ? rt::kNumBuses : std::size_t{rt::kNumPads}) +
          (mix.page == MixPage::kBuses ? " bus" : " ch") +
          (mix.any_solo ? std::string{" · solo"} : std::string{})) |
          ftxui::color(theme::muted()),
  });

  // The facts, in priority order -- dropped from the end when the line is short.
  //
  // The PAGE is deliberately not among them. It was, and it cost the middle hint
  // tier: the header already says "ch 1-8 · 8 of 16 ch" two lines up, so the
  // mode line was spending six cells repeating it and at 72 columns that was
  // exactly enough to push the keymap down to "tab page".
  std::vector<std::string> facts;
  facts.push_back("master " + format_dbfs(state.master_peak));
  facts.push_back(mix.limiter_enabled ? "lim " + format_dbfs(mix.limiter_gain) : "lim off");

  static constexpr std::array<std::string_view, 3> kHints{
      "[] page · hjkl strip/fader · m mute · s solo · b bus",
      "[] page · hjkl · m/s mute solo",
      "[] page",
  };

  // Room for both facts -- "master -0.6 · lim -2.9" is 22 cells -- so the
  // limiter readout does not vanish the moment the terminal narrows. MEASURED
  // at 60, 72, 84 and 100 columns rather than reasoned about, because M4.5
  // proved that arithmetic done in the head puts a fact on the grid at one
  // tempo and off it at another.
  constexpr std::size_t kMinFactCells = 22;

  Element line =
      detail::mode_line(state, terminal_columns, "  mix       ", facts, kHints, kMinFactCells);

  static_cast<void>(terminal_rows);
  return ftxui::vbox({
      header,
      ftxui::text(""),
      ftxui::hbox(std::move(panels)),
      ftxui::filler(),
      line,
  });
}

}  // namespace tui
