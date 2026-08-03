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
#include "ingest/slices.hpp"
#include "rt/sample.hpp"
#include "rt/strip.hpp"
#include "tui/completion.hpp"
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
#include <utility>
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
// A pattern with a recognisable shape: four-on-the-floor on pad 1, offbeat on
// pad 2, sixteenths on pad 4, and one hit on pad 12 so the right-hand column is
// not uniformly empty.
[[nodiscard]] tui::PatternView live_pattern() {
  tui::PatternView pattern;
  pattern.has_pattern = true;
  pattern.pattern = 2;
  pattern.length = 16;
  pattern.bpm_x100 = 9'260;
  pattern.transport_running = true;
  pattern.playhead = true;
  pattern.step = 6;
  for (std::size_t step = 0; step < 16; step += 4) {
    pattern.rows[0].on[step] = true;
  }
  for (std::size_t step = 4; step < 16; step += 8) {
    pattern.rows[1].on[step] = true;
  }
  for (std::size_t step = 0; step < 16; step += 2) {
    pattern.rows[3].on[step] = true;
  }
  pattern.rows[11].on[9] = true;
  return pattern;
}

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

// EDIT on slice 6, framed the way the app frames it: the slice plus a third of
// its length of context on each side, so both boundaries have something on the
// far side of them to be judged against.
[[nodiscard]] tui::UiState edit_state(int columns) {
  tui::UiState state = chopped_state(columns);
  state.screen = tui::Screen::kEdit;
  state.edit.slice = 5;

  // A snap that moved one boundary and not the other, so both readouts appear
  // in the same frame. The signs are opposite on purpose: a formatter that
  // dropped the minus would still look right on one of them.
  state.slices[5].start_snap = -3;
  state.slices[5].end_snap = 0;

  const tui::SliceMark& slice = state.slices[5];
  const std::size_t length = slice.end_frame - slice.start_frame;
  const std::size_t margin = length / 3;
  state.edit.view.first_frame = slice.start_frame - margin;
  state.edit.view.frames_visible = length + (2 * margin);
  state.edit.view.clamp(kFixtureFrames);

  state.edit.envelope = tui::EnvelopeView{.attack_ms = 1.2F,
                                          .decay_ms = 84.0F,
                                          .sustain = 0.5F,
                                          .release_ms = 120.0F,
                                          .gate = true,
                                          .choke_group = 1,
                                          .gain = 1.0F,
                                          .pitch_ratio = 1.0F};
  state.edit.undo_depth = 4;

  // The real crossings of the real fixture, so the ruler is a picture of the
  // audio above it rather than a decorative row of ticks.
  // The same density rule the app uses: past one tick per two columns the row
  // stops being a ruler and goes blank.
  const std::size_t wave_columns = tui::edit_wave_columns_for(static_cast<std::size_t>(columns));
  state.edit.zero_crossings = ingest::zero_crossings_in(
      fixture_sample(), state.edit.view.first_frame, state.edit.view.frames_visible,
      std::max<std::size_t>(wave_columns / 2, 1));

  state.bins.assign(tui::bins_for_columns(wave_columns), ingest::PeakBin{});
  fixture_pyramid().summarize(fixture_sample(), 0, state.edit.view.first_frame,
                              state.edit.view.frames_visible, state.bins);
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

  SECTION("pattern tab, with a pattern playing") {
    // The one that actually pins the lane. The empty case above would look
    // identical whether steps were drawn or not.
    tui::UiState state = playing_state(100);
    state.tab = tui::PanelTab::kPattern;
    state.pattern = live_pattern();
    check_snapshot("perform_pattern_live_100x30", state, 100, 30);
  }

  SECTION("pattern tab, longer than the lane") {
    // A 32-step pattern in a 16-step lane. The caption must SAY it is showing
    // part of it -- drawing the first half silently would make the second half
    // look empty, which is the failure the whole `showing 1-16` clause exists
    // to prevent.
    tui::UiState state = playing_state(100);
    state.tab = tui::PanelTab::kPattern;
    state.pattern = live_pattern();
    state.pattern.length = 32;
    state.pattern.swing = 58;
    state.pattern.song = true;
    state.pattern.slot = 2;
    check_snapshot("perform_pattern_long_100x30", state, 100, 30);
  }

  SECTION("pattern tab, cursor on the second page of a long pattern") {
    // The other half of the same 32-step pattern. Steps 17-32 are only
    // reachable because the window follows the cursor -- without that they are
    // storable and not editable, which is worse than not having them.
    //
    // The caption has to say `showing 17-32` and the beat ruler has to read 5
    // to 8: two pages that both said `1 2 3 4` would be indistinguishable.
    //
    // The playhead is at step 7, on the first page, so its marker row is BLANK
    // here -- a marker drawn at the same column on the wrong page would be a
    // confident wrong answer about where the music is.
    tui::UiState state = playing_state(100);
    state.tab = tui::PanelTab::kPattern;
    state.pattern = live_pattern();
    state.pattern.length = 32;
    state.pattern.cursor_step = 20;
    state.pattern.rows[0].on[20] = true;
    state.pattern.rows[5].on[31] = true;
    state.selected_pad = 0;
    check_snapshot("perform_pattern_page2_100x30", state, 100, 30);
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

  SECTION("the command line is up") {
    tui::UiState state = playing_state(100);
    state.command_active = true;
    state.command_text = "chop transient";
    check_snapshot("perform_command_100x30", state, 100, 30);
  }

  SECTION("a command answered") {
    tui::UiState state = chopped_state(100);
    state.message = "chop transient: 14 slices on 14 pads";
    check_snapshot("perform_message_100x30", state, 100, 30);
  }

  SECTION("a command refused") {
    tui::UiState state = playing_state(100);
    state.message = "unknown command: frobnicate";
    state.message_is_error = true;
    check_snapshot("perform_error_100x30", state, 100, 30);
  }
}

TEST_CASE("EDIT renders at the 100x30 design grid", "[tui]") {
  SECTION("a slice, framed") {
    check_snapshot("edit_slice_100x30", edit_state(100), 100, 30);
  }

  SECTION("zoomed to where the zero-cross ruler means something") {
    // The framed view is 460 samples per column, at which the material crosses
    // zero several times in every one and the ruler is correctly BLANK. This is
    // the other half of that rule: zoomed near sample resolution the crossings
    // separate, the ticks appear, and the one under a boundary becomes `┃`.
    tui::UiState state = edit_state(100);
    const tui::SliceMark& slice = state.slices[state.edit.slice];
    state.edit.view.first_frame = slice.start_frame > 300 ? slice.start_frame - 300 : 0;
    state.edit.view.frames_visible = 900;
    state.edit.view.clamp(kFixtureFrames);

    const std::size_t wave_columns = tui::edit_wave_columns_for(100);
    state.edit.zero_crossings = ingest::zero_crossings_in(
        fixture_sample(), state.edit.view.first_frame, state.edit.view.frames_visible,
        std::max<std::size_t>(wave_columns / 2, 1));

    // Put the boundary ON a crossing, which is what the snap does to it. Without
    // this the `┃` tick -- a crossing that is also a boundary, and the one thing
    // this row is really answering -- never appears in any snapshot.
    if (!state.edit.zero_crossings.empty()) {
      state.slices[state.edit.slice].start_frame =
          *std::min_element(state.edit.zero_crossings.begin(), state.edit.zero_crossings.end(),
                            [&](std::size_t left, std::size_t right) {
                              const std::size_t start = slice.start_frame;
                              const auto distance = [start](std::size_t frame) {
                                return frame > start ? frame - start : start - frame;
                              };
                              return distance(left) < distance(right);
                            });
    }

    state.bins.assign(tui::bins_for_columns(wave_columns), ingest::PeakBin{});
    fixture_pyramid().summarize(fixture_sample(), 0, state.edit.view.first_frame,
                                state.edit.view.frames_visible, state.bins);

    check_snapshot("edit_zoomed_100x30", state, 100, 30);
  }

  SECTION("nothing chopped") {
    // Reachable: `:edit` before a chop, or `chop reset` with EDIT already open.
    // It has to say so rather than draw an empty frame that reads as a bug.
    tui::UiState state = playing_state(100);
    state.screen = tui::Screen::kEdit;
    check_snapshot("edit_empty_100x30", state, 100, 30);
  }

  SECTION("the command line, from EDIT") {
    // The prompt and the message belong to neither screen. This is the snapshot
    // that says so -- if they were built per-screen, this is where the second
    // copy would first look different.
    tui::UiState state = edit_state(100);
    state.command_active = true;
    state.command_text = "slot assign 6 3";
    check_snapshot("edit_command_100x30", state, 100, 30);
  }
}

TEST_CASE("EDIT degrades on smaller terminals", "[tui]") {
  SECTION("80x24") {
    check_snapshot("edit_slice_80x24", edit_state(80), 80, 24);
  }
  SECTION("60x20, where the slice table cannot stand beside the envelope") {
    check_snapshot("edit_slice_60x20", edit_state(60), 60, 20);
  }
  SECTION("below the minimum, which is the same message on both screens") {
    tui::UiState state = edit_state(40);
    check_snapshot("edit_too_small_40x12", state, 40, 12);
  }
}

TEST_CASE("EDIT is a different screen, not a decorated one", "[tui]") {
  const std::string perform = strip_ansi(render_screen(chopped_state(100), 100, 30).ToString());
  const std::string edit = strip_ansi(render_screen(edit_state(100), 100, 30).ToString());

  CHECK(perform != edit);

  // The mode name is the one thing that must never be wrong: everything else on
  // screen is read in the light of which mode you are in.
  CHECK(perform.find("perform") != std::string::npos);
  CHECK(edit.find("edit") != std::string::npos);

  // The 4x4 pad grid belongs to PERFORM and the boundary handles to EDIT.
  CHECK(perform.find("bank a") != std::string::npos);
  CHECK(edit.find("bank a") == std::string::npos);
  CHECK(edit.find("h -1") != std::string::npos);
  CHECK(edit.find("H -1") != std::string::npos);
}

TEST_CASE("EDIT says what the snap did to each boundary", "[tui]") {
  tui::UiState state = edit_state(100);
  const std::string moved = strip_ansi(render_screen(state, 100, 30).ToString());

  // A boundary the snap moved and one it did not are different statements, and
  // "free" is the honest word for the second: a boundary that needed no move and
  // one that could not be moved are the same outcome.
  CHECK(moved.find("start snapped -3 smp") != std::string::npos);
  CHECK(moved.find("end free") != std::string::npos);

  state.slices[5].end_snap = 11;
  const std::string both = strip_ansi(render_screen(state, 100, 30).ToString());
  CHECK(both.find("end snapped +11 smp") != std::string::npos);
  CHECK(both.find("end free") == std::string::npos);

  state.edit.snap_enabled = false;
  const std::string off = strip_ansi(render_screen(state, 100, 30).ToString());
  CHECK(off.find("snap off") != std::string::npos);
  CHECK(off.find("snapped") == std::string::npos);
}

TEST_CASE("the envelope is drawn from the numbers beside it", "[tui]") {
  tui::UiState fast = edit_state(100);
  fast.edit.envelope.attack_ms = 0.5F;
  fast.edit.envelope.decay_ms = 20.0F;
  fast.edit.envelope.release_ms = 5.0F;

  tui::UiState slow = edit_state(100);
  slow.edit.envelope.attack_ms = 400.0F;
  slow.edit.envelope.decay_ms = 20.0F;
  slow.edit.envelope.release_ms = 5.0F;

  // A long attack and a short one have to LOOK different, or the curve is
  // decoration next to the numbers rather than a picture of them.
  CHECK(strip_ansi(render_screen(fast, 100, 30).ToString()) !=
        strip_ansi(render_screen(slow, 100, 30).ToString()));

  // And the numbers themselves are on screen, in their own units.
  const std::string painted = strip_ansi(render_screen(slow, 100, 30).ToString());
  CHECK(painted.find("400 ms") != std::string::npos);
  CHECK(painted.find("gate") != std::string::npos);

  // The sustain is shown in dB, not as a fraction: it is a level, and every
  // other level on this interface is in dB.
  tui::UiState half = edit_state(100);
  half.edit.envelope.sustain = 0.5F;
  CHECK(strip_ansi(render_screen(half, 100, 30).ToString()).find("-6.0 dB") != std::string::npos);
}

TEST_CASE("the slice table follows the slice being edited", "[tui]") {
  tui::UiState early = edit_state(100);
  early.edit.slice = 1;
  const std::string first = strip_ansi(render_screen(early, 100, 30).ToString());

  tui::UiState late = edit_state(100);
  late.edit.slice = 14;
  const std::string last = strip_ansi(render_screen(late, 100, 30).ToString());

  // The row you are editing has to be on screen. A table pinned to the first six
  // slices would silently stop showing it, and nothing else would look wrong.
  CHECK(first.find("slice 02") != std::string::npos);
  CHECK(last.find("slice 15") != std::string::npos);
  CHECK(first != last);
}

TEST_CASE("the prompt takes the mode line, and the answer takes it back", "[tui]") {
  const std::string idle = strip_ansi(render_screen(playing_state(100), 100, 30).ToString());

  tui::UiState prompting = playing_state(100);
  prompting.command_active = true;
  prompting.command_text = "chop gr";
  const std::string prompt = strip_ansi(render_screen(prompting, 100, 30).ToString());

  // What is being typed is on screen, and the keymap it displaced is not. A
  // prompt sharing the line with a keymap is a prompt you cannot read.
  CHECK(prompt.find(":chop gr") != std::string::npos);
  CHECK(idle.find("esc quit") != std::string::npos);
  CHECK(prompt.find("esc quit") == std::string::npos);
  CHECK(prompt.find("voices") == std::string::npos);

  tui::UiState answered = playing_state(100);
  answered.message = "chop grid: 16 slices on 16 pads";
  const std::string answer = strip_ansi(render_screen(answered, 100, 30).ToString());

  CHECK(answer.find("16 slices on 16 pads") != std::string::npos);
  CHECK(answer.find("esc quit") == std::string::npos);

  // And the mode indicator survives both, so the line never stops saying where
  // you are.
  CHECK(prompt.find("perform") != std::string::npos);
  CHECK(answer.find("perform") != std::string::npos);
}

TEST_CASE("a long command line keeps its end on screen", "[tui]") {
  // Typing past the width has to keep showing the CURSOR END. Truncating the
  // tail instead would look exactly like a prompt that had stopped accepting
  // input, which is the one impression it must never give.
  tui::UiState state = playing_state(60);
  state.command_active = true;
  state.command_text = std::string(60, 'a') + "slot assign 12 7";

  const std::string painted = strip_ansi(render_screen(state, 60, 20).ToString());
  CHECK(painted.find("slot assign 12 7") != std::string::npos);

  // The head is gone -- 76 characters do not fit in 60 columns...
  CHECK(painted.find(std::string(60, 'a')) == std::string::npos);
  // ...but the colon is not, because it is pinned rather than scrolled.
  CHECK(painted.find(":a") != std::string::npos);
}

TEST_CASE("a refusal does not look like a confirmation", "[tui]") {
  // Same message, different flag. If these rendered identically then every
  // refusal would read as a success, which is worse than saying nothing.
  tui::UiState ok = playing_state(100);
  ok.message = "chop transient: 14 slices on 14 pads";

  tui::UiState bad = ok;
  bad.message_is_error = true;

  // Compared WITH the escape sequences: the difference is entirely colour and
  // weight, so stripping them first would compare two identical strings and
  // pass no matter what.
  CHECK(render_screen(ok, 100, 30).ToString() != render_screen(bad, 100, 30).ToString());
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

TEST_CASE("EDIT spends its accent on the boundaries and nothing else", "[tui]") {
  // The same rule on the other screen, and the reason it needs its own test: a
  // second screen is a second budget, and EDIT draws two full-height rules where
  // PERFORM draws one playhead.
  const ftxui::Screen screen = render_screen(edit_state(100), 100, 30);

  std::size_t accent_cells = 0;
  std::size_t accent_rows = 0;
  for (int y = 0; y < screen.dimy(); ++y) {
    bool row_has_accent = false;
    for (int x = 0; x < screen.dimx(); ++x) {
      if (screen.CellAt(x, y).foreground_color == tui::theme::accent()) {
        ++accent_cells;
        row_has_accent = true;
      }
    }
    accent_rows += row_has_accent ? 1 : 0;
  }
  INFO("accent cells: " << accent_cells << " across " << accent_rows << " rows");

  // Two rules through the panel and their two ticks: about two per row of the
  // panel interior and nothing anywhere else. Twice that would mean something
  // other than the boundaries had picked the colour up.
  CHECK(accent_cells > 0);
  CHECK(accent_cells < 40);

  // And they are RULES: a vertical line means every waveform row is accented in
  // the same two columns, which a marker drawn on one row would not be.
  CHECK(accent_rows >= 9);
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

TEST_CASE("a sequenced hit is lit but never inverted", "[tui]") {
  // Inversion is the loudest thing this interface can do to a cell, and it reads
  // as a strike -- something you hit, just now. The machine playing a pattern is
  // a different event and should look like one.
  //
  // Asserted on the CELLS rather than on a snapshot, because the two frames
  // differ only in an attribute: the characters are identical, so a text golden
  // could not tell them apart at all.
  const auto inverted_cells = [](bool sequenced) {
    tui::UiState state = loaded_state(100);
    state.pads[0].triggered = true;
    state.pads[0].glow_seconds = 0.01F;  // hot
    state.pads[0].glow_velocity = 1.0F;
    state.pads[0].glow_sequenced = sequenced;

    const ftxui::Screen screen = render_screen(state, 100, 30);
    std::size_t inverted = 0;
    for (int y = 14; y < 27; ++y) {
      for (int x = 0; x < 46; ++x) {
        if (screen.CellAt(x, y).inverted) {
          ++inverted;
        }
      }
    }
    return inverted;
  };

  const std::size_t live = inverted_cells(false);
  const std::size_t sequenced = inverted_cells(true);
  INFO("inverted cells: " << live << " live, " << sequenced << " sequenced");

  CHECK(live > 0);        // a live hit at full velocity inverts
  CHECK(sequenced == 0);  // the same hit from the sequencer does not
}

TEST_CASE("moving the step cursor does not move the pattern", "[tui]") {
  // The cursor is an ATTRIBUTE, never a character. Drawing it splits the row
  // into three pieces for FTXUI to lay out, and the failure mode of that is a
  // row one cell narrower than its neighbours -- which reads on screen as the
  // pattern shifting under the cursor as it moves.
  //
  // That is not hypothetical: it is exactly what splice_at() produced before
  // paint_at() replaced it, because FTXUI shrinks an over-wide hbox by taking
  // cells from every child in proportion rather than truncating the last one.
  // Asserted over every pad and every step, in both lane columns and on both
  // pages, because the bug only appeared on the row the cursor was on.
  const auto lane_text = [](std::size_t pad, std::size_t step, std::uint8_t length) {
    tui::UiState state = loaded_state(100);
    state.tab = tui::PanelTab::kPattern;
    state.pattern = live_pattern();
    state.pattern.length = length;
    state.pattern.cursor_step = step;
    state.selected_pad = static_cast<std::uint8_t>(pad);
    return strip_ansi(render_screen(state, 100, 30).ToString());
  };

  const std::string base = lane_text(0, 0, 16);
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    for (std::size_t step = 0; step < 16; ++step) {
      INFO("pad " << pad + 1 << ", step " << step + 1);
      CHECK(lane_text(pad, step, 16) == base);
    }
  }

  // And within a page of a longer pattern. Crossing to the second page is
  // SUPPOSED to change the text -- that is the window moving, not the row
  // slipping -- so each page is compared against its own first cell.
  const std::string second_page = lane_text(0, 16, 32);
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    for (std::size_t step = 16; step < 32; ++step) {
      INFO("pad " << pad + 1 << ", step " << step + 1 << " of 32");
      CHECK(lane_text(pad, step, 32) == second_page);
    }
  }
  CHECK(second_page != lane_text(0, 0, 32));
}

TEST_CASE("the step cursor is drawn where the next edit will land", "[tui]") {
  // ON THE CELLS, not on a snapshot: the cursor is an inverted cell, so the
  // characters are identical with and without it and a text golden could not
  // tell the two frames apart. Same argument as the sequenced-glow test below.
  //
  // The lane occupies the right-hand panel, so the search is bounded to it --
  // otherwise a stray inversion in the pad grid would answer this question.
  const auto cursor_columns = [](std::size_t pad, std::size_t step) {
    tui::UiState state = loaded_state(100);
    state.tab = tui::PanelTab::kPattern;
    state.pattern = live_pattern();
    state.pattern.transport_running = false;
    state.pattern.playhead = false;
    state.pattern.cursor_step = step;
    state.selected_pad = static_cast<std::uint8_t>(pad);

    const ftxui::Screen screen = render_screen(state, 100, 30);
    std::vector<std::pair<int, int>> cells;
    for (int y = 14; y < 27; ++y) {
      for (int x = 46; x < 100; ++x) {
        if (screen.CellAt(x, y).inverted) {
          cells.emplace_back(x, y);
        }
      }
    }
    return cells;
  };

  // Exactly one cell, wherever it is. Two would mean the left and right columns
  // of the lane were both drawing it.
  const std::vector<std::pair<int, int>> origin = cursor_columns(0, 0);
  REQUIRE(origin.size() == 1);

  // Moving the cursor one step moves it one column right; the group gaps mean
  // step 4 is five columns along rather than four.
  const std::vector<std::pair<int, int>> next = cursor_columns(0, 1);
  REQUIRE(next.size() == 1);
  CHECK(next[0].second == origin[0].second);
  CHECK(next[0].first == origin[0].first + 1);

  const std::vector<std::pair<int, int>> beat = cursor_columns(0, 4);
  REQUIRE(beat.size() == 1);
  CHECK(beat[0].first == origin[0].first + 5);

  // Moving the PAD moves it down a row -- and pad 9 crosses to the lane's right
  // column, back onto the first row. That crossing is the part worth pinning:
  // a cursor that stayed in the left column would point at pad 1's steps while
  // claiming to be editing pad 9's.
  const std::vector<std::pair<int, int>> second_row = cursor_columns(1, 0);
  REQUIRE(second_row.size() == 1);
  CHECK(second_row[0].second == origin[0].second + 1);
  CHECK(second_row[0].first == origin[0].first);

  const std::vector<std::pair<int, int>> right_column = cursor_columns(8, 0);
  REQUIRE(right_column.size() == 1);
  CHECK(right_column[0].second == origin[0].second);
  CHECK(right_column[0].first > origin[0].first);
}

TEST_CASE("the lane's window follows the cursor onto the second page", "[tui]") {
  // A 32-step pattern is two pages of sixteen. Without the window following,
  // steps 17-32 could be stored and never edited.
  const auto lane_text = [](std::size_t cursor) {
    tui::UiState state = loaded_state(100);
    state.tab = tui::PanelTab::kPattern;
    state.pattern = live_pattern();
    state.pattern.length = 32;
    state.pattern.cursor_step = cursor;
    state.pattern.rows[0] = tui::PatternRow{};
    state.pattern.rows[0].on[17] = true;  // second page only
    return strip_ansi(render_screen(state, 100, 30).ToString());
  };

  const std::string first_page = lane_text(0);
  CHECK(first_page.find("showing 1-16") != std::string::npos);

  const std::string second_page = lane_text(20);
  CHECK(second_page.find("showing 17-32") != std::string::npos);

  // The beat ruler is numbered from the start of the PATTERN, so the second
  // page reads 5 to 8. Restarting at 1 would make the two pages identical in a
  // screenshot.
  CHECK(second_page.find("5    6    7    8") != std::string::npos);
  CHECK(first_page.find("1    2    3    4") != std::string::npos);

  // And the hit at step 18 is only drawn on the page that contains it.
  CHECK(second_page.find("·█·· ···· ···· ····") != std::string::npos);
}

TEST_CASE("the lane says when the metronome is on", "[tui]") {
  // A click nobody can see the state of is one that gets left running into a
  // bounce -- and under --no-audio, where nothing is heard at all, the caption
  // is the only place it exists.
  tui::UiState state = loaded_state(100);
  state.tab = tui::PanelTab::kPattern;
  state.pattern = live_pattern();

  CHECK(strip_ansi(render_screen(state, 100, 30).ToString()).find("metro") == std::string::npos);

  state.pattern.metronome = true;
  CHECK(strip_ansi(render_screen(state, 100, 30).ToString()).find("metro") != std::string::npos);

  // It outranks swing in the caption, because the drop loop takes from the end
  // and these two are not equally droppable: swing is audible as a feel, a
  // click left running is audible as a click.
  state.pattern.swing = 58;
  const std::string both = strip_ansi(render_screen(state, 100, 30).ToString());
  REQUIRE(both.find("metro") != std::string::npos);
  CHECK(both.find("metro") < both.find("swing 58%"));
}

TEST_CASE("the playhead is not drawn on a pattern the transport is not on", "[tui]") {
  // While a song plays, the lane shows the pattern being EDITED and the
  // transport is often somewhere else. A marker moving across a grid the audio
  // is not playing would be a confident wrong answer, so it is suppressed and
  // the caption's `slot N` is what says the song is still running.
  tui::UiState state = loaded_state(100);
  state.tab = tui::PanelTab::kPattern;
  state.pattern = live_pattern();
  state.pattern.song = true;
  state.pattern.slot = 2;

  state.pattern.playhead = true;
  const std::string on_it = strip_ansi(render_screen(state, 100, 30).ToString());

  state.pattern.playhead = false;
  const std::string elsewhere = strip_ansi(render_screen(state, 100, 30).ToString());

  CHECK(on_it.find("┯") != std::string::npos);
  CHECK(elsewhere.find("┯") == std::string::npos);

  // The transport is still running either way, and the mode line says so --
  // that is the field the marker does NOT share.
  CHECK(elsewhere.find("slot 3") != std::string::npos);
  CHECK(elsewhere.find("play") != std::string::npos);
}

TEST_CASE("the mode line reports the tempo and the transport", "[tui]") {
  // Neither existed before M4, which is why docs/design/README.md recorded the
  // mode line as showing "no BPM, bar or transport until M4 -- advertising a
  // key that does nothing is worse than not mentioning it".
  tui::UiState state = loaded_state(100);
  state.pattern.bpm_x100 = 9'250;

  state.pattern.transport_running = false;
  const std::string stopped = strip_ansi(render_screen(state, 100, 30).ToString());
  CHECK(stopped.find("92.50 bpm") != std::string::npos);
  CHECK(stopped.find("stop") != std::string::npos);

  state.pattern.transport_running = true;
  const std::string running = strip_ansi(render_screen(state, 100, 30).ToString());
  CHECK(running.find("play") != std::string::npos);

  // And the keymap names the keys that reach them, at the design size. Space
  // rather than `p` as of M4.5: `p` is still an alias, but the line names the
  // binding a player should learn, and `p` goes back to being a pad in M6.
  CHECK(running.find("space play") != std::string::npos);
  CHECK(running.find("t step") != std::string::npos);

  // The command line survives to the design size too. It is the only way to
  // reach chopping, which is what the machine is for -- if a hint tier ever
  // drops it at 100 columns, that is a regression rather than a trade.
  CHECK(running.find(": ") != std::string::npos);
  CHECK(running.find("esc quit") != std::string::npos);
}

TEST_CASE("a sequenced hit is still visible at every step below hot", "[tui]") {
  // The distinction is only at the top step, where the two are actually
  // confusable. Dimming sequenced hits by a whole step instead would make a
  // quiet one invisible, which trades one wrong answer for another.
  const auto accent_cells = [](float age, bool sequenced) {
    tui::UiState state = loaded_state(100);
    state.pads[0].triggered = true;
    state.pads[0].glow_seconds = age;
    state.pads[0].glow_velocity = 1.0F;
    state.pads[0].glow_sequenced = sequenced;

    const ftxui::Screen screen = render_screen(state, 100, 30);
    std::size_t accent = 0;
    for (int y = 14; y < 27; ++y) {
      for (int x = 0; x < 46; ++x) {
        if (screen.CellAt(x, y).foreground_color == tui::theme::accent()) {
          ++accent;
        }
      }
    }
    return accent;
  };

  // At the "lit" and "dim" steps the two are identical -- same colour, same
  // count of accented cells.
  CHECK(accent_cells(0.15F, true) == accent_cells(0.15F, false));
  CHECK(accent_cells(0.30F, true) == accent_cells(0.30F, false));
  CHECK(accent_cells(0.30F, true) > 0);
}

TEST_CASE("a hit that has not been heard yet does not light its pad", "[tui]") {
  // A negative age is a sequenced hit still in flight to the listener (see
  // engine::PadGlow). Without the guard in glow_intensity() the clamp reads
  // "more than full brightness" as full brightness, and the pad lights EARLY --
  // exactly what the listener-time delay exists to prevent.
  tui::UiState state = loaded_state(100);
  state.pads[0].triggered = true;
  state.pads[0].glow_seconds = -0.15F;  // rendered, not yet heard
  state.pads[0].glow_velocity = 1.0F;
  state.pads[0].glow_sequenced = true;

  CHECK(tui::glow_intensity(state.pads[0]) == 0.0F);

  const ftxui::Screen screen = render_screen(state, 100, 30);
  std::size_t lit = 0;
  for (int y = 14; y < 27; ++y) {
    for (int x = 0; x < 46; ++x) {
      if (screen.CellAt(x, y).inverted) {
        ++lit;
      }
    }
  }
  CHECK(lit == 0);
}

// -- MIX ---------------------------------------------------------------------

namespace {

// A magnitude response, sampled left to right, standing in for what the control
// thread computes from the published coefficients. Hand-built rather than run
// through rt::make_eq_band so the snapshot's curve is a fact about the RENDERER
// rather than about the cookbook -- biquad_test.cpp already owns the filter.
[[nodiscard]] std::vector<float> curve_of(float low, float mid, float high) {
  std::vector<float> curve(16, 0.0F);
  for (std::size_t index = 0; index < curve.size(); ++index) {
    const auto position = static_cast<float>(index) / static_cast<float>(curve.size() - 1);
    const float a = low * (1.0F - position) * (1.0F - position);
    const float b = mid * (1.0F - std::abs((2.0F * position) - 1.0F));
    const float c = high * position * position;
    curve[index] = a + b + c;
  }
  return curve;
}

[[nodiscard]] tui::UiState mix_state(int columns) {
  tui::UiState state = chopped_state(columns);
  state.screen = tui::Screen::kMix;

  static constexpr std::array<std::string_view, 8> kNames{"kick", "snap", "rim",  "hat",
                                                          "chop", "bass", "keys", "vox"};
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    tui::StripView& strip = state.mix.strips[pad];
    strip.name = pad < kNames.size() ? std::string{kNames[pad]} : "--";

    // Deliberately varied, so one snapshot covers a fader above unity, several
    // below, a silent strip, a muted one and a soloed one. A snapshot that only
    // ever shows the default state cannot tell a renderer that ignores its
    // input from one that does not.
    strip.gain_db = 3.0F - (static_cast<float>(pad) * 2.1F);
    strip.peak = pad == 3 ? 0.0F : 0.9F / (1.0F + (0.35F * static_cast<float>(pad)));
    strip.balance = pad == 1 ? -0.5F : 0.0F;
    strip.bus = static_cast<std::uint8_t>(pad % rt::kNumBuses);
  }

  state.mix.strips[0].eq_curve = curve_of(9.0F, 0.0F, -4.0F);
  state.mix.strips[0].eq_label = "lo+9";
  state.mix.strips[0].comp_label = "4:1";
  state.mix.strips[0].reduction = 0.35F;

  state.mix.strips[2].eq_curve = curve_of(-12.0F, 6.0F, 0.0F);
  state.mix.strips[2].eq_label = "2bd";

  state.mix.strips[4].mute = true;
  state.mix.strips[6].solo = true;
  state.mix.any_solo = true;

  for (std::size_t bus = 0; bus < rt::kNumBuses; ++bus) {
    tui::StripView& strip = state.mix.buses[bus];
    strip.name = "bus";
    strip.gain_db = static_cast<float>(bus) * -1.5F;
    strip.peak = 0.8F / (1.0F + static_cast<float>(bus));
    strip.has_balance = false;
    strip.has_bus = false;
    strip.routed = 4;
  }

  state.mix.master.name = "master";
  state.mix.master.gain_db = 0.0F;
  state.mix.master.peak = 0.93F;
  state.mix.master.has_balance = false;
  state.mix.master.has_bus = false;
  state.mix.limiter_enabled = true;
  state.mix.limiter_gain = 0.72F;
  state.master_peak = 0.93F;

  return state;
}

}  // namespace

TEST_CASE("MIX draws eight strips and master at the design grid", "[tui]") {
  check_snapshot("mix_100x30", mix_state(100), 100, 30);
}

TEST_CASE("MIX shows the second page of channels", "[tui]") {
  tui::UiState state = mix_state(100);
  state.mix.page = tui::MixPage::kChannelsHigh;
  state.mix.cursor = 2;
  check_snapshot("mix_page_high_100x30", state, 100, 30);
}

TEST_CASE("MIX shows the buses on their own page", "[tui]") {
  // THE DEPARTURE FROM THE MOCKUP. It draws no bus strips at all, and twenty-one
  // strips do not fit on a hundred columns, so the row pages and master stays
  // pinned right. docs/design/README.md records it.
  tui::UiState state = mix_state(100);
  state.mix.page = tui::MixPage::kBuses;
  check_snapshot("mix_page_buses_100x30", state, 100, 30);
}

TEST_CASE("MIX drops strips rather than shrinking them when narrow", "[tui]") {
  // Master is pinned, so a narrow terminal loses channels from the right rather
  // than squeezing every strip until the names and readouts stop fitting.
  check_snapshot("mix_72x30", mix_state(72), 72, 30);
}

namespace {

// The row a strip's fader cap sits on, found by looking for the cap glyph in the
// panel's columns. Screen inspection rather than string matching, because the
// row is what the test is about.
[[nodiscard]] int fader_cap_row(const ftxui::Screen& screen, int panel_left) {
  for (int y = 0; y < screen.dimy(); ++y) {
    for (int x = panel_left; x < panel_left + 10 && x < screen.dimx(); ++x) {
      if (screen.PixelAt(x, y).character == "━") {
        return y;
      }
    }
  }
  return -1;
}

// One row of the screen as text. Needed because "does the word master appear"
// is answered by the MODE LINE as well as by the panel -- a weaker version of
// the test below passed happily with the master panel deleted.
[[nodiscard]] std::string screen_row(const ftxui::Screen& screen, int row) {
  std::string out;
  for (int x = 0; x < screen.dimx(); ++x) {
    out += screen.PixelAt(x, row).character;
  }
  return out;
}

[[nodiscard]] std::size_t meter_cells(const ftxui::Screen& screen, int panel_left) {
  std::size_t lit = 0;
  for (int y = 0; y < screen.dimy(); ++y) {
    for (int x = panel_left; x < panel_left + 10 && x < screen.dimx(); ++x) {
      if (screen.PixelAt(x, y).character == "█") {
        ++lit;
      }
    }
  }
  return lit;
}

}  // namespace

TEST_CASE("the fader cap moves with the gain, and upward means louder", "[tui]") {
  tui::UiState quiet = mix_state(100);
  quiet.mix.strips[0].gain_db = -40.0F;
  tui::UiState loud = mix_state(100);
  loud.mix.strips[0].gain_db = 6.0F;

  const int quiet_row = fader_cap_row(render_screen(quiet, 100, 30), 1);
  const int loud_row = fader_cap_row(render_screen(loud, 100, 30), 1);

  REQUIRE(quiet_row > 0);
  REQUIRE(loud_row > 0);

  // Smaller y is higher on the screen, so louder must sit ABOVE quieter. A
  // fader drawn upside down still moves when the gain changes, which is why the
  // direction is asserted rather than just the difference.
  CHECK(loud_row < quiet_row);
}

TEST_CASE("the meter is scaled in decibels, not linearly", "[tui]") {
  // A linear meter over ten rows spends nine of them on the top 20 dB and reads
  // EMPTY for anything quiet -- which on a sampler is most of the time. -40 dBFS
  // is quiet and audible, and must light something.
  tui::UiState state = mix_state(100);
  for (tui::StripView& strip : state.mix.strips) {
    strip.peak = 0.0F;
  }
  state.mix.strips[0].peak = 0.01F;  // -40 dBFS

  const ftxui::Screen screen = render_screen(state, 100, 30);
  CHECK(meter_cells(screen, 1) > 0);

  // ...and it is not simply full: a meter that lights everything for everything
  // would also pass the line above.
  tui::UiState hot = state;
  hot.mix.strips[0].peak = 1.0F;
  CHECK(meter_cells(render_screen(hot, 100, 30), 1) > meter_cells(screen, 1));
}

TEST_CASE("a strip that is not reaching the mix shows no meter", "[tui]") {
  // engine::Telemetry::strip_peak is zero for a muted or soloed-out strip, and
  // the meter beside the fader must say so rather than showing what the pad
  // played. The two questions are answered by two different numbers
  // (docs/MIXER.md, "Metering").
  tui::UiState state = mix_state(100);
  state.mix.strips[0].peak = 0.0F;
  CHECK(meter_cells(render_screen(state, 100, 30), 1) == 0);
}

TEST_CASE("master is pinned on every page", "[tui]") {
  // The reason the strip row pages at all is that twenty-one strips do not fit.
  // Master is the one you always want, so it is not part of the paging.
  for (const tui::MixPage page :
       {tui::MixPage::kChannelsLow, tui::MixPage::kChannelsHigh, tui::MixPage::kBuses}) {
    tui::UiState state = mix_state(100);
    state.mix.page = page;
    const std::string titles = screen_row(render_screen(state, 100, 30), 3);
    INFO("page " << static_cast<int>(page) << ", titles: " << titles);
    CHECK(titles.find("master") != std::string::npos);
  }
}

TEST_CASE("a narrow terminal drops strips and keeps master", "[tui]") {
  for (const int columns : {100, 84, 72, 60}) {
    const ftxui::Screen screen = render_screen(mix_state(columns), columns, 30);

    // The PANEL TITLE ROW, not the whole screen: the mode line also says
    // "master", so searching the render as a whole finds it whether or not the
    // panel is there -- which is exactly what let a version of this test pass
    // with the master panel deleted.
    const std::string titles = screen_row(screen, 3);
    INFO("columns " << columns << ", titles: " << titles);
    CHECK(titles.find("master") != std::string::npos);

    // Strip 1 is always drawn, and always at the same width -- panels are
    // dropped rather than squeezed, because a squeezed strip loses its name and
    // its readout, which is most of what it is for.
    CHECK(titles.find("01 kick") != std::string::npos);
  }
}

TEST_CASE("the EQ curve follows the published response", "[tui]") {
  // The curve is drawn from StripView::eq_curve, so a boost must draw higher
  // than a cut. Without this, a renderer that ignored the curve entirely would
  // still pass every snapshot that happens to contain a flat one.
  tui::UiState boosted = mix_state(100);
  boosted.mix.strips[1].eq_curve = std::vector<float>(16, 9.0F);
  tui::UiState cut = mix_state(100);
  cut.mix.strips[1].eq_curve = std::vector<float>(16, -9.0F);

  const std::string with_boost = strip_ansi(render_screen(boosted, 100, 30).ToString());
  const std::string with_cut = strip_ansi(render_screen(cut, 100, 30).ToString());
  CHECK(with_boost != with_cut);

  // And flat is different from both, so the curve is not merely "any two
  // settings differ".
  tui::UiState flat = mix_state(100);
  flat.mix.strips[1].eq_curve = std::vector<float>(16, 0.0F);
  const std::string with_flat = strip_ansi(render_screen(flat, 100, 30).ToString());
  CHECK(with_flat != with_boost);
  CHECK(with_flat != with_cut);
}

TEST_CASE("the MIX mode line keeps its facts and picks a hint tier that fits", "[tui]") {
  // MEASURED, NOT REASONED ABOUT. M4.5 proved that hand arithmetic on this line
  // puts a fact on the design grid at one tempo and off it at another, so the
  // widths that matter are checked rather than argued.
  for (const int columns : {60, 72, 84, 100}) {
    const ftxui::Screen screen = render_screen(mix_state(columns), columns, 30);
    const std::string line = screen_row(screen, 29);
    INFO("columns " << columns << "\n[" << line << "]");

    // Both facts survive at every width the layout supports. The limiter readout
    // is the one that would go first, and it is the one that says whether the
    // master is being held down at all.
    CHECK(line.find("master") != std::string::npos);
    CHECK(line.find("lim") != std::string::npos);

    // And some keymap is always shown -- a mode line with no hint is a mode line
    // that has stopped doing its second job.
    CHECK(line.find("[] page") != std::string::npos);
  }

  // The widest tier is what the design grid gets, and it names every key MIX
  // binds. If a key is added without extending this, the hint silently stops
  // describing the screen.
  const std::string wide = screen_row(render_screen(mix_state(100), 100, 30), 29);
  for (const std::string& key : {std::string{"[] page"}, std::string{"hjkl"}, std::string{"m mute"},
                                 std::string{"s solo"}, std::string{"b bus"}}) {
    INFO("wide tier: [" << wide << "] missing " << key);
    CHECK(wide.find(key) != std::string::npos);
  }
}

// -- BROWSE ------------------------------------------------------------------

namespace {

[[nodiscard]] tui::UiState browse_state(int columns) {
  tui::UiState state = chopped_state(columns);
  state.screen = tui::Screen::kBrowse;
  state.browser.path = "/crate/breaks";
  state.browser.entries = {
      tui::BrowserEntry{.name = "..", .is_directory = true},
      tui::BrowserEntry{.name = "kits", .is_directory = true},
      tui::BrowserEntry{.name = "amen_brother.wav", .bytes = 1'482'000, .loaded = true},
      tui::BrowserEntry{.name = "think_break.wav", .bytes = 806'400},
      tui::BrowserEntry{.name = "a_very_long_filename_that_will_not_fit_in_the_panel.wav",
                        .bytes = 12'700'000},
      tui::BrowserEntry{.name = "vocal_take.wav", .bytes = 240},
  };
  state.browser.cursor = 2;

  state.files = {
      tui::UiState::FileEntry{
          .id = static_cast<ingest::FileId>(1), .name = "amen_brother.wav", .slices = 8},
      tui::UiState::FileEntry{
          .id = static_cast<ingest::FileId>(2), .name = "vocal.wav", .slices = 1},
  };
  state.current_file = static_cast<ingest::FileId>(1);
  return state;
}

}  // namespace

TEST_CASE("BROWSE lists a directory beside the crate", "[tui]") {
  check_snapshot("browse_100x30", browse_state(100), 100, 30);
}

TEST_CASE("BROWSE drops the crate panel before it squeezes the listing", "[tui]") {
  // The listing is what BROWSE is for: a browser that cannot show a filename has
  // stopped being one, so the crate is what gives way.
  check_snapshot("browse_72x24", browse_state(72), 72, 24);
}

TEST_CASE("BROWSE says why a listing is empty", "[tui]") {
  // An unreadable directory and an empty one look identical otherwise, and only
  // one of them is a mistake to fix.
  tui::UiState state = browse_state(100);
  state.browser.entries.clear();
  state.browser.note = "cannot read breaks — permission denied";
  check_snapshot("browse_empty_100x30", state, 100, 30);
}

TEST_CASE("BROWSE scrolls to keep the cursor on screen", "[tui]") {
  // A listing longer than the panel. The window follows the cursor rather than
  // the cursor being clamped to the window -- a browser that stopped at the
  // bottom of the first page is one nobody can reach the end of.
  tui::UiState state = browse_state(100);
  state.browser.entries.clear();
  for (int index = 0; index < 40; ++index) {
    state.browser.entries.push_back(
        tui::BrowserEntry{.name = "take_" + std::to_string(index) + ".wav", .bytes = 48'000});
  }
  state.browser.cursor = 37;
  check_snapshot("browse_scrolled_100x20", state, 100, 20);
}

// -- the completion menu ------------------------------------------------------

namespace {

[[nodiscard]] tui::UiState completing(int columns, const std::string& typed, tui::CompletionSet set,
                                      std::size_t cursor = 0) {
  // The cursor moves BEFORE the line is built. Getting this backwards is what
  // the first version did, and it produced a snapshot whose line read
  // `chop grid` while the marker sat on `chop transient` -- a picture of a bug
  // the program does not have, committed as the expected output.
  set.cursor = cursor;

  tui::UiState state = chopped_state(columns);
  state.command_active = true;
  state.completion.entries = set.entries;
  state.completion.replace_from = set.replace_from;
  state.completion.cursor = cursor;
  state.completion.active = true;
  state.command_text = set.apply(typed);
  return state;
}

}  // namespace

TEST_CASE("the completion menu lists what Tab is offering", "[tui]") {
  // The line shows the SELECTION, not what was typed -- so that Enter runs what
  // is on screen rather than what was on it a keystroke ago.
  check_snapshot("complete_verbs_100x30", completing(100, "ch", tui::complete_verbs("ch")), 100,
                 30);
}

TEST_CASE("the completion menu marks the selection as it cycles", "[tui]") {
  check_snapshot("complete_second_100x30", completing(100, "ch", tui::complete_verbs("ch"), 1), 100,
                 30);
}

TEST_CASE("a menu too tall for the terminal shrinks and says how much it hid", "[tui]") {
  // The case that was silently blank before the menu learned to shrink: an empty
  // line matches all thirty-three verbs, which wants eleven rows against a
  // budget of ten at the design size.
  check_snapshot("complete_all_100x30", completing(100, "", tui::complete_verbs("")), 100, 30);
}

TEST_CASE("a path completion clips from the left", "[tui]") {
  // A path's identity is at its end. The verbs clip from the right, and the two
  // are told apart by which has a detail column.
  const tui::PathContext context = tui::path_being_typed("load /home/someone/Music/Samples/br");
  const std::vector<std::string> names{
      "/home/someone/Music/Samples/breakbeats_and_the_other_things_kept_here/"
      "amen_brother_full_take_02.wav",
      "/home/someone/Music/Samples/break.wav",
  };
  check_snapshot(
      "complete_paths_100x30",
      completing(100, "load /home/someone/Music/Samples/br", tui::complete_paths(context, names)),
      100, 30);
}

TEST_CASE("the prompt carries its own note", "[tui]") {
  // The message branch of mode_line() is unreachable while the prompt is up --
  // the prompt returns first -- so anything a command said mid-typing was
  // invisible. Found when Tab wanted to answer "nothing matches", which is a
  // thing it MUST say: a Tab that does nothing is indistinguishable from a Tab
  // the terminal ate, and this project has already spent a milestone believing
  // exactly that.
  tui::UiState state = chopped_state(100);
  state.command_active = true;
  state.command_text = "zzz";
  state.message = "no command starts with that";
  check_snapshot("prompt_note_100x30", state, 100, 30);
}

// -- the BROWSE preview strip -------------------------------------------------

namespace {

[[nodiscard]] tui::UiState previewing(int columns, int rows) {
  tui::UiState state = browse_state(columns);

  // Four bursts and four gaps, so the picture is obviously a picture of
  // something rather than a band of ink.
  state.preview.name = "amen_brother.wav";
  state.preview.frames = 96'000;
  state.preview.rate = 48'000;
  const std::size_t count =
      tui::bins_for_columns(tui::preview_columns_for(static_cast<std::size_t>(columns)));
  state.preview.bins.assign(count, ingest::PeakBin{});
  for (std::size_t bin = 0; bin < count; ++bin) {
    const float level = (bin / (count / 8 + 1)) % 2 == 0 ? 0.85F : 0.04F;
    state.preview.bins[bin] = ingest::PeakBin{.min = -level, .max = level};
  }
  state.preview.playhead = 36'000;
  state.preview.playing = true;
  static_cast<void>(rows);
  return state;
}

}  // namespace

TEST_CASE("BROWSE draws what is being previewed", "[tui]") {
  // The visual the browser was missing: a listing says a file is 47 KB and
  // nothing else, and 47 KB of loop and 47 KB of silence-then-a-crash look
  // identical until you have waited through both.
  check_snapshot("browse_preview_100x30", previewing(100, 30), 100, 30);
}

TEST_CASE("the preview marker stays where a finished sound left it", "[tui]") {
  // Hollow rather than filled, and NOT reset to zero: a preview that ended
  // should leave the picture where it ended rather than jumping the marker home
  // and implying it is about to start again.
  tui::UiState state = previewing(100, 30);
  state.preview.playing = false;
  check_snapshot("browse_preview_stopped_100x30", state, 100, 30);
}

TEST_CASE("the preview gives way on a short terminal", "[tui]") {
  // The listing is what BROWSE is for. The strip takes its rows off the panels,
  // so below the point where both fit it is the strip that goes -- the same
  // order of sacrifice the crate panel follows on a narrow one.
  check_snapshot("browse_preview_min_100x20", previewing(100, 20), 100, 20);
}

TEST_CASE("BROWSE shows the region marked for grabbing", "[tui]") {
  // "Browsing should have an ability to just get a slice from a sample": the
  // bracket under the waveform is that slice, and a pad key puts it on a pad.
  tui::UiState state = previewing(100, 30);
  state.preview.region_start = 24'000;
  state.preview.region_end = 60'000;
  state.preview.has_region = true;
  check_snapshot("browse_region_100x30", state, 100, 30);
}

TEST_CASE("a region marked from the very start still shows both edges", "[tui]") {
  // The playhead is drawn over the bracket, so a region starting at frame 0 with
  // the sound at frame 0 puts two glyphs in one cell. The opening bracket loses,
  // which is right -- but the closing one must still be there to say how far the
  // region reaches.
  tui::UiState state = previewing(100, 30);
  state.preview.playhead = 0;
  state.preview.region_start = 0;
  state.preview.region_end = 48'000;
  state.preview.has_region = true;
  check_snapshot("browse_region_from_zero_100x30", state, 100, 30);
}
