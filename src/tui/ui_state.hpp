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
};

struct UiState {
  std::string version;

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

  std::array<PadState, rt::kNumPads> pads;
  std::uint8_t selected_pad = 0;

  bool playing = false;
  std::size_t playhead_frame = 0;
  std::uint8_t playhead_pad = 0;
  float master_peak = 0.0F;

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
