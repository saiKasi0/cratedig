// The braille waveform renderer.
//
// Every expectation here is a literal glyph, because that is what the user
// actually sees. Checking dot masks would pass just as happily with the bit
// table transposed.
//
// The golden strings were produced by an independent reference implementation
// written from the header's documented rules rather than from waveform.cpp
// (kept in the design notes), so agreement between the two is evidence and not
// a restatement of the code.

#include "tui/waveform.hpp"

#include "ingest/peak_pyramid.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

using ingest::PeakBin;
using tui::waveform_rows;
using tui::WaveformGeometry;

// `count` cells of identical content, i.e. both dot columns the same -- which is
// the only case the design mockups exercise.
[[nodiscard]] std::vector<PeakBin> flat(float min, float max, std::size_t cells) {
  return std::vector<PeakBin>(cells * tui::kDotColumnsPerCell, PeakBin{min, max});
}

}  // namespace

TEST_CASE("the glyph set is exactly the one the design mockups use", "[unit]") {
  // docs/design/*.html draws waveforms with ⣿ ⣶ ⣤ ⣀ and their mirrors ⠉ ⠛ ⠿.
  // Reconstructing those from a 2x4 renderer is what established that the
  // mockups are a special case of real braille -- both dot columns filled
  // identically -- rather than a separate glyph vocabulary to imitate.
  const WaveformGeometry one_row{.rows = 1, .gain = 1.0F};

  CHECK(waveform_rows(flat(-1.0F, -1.0F, 1), one_row)[0] == "⣀");
  CHECK(waveform_rows(flat(-1.0F, 0.0F, 1), one_row)[0] == "⣤");
  CHECK(waveform_rows(flat(-1.0F, 0.5F, 1), one_row)[0] == "⣶");
  CHECK(waveform_rows(flat(-1.0F, 1.0F, 1), one_row)[0] == "⣿");
  CHECK(waveform_rows(flat(1.0F, 1.0F, 1), one_row)[0] == "⠉");
  CHECK(waveform_rows(flat(0.5F, 1.0F, 1), one_row)[0] == "⠛");
  CHECK(waveform_rows(flat(0.0F, 1.0F, 1), one_row)[0] == "⠿");
}

TEST_CASE("braille_glyph encodes U+2800 plus the dot mask", "[unit]") {
  CHECK(tui::braille_glyph(0xFF) == "⣿");
  CHECK(tui::braille_glyph(0xC0) == "⣀");
  CHECK(tui::braille_glyph(0x09) == "⠉");

  // Every braille cell is three UTF-8 bytes...
  CHECK(tui::braille_glyph(0x01).size() == 3);

  // ...except an empty one, which is a real space. U+2800 would also be blank,
  // but a space keeps the committed snapshots readable and sidesteps terminals
  // that render blank braille at zero width.
  CHECK(tui::braille_glyph(0x00) == " ");
}

TEST_CASE("the two dot columns are independent", "[unit]") {
  // This is what buys the waveform twice the horizontal resolution of the
  // character grid. A renderer that drew one value per character would produce
  // ⣿, ⠉ or ⣀ here -- never the asymmetric glyph.
  const std::vector<PeakBin> bins{PeakBin{-1.0F, -1.0F}, PeakBin{1.0F, 1.0F}};
  const std::vector<std::string> rows = waveform_rows(bins, WaveformGeometry{.rows = 1});

  REQUIRE(rows.size() == 1);
  CHECK(rows[0] == "⡈");  // left column at the bottom, right column at the top
}

TEST_CASE("positive amplitude is drawn upward", "[unit]") {
  // Screen coordinates grow downward and audio does not. An inverted waveform
  // is perfectly plausible-looking on a symmetrical signal, so it needs a
  // deliberately asymmetric one to catch.
  const WaveformGeometry five{.rows = 5, .gain = 1.0F};

  const std::vector<std::string> positive = waveform_rows(flat(0.5F, 0.5F, 2), five);
  REQUIRE(positive.size() == 5);
  CHECK(positive[1] == "⠒⠒");  // above the middle row
  CHECK(positive[3] == "  ");

  const std::vector<std::string> negative = waveform_rows(flat(-0.5F, -0.5F, 2), five);
  CHECK(negative[3] == "⠤⠤");  // below it
  CHECK(negative[1] == "  ");
}

TEST_CASE("silence draws the centre line, not an empty panel", "[unit]") {
  // "No file loaded" and "a silent file" must not look the same. They are
  // different situations and the interface has to be able to say which.
  const std::vector<std::string> rows =
      waveform_rows(flat(0.0F, 0.0F, 2), WaveformGeometry{.rows = 5});
  REQUIRE(rows.size() == 5);
  CHECK(rows[0] == "  ");
  CHECK(rows[1] == "  ");
  CHECK(rows[2] == "⠤⠤");
  CHECK(rows[3] == "  ");
  CHECK(rows[4] == "  ");
}

TEST_CASE("full scale fills every row and clips rather than wraps", "[unit]") {
  const WaveformGeometry three{.rows = 3, .gain = 1.0F};

  const std::vector<std::string> full = waveform_rows(flat(-1.0F, 1.0F, 2), three);
  CHECK(full == std::vector<std::string>{"⣿⣿", "⣿⣿", "⣿⣿"});

  // Four times gain on a half-scale signal is 2x full scale. It must saturate.
  // Wrapping here would draw a loud passage as a thin line through the middle,
  // which is the most misleading thing a meter can do.
  const std::vector<std::string> hot =
      waveform_rows(flat(-0.5F, 0.5F, 2), WaveformGeometry{.rows = 3, .gain = 4.0F});
  CHECK(hot == std::vector<std::string>{"⣿⣿", "⣿⣿", "⣿⣿"});
}

TEST_CASE("gain scales the excursion", "[unit]") {
  const std::vector<std::string> half =
      waveform_rows(flat(-1.0F, 1.0F, 2), WaveformGeometry{.rows = 3, .gain = 0.5F});
  CHECK(half == std::vector<std::string>{"⣀⣀", "⣿⣿", "⠉⠉"});
}

TEST_CASE("a sine renders as a sine", "[unit]") {
  // The characterisation test: a full cycle over eight character columns, three
  // rows tall. Committed as a golden because "the numbers are in range" would
  // pass for a great many pictures that are not a sine wave. Rising through the
  // centre on the left, peak, falling, trough, rising again -- readable as such
  // in the literal below, which is the point.
  //
  // Amplitude 0.9 and a quarter-step phase offset are not decoration. At unit
  // amplitude and zero phase, one bin edge lands exactly on sin(pi), where the
  // mapping sits on a rounding knife edge: pos comes out at exactly 5.5, and
  // whether that becomes dot row 5 or 6 depends on whether the subtraction
  // happened in float or double. The first draft of this golden disagreed with
  // its reference implementation over precisely that one cell. Offset like this,
  // no value comes within 0.07 dot rows of a boundary, so the golden means what
  // it looks like it means on any libm.
  constexpr std::size_t kBins = 16;
  constexpr double kTwoPi = 6.283185307179586;
  constexpr double kAmplitude = 0.9;
  constexpr double kPhaseSteps = 0.25;

  const auto sample_at = [](std::size_t step) {
    return static_cast<float>(
        kAmplitude *
        std::sin(kTwoPi * (static_cast<double>(step) + kPhaseSteps) / static_cast<double>(kBins)));
  };

  std::vector<PeakBin> bins;
  bins.reserve(kBins);
  for (std::size_t index = 0; index < kBins; ++index) {
    const float first = sample_at(index);
    const float second = sample_at(index + 1);
    bins.push_back(PeakBin{std::min(first, second), std::max(first, second)});
  }

  const std::vector<std::string> rows = waveform_rows(bins, WaveformGeometry{.rows = 3});
  CHECK(rows == std::vector<std::string>{
                    "⣠⠖⠲⡄    ",
                    "⠃  ⠹⡄  ⣰",
                    "    ⠙⠦⠴⠃",
                });
}

TEST_CASE("degenerate geometry produces nothing rather than misbehaving", "[unit]") {
  CHECK(waveform_rows(flat(-1.0F, 1.0F, 4), WaveformGeometry{.rows = 0}).empty());

  const std::vector<std::string> no_bins = waveform_rows({}, WaveformGeometry{.rows = 3});
  REQUIRE(no_bins.size() == 3);
  for (const std::string& row : no_bins) {
    CHECK(row.empty());
  }

  // An odd bin count cannot fill its last cell, so that half-cell is dropped
  // rather than half-drawn.
  const std::vector<PeakBin> odd(5, PeakBin{-1.0F, 1.0F});
  const std::vector<std::string> rows = waveform_rows(odd, WaveformGeometry{.rows = 1});
  REQUIRE(rows.size() == 1);
  CHECK(rows[0] == "⣿⣿");
}

TEST_CASE("envelope_rows fills from the baseline up", "[unit]") {
  // Not waveform_rows with one half blanked: an envelope is a gain over time and
  // has no negative half, so it is drawn as a bar chart rather than mirrored
  // about a centre line.
  const std::vector<float> full(4, 1.0F);
  const std::vector<std::string> filled = tui::envelope_rows(full, 2);
  REQUIRE(filled.size() == 2);
  // Every dot set, in both rows: "⣿⣿".
  CHECK(filled[0] == "⣿⣿");
  CHECK(filled[1] == "⣿⣿");

  const std::vector<float> silent(4, 0.0F);
  const std::vector<std::string> empty = tui::envelope_rows(silent, 2);
  REQUIRE(empty.size() == 2);
  // Spaces, not blank braille -- the same choice braille_glyph makes.
  CHECK(empty[0] == "  ");
  CHECK(empty[1] == "  ");
}

TEST_CASE("envelope_rows puts a half level in the bottom half", "[unit]") {
  // The direction is the whole point. Filling from the top would draw a decay as
  // a rise, which is a picture of the opposite of what the numbers say.
  const std::vector<float> half(2, 0.5F);
  const std::vector<std::string> rows = tui::envelope_rows(half, 2);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0] == " ");  // top row empty
  CHECK(rows[1] == "⣿");  // bottom row full
}

TEST_CASE("envelope_rows clamps rather than wrapping", "[unit]") {
  // A level above one is a bug upstream; drawing it as a low bar because the
  // dot count wrapped would hide that bug behind a plausible picture.
  const std::vector<float> over{4.0F, 4.0F};
  const std::vector<float> under{-1.0F, -1.0F};
  CHECK(tui::envelope_rows(over, 1)[0] == "⣿");
  CHECK(tui::envelope_rows(under, 1)[0] == " ");

  // Degenerate shapes are empty rather than a crash or a stripe.
  CHECK(tui::envelope_rows(over, 0).empty());
  CHECK(tui::envelope_rows(std::vector<float>{}, 3).empty());
}
