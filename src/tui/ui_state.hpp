#ifndef CRATEDIG_TUI_UI_STATE_HPP
#define CRATEDIG_TUI_UI_STATE_HPP

#include "ingest/peak_pyramid.hpp"
#include "ingest/pool.hpp"
#include "rt/pad_event.hpp"
#include "rt/sequencer.hpp"
#include "rt/strip.hpp"
#include "tui/completion.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tui {

// Everything the interface draws, as plain data.
//
// UiState holds no Engine, no AudioDevice, no Sample and no clock. That is the
// whole design: render() is a pure function of this struct, so a snapshot test
// constructs one as a literal and renders it to an offscreen Screen with no
// audio hardware, no file and no time source involved. Determinism comes from
// there being nothing non-deterministic in scope, rather than from suppressing
// sources of variance one at a time.

// The visible window into a sample, measured in source frames.
//
// Kept in frames rather than seconds or pixels so it survives a terminal resize
// and does not accumulate rounding: the same view at 80 columns and at 120
// columns shows the same audio.
struct WaveView {
  std::size_t first_frame = 0;
  std::size_t frames_visible = 0;

  // Below about this, every column is one frame and zooming further only makes
  // the same picture wider. Sixteen frames is a third of a millisecond -- well
  // past where M3's zero-crossing nudging needs to see individual samples.
  static constexpr std::size_t kMinFramesVisible = 16;

  // Show the whole sample.
  void fit(std::size_t total_frames) noexcept;

  // Keep the window inside the sample and no narrower than kMinFramesVisible.
  // Idempotent, so calling it after every mutation is free.
  void clamp(std::size_t total_frames) noexcept;

  // Positive scrolls right. Saturates at the ends rather than wrapping.
  void scroll_by(std::ptrdiff_t frames, std::size_t total_frames) noexcept;

  // factor > 1 zooms out, < 1 zooms in.
  //
  // ANCHORED AT THE CENTRE of the current view, which is what makes zoom
  // reversible: zooming in and back out returns the identical window. Anchoring
  // at the left edge instead makes scroll-then-zoom drift, and the drift is only
  // obvious after it has already lost your place.
  void zoom_by(double factor, std::size_t total_frames) noexcept;

  [[nodiscard]] std::size_t last_frame() const noexcept { return first_frame + frames_visible; }

  [[nodiscard]] bool contains(std::size_t frame) const noexcept {
    return frame >= first_frame && frame < last_frame();
  }

  // Which of `columns` columns a frame falls in, or columns when it is outside
  // the view. Used for the playhead marker.
  [[nodiscard]] std::size_t column_of(std::size_t frame, std::size_t columns) const noexcept;

  friend bool operator==(const WaveView&, const WaveView&) = default;
};

// Which panel the right-hand box is showing.
//
// The pattern lane belongs to M4's sequencer. It is a selectable tab now, with a
// placeholder behind it, so that landing the sequencer is filling in a panel
// rather than re-cutting the layout -- and so the tab mechanism itself is under
// snapshot test from the start.
enum class PanelTab : std::uint8_t {
  kSample = 0,
  kPattern,
};

struct PadState {
  std::string name;  // empty when nothing is loaded
  float level = 0.0F;
  bool loaded = false;

  // WHICH FILE AND WHICH SLICE this pad plays, or none. Displayed rather than
  // acted on -- the engine already has the config.
  //
  // The file is half the answer as of M5.5, and that is the assumption this
  // milestone removes: until now a session held one file, so a slice index alone
  // named a sound. With a crate it does not -- slice 3 of the break and slice 3
  // of the vocal are different sounds, and a pad grid that showed only the index
  // would say the same thing about both.
  //
  // A FileId rather than a pool position, for the reason ingest/pool.hpp gives:
  // unloading a file must not silently re-point every pad after it.
  bool has_slice = false;
  ingest::FileId file = ingest::kNoFile;
  std::size_t slice_index = 0;

  // How long ago the pad was hit, in seconds, and how hard. Straight from
  // engine::PadGlow; `glow_seconds` is meaningless unless `triggered`.
  bool triggered = false;
  float glow_seconds = 0.0F;
  float glow_velocity = 0.0F;

  // Whether the sequencer played it rather than a person. Straight from
  // engine::PadGlow; the interface draws the two differently.
  bool glow_sequenced = false;
};

// How long a pad stays visibly lit after a hit.
//
// A LOOK, not an engine fact, which is why it lives here and not in the
// telemetry: the engine publishes an age and the interface decides what to do
// with it. 0.35 s is long enough to see at 30 Hz and short enough that a
// sixteenth at 160 bpm still reads as separate flashes.
inline constexpr float kGlowSeconds = 0.35F;

// The visible strength of a pad's glow, 0 when it is not lit.
//
// Velocity scales it, so a soft hit lights the pad less than a hard one -- the
// same information the player put in, coming back out.
[[nodiscard]] float glow_intensity(const PadState& pad) noexcept;

// One chop, as the interface needs it. Frames rather than seconds so the
// waveform view can place it without a rate conversion at every use.
struct SliceMark {
  std::size_t start_frame = 0;
  std::size_t end_frame = 0;

  // How far the zero-crossing snap moved each boundary, signed, in frames.
  // Straight from ingest::Slice, and the reason EDIT can say "start snapped
  // -3 smp" rather than only showing where the boundary ended up.
  std::ptrdiff_t start_snap = 0;
  std::ptrdiff_t end_snap = 0;
};

// The pattern lane, as the interface needs it.
//
// A flat copy rather than a pointer to rt::SequencerState: render() is a pure
// function of UiState and holds no engine (see the note at the top of this
// file), so a snapshot test builds one of these as a literal. Copying ~16 rows
// of bools once per frame is nothing next to drawing them.
struct PatternRow {
  std::array<bool, rt::kMaxSteps> on{};
};

struct PatternView {
  // False until a sequencer state has been published, which is the state a
  // session starts in and is drawn as an empty lane rather than a blank panel.
  bool has_pattern = false;

  std::uint8_t pattern = 0;
  std::size_t length = 16;
  std::uint8_t swing = 0;
  std::uint32_t bpm_x100 = 12'000;

  // Whether the transport is running at all. Distinct from `playhead` below,
  // and the mode line's `play`/`stop` reads THIS one -- the transport can be
  // running on a pattern the lane is not showing.
  bool transport_running = false;

  // Whether the transport is on the pattern the lane is drawing, and `step` is
  // therefore a position in it. False while a song is playing some other
  // pattern, where a marker would claim a position on a grid it does not belong
  // to -- the caption's `slot N` is what says the song is running elsewhere.
  bool playhead = false;
  std::size_t step = 0;

  // Which song slot is playing, and whether there is a song at all. An empty
  // song is a pattern repeating, not slot zero of nothing.
  bool song = false;
  std::uint8_t slot = 0;

  // Where the next step edit will land, in steps. THE PAD comes from
  // UiState::selected_pad rather than being copied here: it is the same
  // selection the pad grid already highlights, and two fields meaning "the pad
  // being worked on" is two fields to keep in step.
  //
  // The lane's sixteen-step window follows this, which is what makes the second
  // half of a 32-step pattern reachable at all.
  std::size_t cursor_step = 0;

  // Drawn in the caption when it is on. A click nobody can see the state of is
  // one that gets left running into a bounce.
  bool metronome = false;

  std::array<PatternRow, rt::kNumPads> rows{};
};

// Which screen is up.
//
// PERFORM and EDIT are two pure functions of this same struct, chosen here. The
// alternative -- two UiStates -- would mean the pad levels, the message line and
// the transport all existed twice and had to be kept in step.
enum class Screen : std::uint8_t {
  kPerform = 0,
  kEdit,
  kMix,
  kBrowse,
};

// One line of the browser's directory listing.
struct BrowserEntry {
  std::string name;
  bool is_directory = false;

  // Size on disk. Shown because it is the one fact available without decoding,
  // and it is enough to tell a loop from an album.
  std::uint64_t bytes = 0;

  // Already in the crate. Marked rather than hidden: seeing that a file is
  // loaded is the answer to "did I already do this", and hiding it would make
  // the directory look different from what is actually in it.
  bool loaded = false;
};

// What the browser is showing.
//
// A SCREEN RATHER THAN A PANEL, and that is a departure the mockups do not cover
// -- they draw no browser at all. A path plus a listing plus the crate beside it
// does not fit in the right-hand panel PERFORM already spends on the sample and
// the pattern, and shrinking it far enough to fit would leave a file browser
// that cannot show a filename.
struct BrowserState {
  // The directory being listed, as text. The renderer never touches the
  // filesystem -- everything here was read on the control thread, which is where
  // I/O belongs and what keeps render() a pure function.
  std::string path;

  std::vector<BrowserEntry> entries;
  std::size_t cursor = 0;

  // First row drawn, for a listing longer than the panel. Kept here rather than
  // recomputed in the renderer so that moving the cursor and scrolling to follow
  // it are one decision made in one place.
  std::size_t first_visible = 0;

  // Why the listing is empty, when it is: an unreadable directory and an empty
  // one look identical otherwise.
  std::string note;
};

// Which page of strips the MIX row is showing.
//
// THE DEPARTURE FROM THE MOCKUP, and the reason for it. The mockup draws eight
// channel strips with master pinned right, filling all hundred columns exactly,
// and draws no bus strips at all. Sixteen channels, four buses and a master is
// twenty-one strips; at the ten columns a legible strip needs, that is two
// hundred and ten. It does not fit and no amount of arranging makes it fit.
//
// So the strip row is a VIEWPORT and the buses are a page of it. Master stays
// pinned right on every page, because master is the one strip you always want to
// see. The alternative -- shrinking strips until all twenty-one fit -- costs the
// name, the EQ curve and the readout, which is most of what a strip is for.
enum class MixPage : std::uint8_t {
  kChannelsLow = 0,  // pads 1-8
  kChannelsHigh,     // pads 9-16
  kBuses,            // buses A-D
};

// One pad's playback parameters, as EDIT displays them.
//
// Milliseconds and linear gains rather than rt::AdsrFrames, because converting
// frames to time needs a sample rate: the control thread has one and the
// renderer does not. Keeping the conversion on the control side is what lets the
// snapshot tests write these as literals.
struct EnvelopeView {
  float attack_ms = 0.0F;
  float decay_ms = 0.0F;
  float sustain = 1.0F;  // linear, 0..1
  float release_ms = 0.0F;

  bool gate = false;
  std::uint8_t choke_group = 0;
  float gain = 1.0F;
  float pitch_ratio = 1.0F;
};

// What EDIT is looking at.
//
// Its own WaveView, separate from PERFORM's: stepping to the next slice zooms
// to it, and coming back to PERFORM must not have moved the overview. Two views
// is the honest model of two screens that both scroll.
struct EditState {
  std::size_t slice = 0;  // 0-based index into UiState::slices
  WaveView view;

  // Where the signal crosses zero inside the current view, in source frames,
  // ascending. Computed on the control thread because it needs the audio; the
  // renderer only places ticks.
  std::vector<std::size_t> zero_crossings;

  EnvelopeView envelope;

  // How many boundary nudges are undoable. Shown rather than acted on -- `u`
  // does the acting, and a count is what tells you whether it will.
  std::size_t undo_depth = 0;

  // Whether the boundaries were snapped to zero crossings when this chop was
  // made. Reported rather than obeyed: the snap already happened, and saying so
  // is what makes "start snapped -3 smp" and "start free" different statements.
  bool snap_enabled = true;
};

// One strip as MIX draws it: a channel, a bus, or the master.
//
// Denominated in what the SCREEN needs rather than in what rt::StripConfig
// holds. dB rather than linear because that is what the readout says and what a
// fader is scaled in; a label rather than an rt::EqConfig because the renderer
// must not have to know the RBJ cookbook to print "4bd". The conversion happens
// on the control thread, which is also what lets the snapshot tests write these
// as literals.
struct StripView {
  std::string name;

  // Fader position and the meter beside it. `peak` is post-fader and linear,
  // matching engine::Telemetry::strip_peak -- and zero when the strip is not
  // reaching the mix, which is the honest thing for a meter next to a fader.
  float gain_db = 0.0F;
  float peak = 0.0F;

  // [-1, +1]. Drawn as a two-character offset under the name, and absent
  // entirely on a bus and on master, neither of which has one.
  float balance = 0.0F;
  bool has_balance = true;

  // The EQ magnitude response in dB, sampled left to right across the panel at
  // braille resolution -- two samples per character column, the same way the
  // waveform is summarised. Empty means the EQ is bypassed and the curve row
  // draws a flat line rather than a lie.
  std::vector<float> eq_curve;

  // What the two fixed chain rows say. The mockup draws two FREE-FORM insert
  // slots ("a comp", "b eq"); the chain is fixed at EQ then compressor
  // (docs/MIXER.md), so these name what is actually there. Arbitrary processors
  // in arbitrary slots is M8's plugin chain.
  std::string eq_label = "--";
  std::string comp_label = "--";

  // Compressor gain reduction, LINEAR, 1.0 meaning none -- matching
  // engine::Telemetry::strip_reduction. Drawn as a downward bar beside the
  // meter, which is the one place on the strip that reads top-down.
  float reduction = 1.0F;

  bool mute = false;
  bool solo = false;

  // Which bus this strip feeds, 0-based. Shown on a channel, meaningless on a
  // bus or on master.
  std::uint8_t bus = 0;
  bool has_bus = true;

  // How many channels feed this bus. Meaningful only on a bus strip, where it
  // fills the space a channel spends on its EQ curve: a bus in M5 has a gain and
  // a meter and nothing else, and "how much is coming into this" is the fact
  // worth putting there rather than three blank rows.
  std::size_t routed = 0;
};

// What MIX is looking at.
struct MixState {
  MixPage page = MixPage::kChannelsLow;

  // Which strip on the current page the keys act on, 0-based within the page.
  std::size_t cursor = 0;

  std::array<StripView, rt::kNumPads> strips;
  std::array<StripView, rt::kNumBuses> buses;
  StripView master;

  // The master limiter's current gain, linear. 1.0 when it is not reducing --
  // and when it is switched off, which is the truth about the signal either way.
  float limiter_gain = 1.0F;
  bool limiter_enabled = false;

  // True when ANY strip is soloed, which is what makes every un-soloed strip
  // silent. Derived on the control thread from the same set the audio thread
  // derives it from, so the screen and the sound agree.
  bool any_solo = false;
};

// The Tab menu's state: what is on offer and which one is selected.
//
// The entries are a copy rather than a reference into the completion table
// because paths are in here too and those are built per keystroke. Cheap: the
// longest set is thirty-three short strings, rebuilt only when Tab is pressed.
struct CompletionState {
  std::vector<Completion> entries;
  std::size_t cursor = 0;

  // Where in the typed line the selection goes. See tui::CompletionSet.
  std::size_t replace_from = 0;

  // Showing. Distinct from `entries.empty()` so that closing the menu and
  // having nothing to offer stay separable -- the first is a thing the person
  // did and the second is a thing the line is.
  bool active = false;

  [[nodiscard]] bool showing() const noexcept { return active && !entries.empty(); }
};

// The file BROWSE is previewing, summarised for drawing.
//
// A VISUAL OF WHAT YOU ARE HEARING, which is what a browser is missing without
// it: the listing says a file is 47 KB and nothing else, and 47 KB of loop and
// 47 KB of silence-then-a-crash look identical until you have waited through
// both.
//
// Held separately from `bins` -- the current file's waveform, which PERFORM and
// EDIT draw -- because a preview is deliberately NOT the current file. The whole
// point of auditioning is hearing something you have not loaded, so the two are
// different sounds and must be able to be on screen at once.
struct PreviewState {
  std::string name;
  std::size_t frames = 0;
  std::uint32_t rate = 48'000;

  std::vector<ingest::PeakBin> bins;

  // Where the sound has got to, in frames from the start of the file, and
  // whether it is still going. Two fields rather than a sentinel because the
  // marker is drawn at its last position when the sound stops -- a preview that
  // finished should leave the picture where it ended rather than jumping the
  // marker home.
  std::size_t playhead = 0;
  bool playing = false;

  // The region marked for grabbing, in frames. `has_region` because an unset
  // region and a region starting at frame 0 are different things.
  std::size_t region_start = 0;
  std::size_t region_end = 0;
  bool has_region = false;

  [[nodiscard]] bool showing() const noexcept { return frames > 0 && !bins.empty(); }
};

struct UiState {
  std::string version;

  Screen screen = Screen::kPerform;
  EditState edit;
  MixState mix;
  BrowserState browser;

  // WHAT THE WAVE PANEL AND EDIT ARE LOOKING AT -- one file out of the pool,
  // not "the" file. Every field below describes that one; the crate as a whole
  // is `files`.
  //
  // Kept as flat display fields rather than as a pointer into the pool, so
  // render() stays a pure function of this struct and needs neither the pool nor
  // the Sample. That is the same reason `bins` is a summarised waveform rather
  // than the audio.
  ingest::FileId current_file = ingest::kNoFile;

  // The loaded sample. `frames == 0` means nothing is loaded, which is a
  // legitimate state the interface has to be able to draw.
  std::string sample_name;
  std::uint32_t sample_rate = 0;
  std::uint16_t sample_channels = 0;
  std::size_t sample_frames = 0;

  // The waveform for the current view, already summarised to two bins per
  // character column. Passed in rather than computed here so render() needs
  // neither the Sample nor the pyramid -- see wave_columns_for() in render.hpp
  // for how many to produce.
  std::vector<ingest::PeakBin> bins;

  WaveView view;
  PanelTab tab = PanelTab::kSample;

  // The chops, ascending and non-overlapping. Empty until something has been
  // chopped, which is the state the wave panel's ruler row falls back to.
  PatternView pattern;

  std::vector<SliceMark> slices;
  std::string chop_algorithm;  // "transient" or "grid n", for the panel

  // The crate: one line per loaded file, in pool order. Enough to list and to
  // choose between, and no more -- the browser (M5.5) draws from this and the
  // pool itself stays on the control side.
  struct FileEntry {
    ingest::FileId id = ingest::kNoFile;
    std::string name;
    std::size_t slices = 0;
    std::size_t frames = 0;
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
  };

  std::vector<FileEntry> files;

  std::array<PadState, rt::kNumPads> pads;
  std::uint8_t selected_pad = 0;

  // What is being previewed, or empty when nothing is.
  //
  // A LABEL RATHER THAN A FLAG, because it is what makes the preview key a
  // TOGGLE rather than a retrigger: pressing space on the thing already sounding
  // stops it, and pressing it on anything else starts that instead. A bool could
  // only answer the first half.
  //
  // Cleared by the frame loop when the sound ends, so the key does not report a
  // preview that finished on its own thirty seconds ago.
  std::string auditioning;

  // What BROWSE is previewing. See PreviewState.
  PreviewState preview;

  bool playing = false;
  std::size_t playhead_frame = 0;
  std::uint8_t playhead_pad = 0;
  float master_peak = 0.0F;

  // The `:` line. When active it takes over the mode line entirely, because a
  // prompt competing for space with a keymap is a prompt you cannot read.
  bool command_active = false;
  std::string command_text;

  // What Tab is offering, if it has been pressed.
  //
  // OPENED BY TAB RATHER THAN SHOWN WHILE TYPING, which is the one real choice
  // in this feature. A menu that appeared on its own would put thirty-three
  // verbs on screen the instant you pressed `:` -- everything matches an empty
  // line -- and then shrink as you typed, which is a lot of movement in exchange
  // for a list you did not ask for. Tab is what a person at a terminal presses
  // when they want to know, and it costs nothing when they do not.
  //
  // CAPTURED, NOT RECOMPUTED. Once open the entries stay as they were and Tab
  // cycles the cursor, because applying the selection changes the line and
  // recomputing from the changed line would give a set of one -- the thing just
  // applied -- and cycling would stop after the first press.
  CompletionState completion;

  // What the last command said. It takes the mode line's whole right-hand side,
  // displacing both the counters and the keymap: an answer to something the
  // player just did outranks a reminder of keys they already pressed, and the
  // counters are back on the next keystroke because that is what clears it.
  //
  // The flag exists because a refusal that looks exactly like a confirmation is
  // a refusal nobody notices.
  std::string message;
  bool message_is_error = false;

  // Engine and device facts for the mode line. An empty audio_api means the
  // interface is running with no device (--no-audio), and says so.
  std::uint32_t engine_rate = 48'000;
  std::uint32_t block_frames = 0;
  std::string audio_api;
  std::size_t active_voices = 0;
  std::size_t max_voices = 0;
  std::uint64_t xruns = 0;
  std::uint64_t dropped = 0;

  [[nodiscard]] bool has_sample() const noexcept { return sample_frames > 0; }
};

}  // namespace tui

#endif  // CRATEDIG_TUI_UI_STATE_HPP
