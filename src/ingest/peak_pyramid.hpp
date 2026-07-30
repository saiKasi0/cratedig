#ifndef CRATEDIG_INGEST_PEAK_PYRAMID_HPP
#define CRATEDIG_INGEST_PEAK_PYRAMID_HPP

#include "rt/sample.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ingest {

// The extremes of a range of frames -- deliberately not an average.
//
// A waveform drawn from averages loses every transient: a single-sample kick
// spike averaged over 75 000 frames is indistinguishable from silence. Min/max
// is what makes a drum hit visible when the whole five-minute file is on screen,
// which is the entire point of the display.
struct PeakBin {
  float min = 0.0F;
  float max = 0.0F;
};

// A multi-resolution min/max summary of one Sample.
//
// Built on a worker (or, for now, on the control thread at load) and thereafter
// read-only. It is an ACCELERATION STRUCTURE, never a source of approximation
// error the caller cannot reason about:
//
//   - Zoomed in far enough that a column spans fewer frames than one base bin,
//     summarize() reads the sample directly and is exact.
//   - Zoomed out, a column's reported range is a SUPERSET of the true range over
//     that column's frames, over-reporting by at most one bin -- and the level is
//     chosen so one bin is never wider than one column. A transient can therefore
//     smear sideways by less than a character, but it can never disappear.
//
// The asymmetry is deliberate. Under-reporting hides a peak, which is a lie about
// the audio; over-reporting draws it one column wide when it was half a column,
// which nobody can see at 4 dots per character.
class PeakPyramid {
 public:
  // 256 frames is ~5.3 ms at 48 kHz -- comfortably shorter than any transient
  // worth seeing, and it keeps the whole structure at ~1.2 MB for a five-minute
  // stereo file (~0.8% of the 115 MB the samples themselves occupy).
  static constexpr std::size_t kBaseBinFrames = 256;

  // Each level summarises four bins of the level below. With this ratio, the
  // level whose bins fit inside a column is never more than 4x finer than the
  // column, so summarize() reads at most ~5 bins per column at ANY zoom. That
  // bound, not the memory, is why the ratio is small.
  static constexpr std::size_t kLevelRatio = 4;

  PeakPyramid() = default;

  // Allocates. Never call this from the audio thread.
  [[nodiscard]] static PeakPyramid build(const rt::Sample& sample);

  [[nodiscard]] std::uint16_t num_channels() const noexcept { return m_num_channels; }

  [[nodiscard]] std::size_t num_frames() const noexcept { return m_num_frames; }

  [[nodiscard]] std::size_t num_levels() const noexcept { return m_levels.size(); }

  [[nodiscard]] bool empty() const noexcept { return m_levels.empty(); }

  // Frames summarised by one bin at `level`. Level 0 is kBaseBinFrames.
  [[nodiscard]] std::size_t bin_frames(std::size_t level) const noexcept;

  [[nodiscard]] std::size_t num_bins(std::size_t level) const noexcept;

  // One channel's bins at one level, for tests and for callers that want to walk
  // a level directly.
  [[nodiscard]] std::span<const PeakBin> bins(std::size_t level,
                                              std::uint16_t channel) const noexcept;

  // Fills `out` with one bin per column, together covering
  // [first_frame, first_frame + frame_count).
  //
  // `sample` must be the one this pyramid was built from -- the pyramid does not
  // own it, because a display holding a second reference to a 115 MB buffer is
  // not a thing to do by accident. Columns that fall entirely outside the sample
  // are filled with {0, 0}.
  //
  // Allocation-free: it is called on every frame of the UI.
  void summarize(const rt::Sample& sample, std::uint16_t channel, std::size_t first_frame,
                 std::size_t frame_count, std::span<PeakBin> out) const noexcept;

 private:
  struct Level {
    std::size_t bin_frames = 0;
    std::size_t num_bins = 0;
    // Channel-major: channel c occupies [c * num_bins, (c + 1) * num_bins).
    std::vector<PeakBin> bins;
  };

  // Largest level whose bins fit within `frames_per_column`, or num_levels()
  // when even level 0 is too coarse (the caller then reads raw frames).
  [[nodiscard]] std::size_t level_for(std::size_t frames_per_column) const noexcept;

  std::uint16_t m_num_channels = 0;
  std::size_t m_num_frames = 0;
  std::vector<Level> m_levels;
};

}  // namespace ingest

#endif  // CRATEDIG_INGEST_PEAK_PYRAMID_HPP
