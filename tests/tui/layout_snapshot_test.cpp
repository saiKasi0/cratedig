// The interface, rendered offscreen and compared against committed goldens.
//
// These snapshots are the ground truth for what CRATEDIG's interface IS; the
// mockups in docs/design are what it aspires to (CLAUDE.md). A change here is a
// change to the product, and it should be visible as one in the diff.
//
// Determinism comes from render() being a pure function of a UiState literal:
// no engine, no device, no file, no clock is in scope, so there is nothing to
// suppress. The one piece of global state that would leak in -- the terminal's
// detected colour support -- is pinned below, because whether the ANSI output
// says "38;2;255;79;0" or "38;5;202" otherwise depends on which terminal
// happened to launch the test.

#include "ingest/peak_pyramid.hpp"
#include "rt/sample.hpp"
#include "tui/render.hpp"
#include "tui/theme.hpp"
#include "tui/ui_state.hpp"
#include "tui/waveform.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

// Set CRATEDIG_UPDATE_SNAPSHOTS=1 (scripts/update_tui_snapshots.sh) to rewrite
// the goldens instead of comparing. CLAUDE.md requires looking at the rendered
// output in a real terminal before doing that.
[[nodiscard]] bool updating() {
  // Single-threaded test setup, and there is no thread-safe standard way to
  // read an environment variable.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char* const value = std::getenv("CRATEDIG_UPDATE_SNAPSHOTS");
  return value != nullptr && *value != '\0' && *value != '0';
}

// The escape codes carry the colour roles, which are asserted separately and
// deliberately below. Keeping them out of the goldens is what makes a layout
// diff readable -- and an unreadable diff is one that gets accepted without
// being read.
[[nodiscard]] std::string strip_ansi(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t index = 0; index < text.size();) {
    if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
      index += 2;
      while (index < text.size() &&
             (std::isdigit(static_cast<unsigned char>(text[index])) != 0 || text[index] == ';')) {
        ++index;
      }
      if (index < text.size()) {
        ++index;  // the final letter
      }
      continue;
    }
    out += text[index];
    ++index;
  }
  return out;
}

[[nodiscard]] ftxui::Screen render_screen(const tui::UiState& state, int columns, int rows) {
  ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
  ftxui::Screen screen =
      ftxui::Screen::Create(ftxui::Dimension::Fixed(columns), ftxui::Dimension::Fixed(rows));
  ftxui::Element document =
      tui::render(state, static_cast<std::size_t>(columns), static_cast<std::size_t>(rows));
  ftxui::Render(screen, document);
  return screen;
}

void check_snapshot(const std::string& name, const tui::UiState& state, int columns, int rows) {
  const ftxui::Screen screen = render_screen(state, columns, rows);
  const std::string actual = strip_ansi(screen.ToString());

  const std::filesystem::path path =
      std::filesystem::path{CRATEDIG_TUI_SNAPSHOT_DIR} / (name + ".txt");

  if (updating()) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    REQUIRE(out.good());
    out << actual;
    SUCCEED("wrote " << path.string());
    return;
  }

  INFO("snapshot: " << path.string()
                    << "\nregenerate with scripts/update_tui_snapshots.sh after looking at the "
                       "rendered output in a real terminal\n--- actual ---\n"
                    << actual);
  std::ifstream in{path, std::ios::binary};
  REQUIRE(in.good());
  std::ostringstream buffer;
  buffer << in.rdbuf();
  CHECK(actual == buffer.str());
}

// -- fixtures ----------------------------------------------------------------
//
// One synthetic sample, run through the real PeakPyramid, so the snapshots
// exercise the actual summarise-and-draw path rather than hand-written bins.

constexpr std::uint32_t kFixtureRate = 44'100;
constexpr std::size_t kFixtureFrames = 92 * kFixtureRate / 10;  // 9.2 s

[[nodiscard]] const rt::Sample& fixture_sample() {
  static const rt::Sample kSample = [] {
    rt::Sample built{kFixtureRate, 2, kFixtureFrames};
    for (std::uint16_t channel = 0; channel < 2; ++channel) {
      const std::span<float> data = built.mutable_channel(channel);
      for (std::size_t frame = 0; frame < kFixtureFrames; ++frame) {
        const auto position = static_cast<double>(frame);
        // A repeating percussive envelope on a low tone: something that looks
        // like a loop rather than like a test signal, so the snapshots read as
        // a picture of audio.
        const double beat = std::fmod(position / kFixtureRate, 0.5);
        const double envelope = std::exp(-beat * 9.0);
        const double tone = std::sin(position * (0.0121 + (0.004 * channel)));
        data[frame] = static_cast<float>(envelope * tone * 0.92);
      }
    }
    return built;
  }();
  return kSample;
}

[[nodiscard]] const ingest::PeakPyramid& fixture_pyramid() {
  static const ingest::PeakPyramid kPyramid = ingest::PeakPyramid::build(fixture_sample());
  return kPyramid;
}

void fill_bins(tui::UiState& state, int columns) {
  const std::size_t wave_columns = tui::wave_columns_for(static_cast<std::size_t>(columns));
  state.bins.assign(tui::bins_for_columns(wave_columns), ingest::PeakBin{});
  fixture_pyramid().summarize(fixture_sample(), 0, state.view.first_frame,
                              state.view.frames_visible, state.bins);
}

[[nodiscard]] tui::UiState empty_state() {
  tui::UiState state;
  state.version = "0.0.1";
  state.engine_rate = 48'000;
  state.block_frames = 256;
  state.audio_api = "CoreAudio";
  state.max_voices = 16;
  return state;
}

[[nodiscard]] tui::UiState loaded_state(int columns) {
  tui::UiState state = empty_state();
  state.sample_name = "long_form_drums.flac";
  state.sample_rate = kFixtureRate;
  state.sample_channels = 2;
  state.sample_frames = kFixtureFrames;
  state.view.fit(kFixtureFrames);

  state.pads[0] = tui::PadState{.name = "drums", .level = 0.0F, .loaded = true};
  state.pads[1] = tui::PadState{.name = "kick", .level = 0.0F, .loaded = true};
  state.pads[4] = tui::PadState{.name = "chop1", .level = 0.0F, .loaded = true};

  fill_bins(state, columns);
  return state;
}

[[nodiscard]] tui::UiState playing_state(int columns) {
  tui::UiState state = loaded_state(columns);
  state.playing = true;
  state.playhead_frame = kFixtureFrames / 3;
  state.playhead_pad = 0;
  state.active_voices = 2;
  state.master_peak = 0.71F;
  state.pads[0].level = 0.82F;
  state.pads[1].level = 0.31F;
  state.selected_pad = 0;
  return state;
}

// A chopped state: sixteen slices across the fixture, assigned to pads, with
// three pads glowing at different ages so the whole ramp appears in one frame.
[[nodiscard]] tui::UiState chopped_state(int columns) {
  tui::UiState state = playing_state(columns);
  state.chop_algorithm = "transient";

  constexpr std::size_t kSlices = 16;
  for (std::size_t index = 0; index < kSlices; ++index) {
    const std::size_t start = (kFixtureFrames * index) / kSlices;
    const std::size_t end = (kFixtureFrames * (index + 1)) / kSlices;
    state.slices.push_back(tui::SliceMark{.start_frame = start, .end_frame = end});

    // "s07" rather than "chop7". A pad cell has five columns for a name, so
    // "chop10".."chop16" all truncate to "chop1" and the grid stops
    // distinguishing them -- which is exactly the sort of thing a snapshot is
    // for noticing. Short and zero-padded, so every cell is the same width and
    // the column of names lines up.
    const std::string number =
        index < 9 ? "0" + std::to_string(index + 1) : std::to_string(index + 1);
    state.pads[index] = tui::PadState{.name = "s" + number,
                                      .level = 0.0F,
                                      .loaded = true,
                                      .has_slice = true,
                                      .slice_index = index};
  }

  // One pad at each step of the glow ramp, plus one that was hit long enough ago
  // to be dark again. Without all four in one snapshot, a change to the ramp
  // could alter three of them invisibly.
  state.pads[0].triggered = true;
  state.pads[0].glow_seconds = 0.01F;
  state.pads[0].glow_velocity = 1.0F;  // hot
  state.pads[5].triggered = true;
  state.pads[5].glow_seconds = 0.15F;
  state.pads[5].glow_velocity = 1.0F;  // lit
  state.pads[10].triggered = true;
  state.pads[10].glow_seconds = 0.30F;
  state.pads[10].glow_velocity = 1.0F;  // dim
  state.pads[15].triggered = true;
  state.pads[15].glow_seconds = 2.00F;
  state.pads[15].glow_velocity = 1.0F;  // long over

  state.pads[0].level = 0.82F;
  state.pads[5].level = 0.31F;
  return state;
}

}  // namespace

TEST_CASE("PERFORM renders at the 100x30 design grid", "[tui]") {
  SECTION("nothing loaded") {
    check_snapshot("perform_empty_100x30", empty_state(), 100, 30);
  }
  SECTION("loaded, idle") {
    check_snapshot("perform_loaded_100x30", loaded_state(100), 100, 30);
  }
  SECTION("playing") {
    check_snapshot("perform_playing_100x30", playing_state(100), 100, 30);
  }

  SECTION("zoomed in") {
    tui::UiState state = playing_state(100);
    state.view.first_frame = kFixtureFrames / 3;
    state.view.frames_visible = kFixtureRate / 8;  // 125 ms
    state.view.clamp(kFixtureFrames);
    // Non-zero fault counters, which only appear in the mode line when they have
    // something to report -- this is the snapshot that covers that path.
    state.xruns = 3;
    state.dropped = 1;
    fill_bins(state, 100);
    check_snapshot("perform_zoomed_100x30", state, 100, 30);
  }

  SECTION("pattern tab") {
    tui::UiState state = playing_state(100);
    state.tab = tui::PanelTab::kPattern;
    check_snapshot("perform_pattern_100x30", state, 100, 30);
  }

  SECTION("chopped, with pads glowing") {
    check_snapshot("perform_chopped_100x30", chopped_state(100), 100, 30);
  }

  SECTION("chopped and zoomed, so most boundaries are off screen") {
    // The other half of the slice ruler: when only a few boundaries fall inside
    // the view, only those get a tick -- and the numbers stay the SLICE numbers
    // rather than being renumbered from what happens to be visible.
    tui::UiState state = chopped_state(100);
    state.view.first_frame = kFixtureFrames / 2;
    state.view.frames_visible = kFixtureFrames / 8;
    state.view.clamp(kFixtureFrames);
    fill_bins(state, 100);
    check_snapshot("perform_chopped_zoom_100x30", state, 100, 30);
  }
}

TEST_CASE("slice markers replace the time ruler only when there are slices", "[tui]") {
  // Two states differing only in whether anything has been chopped. Before, the
  // row pair is elapsed time; after, it is numbered boundaries.
  const std::string unchopped = strip_ansi(render_screen(playing_state(100), 100, 30).ToString());
  const std::string chopped = strip_ansi(render_screen(chopped_state(100), 100, 30).ToString());

  CHECK(unchopped != chopped);
  CHECK(unchopped.find("┬") != std::string::npos);  // the time ruler's own ticks
  CHECK(chopped.find("┬") != std::string::npos);    // ...and the slice ruler's

  // The give-away is the numbering: slice numbers are 01..16, times are not.
  CHECK(chopped.find("01") != std::string::npos);
  CHECK(chopped.find("16") != std::string::npos);
}

TEST_CASE("a pad's glow fades and then goes out", "[tui]") {
  // The ramp, asserted on the rendered cells rather than by reading the layout:
  // the rule is about what reaches the screen.
  //
  // Counted as ACCENT CELLS on the pad row, because the ramp is built from
  // intensity and weight rather than from four different colours -- in sixteen
  // colours there is no glow, and a ramp made of colours would simply not exist
  // on a 16-colour console.
  const auto lit_pads = [](float age) {
    tui::UiState state = chopped_state(100);
    for (tui::PadState& pad : state.pads) {
      pad.triggered = false;
      pad.level = 0.0F;
    }
    state.playing = false;
    state.pads[0].triggered = true;
    state.pads[0].glow_seconds = age;
    state.pads[0].glow_velocity = 1.0F;

    const ftxui::Screen screen = render_screen(state, 100, 30);
    std::size_t accent = 0;
    for (int y = 14; y < 27; ++y) {
      for (int x = 0; x < 46; ++x) {
        if (screen.CellAt(x, y).foreground_color == tui::theme::accent() ||
            screen.CellAt(x, y).inverted) {
          ++accent;
        }
      }
    }
    return accent;
  };

  const std::size_t fresh = lit_pads(0.01F);
  const std::size_t old = lit_pads(0.30F);
  const std::size_t gone = lit_pads(1.00F);

  INFO("accent/inverted cells in the pad grid: " << fresh << " fresh, " << old << " fading, "
                                                 << gone << " past the fade");
  CHECK(fresh > 0);
  CHECK(gone == 0);
  CHECK(old > 0);
}

TEST_CASE("a soft hit lights a pad less than a hard one", "[tui]") {
  // Velocity scales the glow, so the information the player put in comes back
  // out. Without it every hit looks identical and the grid stops telling you
  // anything about how you played.
  tui::PadState hard{};
  hard.triggered = true;
  hard.glow_seconds = 0.0F;
  hard.glow_velocity = 1.0F;

  tui::PadState soft = hard;
  soft.glow_velocity = 0.2F;

  CHECK(tui::glow_intensity(hard) > tui::glow_intensity(soft));
  CHECK(tui::glow_intensity(soft) > 0.0F);

  tui::PadState never{};
  CHECK(tui::glow_intensity(never) == 0.0F);
}

TEST_CASE("PERFORM degrades on smaller and larger terminals", "[tui]") {
  SECTION("80x24, the classic minimum") {
    check_snapshot("perform_playing_80x24", playing_state(80), 80, 24);
  }
  SECTION("120x40") {
    check_snapshot("perform_playing_120x40", playing_state(120), 120, 40);
  }
  SECTION("60x20, the smallest supported") {
    check_snapshot("perform_playing_60x20", playing_state(60), 60, 20);
  }
  SECTION("below the minimum, a message rather than a mess") {
    check_snapshot("too_small_40x12", playing_state(40), 40, 12);
  }
}

TEST_CASE("the accent colour is spent on exactly one thing", "[tui]") {
  // DESIGN_BRIEF: "the orange used sparingly -- one glowing element per screen".
  // Asserted on the rendered cells rather than by reading the layout code,
  // because the rule is about what reaches the screen.
  const ftxui::Screen screen = render_screen(playing_state(100), 100, 30);

  std::size_t accent_cells = 0;
  std::size_t accent_columns_in_wave = 0;
  for (int y = 0; y < screen.dimy(); ++y) {
    for (int x = 0; x < screen.dimx(); ++x) {
      if (screen.CellAt(x, y).foreground_color == tui::theme::accent()) {
        ++accent_cells;
        if (y >= 2 && y <= 11) {
          ++accent_columns_in_wave;
        }
      }
    }
  }
  INFO("accent cells: " << accent_cells
                        << ", of which in the wave panel: " << accent_columns_in_wave);
  CHECK(accent_cells > 0);
  // The playhead column plus the pad meters that are actually lit. Well short of
  // "the orange is everywhere", which is the failure this guards against.
  CHECK(accent_cells < 40);
  CHECK(accent_columns_in_wave > 0);
}

TEST_CASE("colour roles land where they are supposed to", "[tui]") {
  const ftxui::Screen screen = render_screen(playing_state(100), 100, 30);

  // The product name is the brightest thing in the header.
  CHECK(screen.CellAt(1, 0).foreground_color == tui::theme::bright());
  CHECK(screen.CellAt(1, 0).bold);

  // Panel borders are structure, never content colour: the top-left corner of
  // the wave panel.
  CHECK(screen.CellAt(0, 2).character == "╭");
  CHECK(screen.CellAt(0, 2).foreground_color == tui::theme::structure());
}

TEST_CASE("an empty interface is distinguishable from a silent one", "[tui]") {
  // A blank wave panel and a panel showing a silent file must not render
  // identically -- they are different situations and the interface has to say
  // which. The braille renderer draws a centre line for silence; nothing loaded
  // says so in words.
  const std::string empty = strip_ansi(render_screen(empty_state(), 100, 30).ToString());

  tui::UiState silent = loaded_state(100);
  std::fill(silent.bins.begin(), silent.bins.end(), ingest::PeakBin{});
  const std::string quiet = strip_ansi(render_screen(silent, 100, 30).ToString());

  CHECK(empty != quiet);
  CHECK(empty.find("no sample loaded") != std::string::npos);
  CHECK(quiet.find("no sample loaded") == std::string::npos);
  CHECK(quiet.find("⠤") != std::string::npos);  // the centre line
}
