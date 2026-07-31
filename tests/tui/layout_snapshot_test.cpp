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
  state.edit.pad = 5;
  state.edit.pad_known = true;
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
