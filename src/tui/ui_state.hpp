#ifndef CRATEDIG_TUI_UI_STATE_HPP
#define CRATEDIG_TUI_UI_STATE_HPP

#include "ingest/peak_pyramid.hpp"
#include "rt/pad_event.hpp"

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

  // Which slice this pad plays, or none. Displayed rather than acted on -- the
  // engine already has the config.
  bool has_slice = false;
  std::size_t slice_index = 0;

  // How long ago the pad was hit, in seconds, and how hard. Straight from
  // engine::PadGlow; `glow_seconds` is meaningless unless `triggered`.
  bool triggered = false;
  float glow_seconds = 0.0F;
  float glow_velocity = 0.0F;
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

// Which screen is up.
//
// PERFORM and EDIT are two pure functions of this same struct, chosen here. The
// alternative -- two UiStates -- would mean the pad levels, the message line and
// the transport all existed twice and had to be kept in step.
enum class Screen : std::uint8_t {
  kPerform = 0,
  kEdit,
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

  // Which pad plays this slice, if any. Shown in the header and in the table's
  // `pad` column, because "which key makes this sound" is the question EDIT is
  // usually being asked in service of.
  std::uint8_t pad = 0;
  bool pad_known = false;

  // How many boundary nudges are undoable. Shown rather than acted on -- `u`
  // does the acting, and a count is what tells you whether it will.
  std::size_t undo_depth = 0;

  // Whether the boundaries were snapped to zero crossings when this chop was
  // made. Reported rather than obeyed: the snap already happened, and saying so
  // is what makes "start snapped -3 smp" and "start free" different statements.
  bool snap_enabled = true;
};

struct UiState {
  std::string version;

  Screen screen = Screen::kPerform;
  EditState edit;

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
  std::vector<SliceMark> slices;
  std::string chop_algorithm;  // "transient" or "grid n", for the panel

  std::array<PadState, rt::kNumPads> pads;
  std::uint8_t selected_pad = 0;

  bool playing = false;
  std::size_t playhead_frame = 0;
  std::uint8_t playhead_pad = 0;
  float master_peak = 0.0F;

  // The `:` line. When active it takes over the mode line entirely, because a
  // prompt competing for space with a keymap is a prompt you cannot read.
  bool command_active = false;
  std::string command_text;

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
