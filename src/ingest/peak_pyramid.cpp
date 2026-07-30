#include "ingest/peak_pyramid.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ingest {
namespace {

// The extremes of raw frames [first, last). Exact by construction -- this is the
// path taken when the view is zoomed in past one base bin, and it is also what
// level 0 is built from.
[[nodiscard]] PeakBin scan_frames(std::span<const float> data, std::size_t first,
                                  std::size_t last) noexcept {
  if (first >= last || first >= data.size()) {
    return PeakBin{};
  }
  last = std::min(last, data.size());

  PeakBin bin{data[first], data[first]};
  for (std::size_t index = first + 1; index < last; ++index) {
    bin.min = std::min(bin.min, data[index]);
    bin.max = std::max(bin.max, data[index]);
  }
  return bin;
}

[[nodiscard]] PeakBin merge(const PeakBin& left, const PeakBin& right) noexcept {
  return PeakBin{std::min(left.min, right.min), std::max(left.max, right.max)};
}

[[nodiscard]] std::size_t divide_rounding_up(std::size_t value, std::size_t divisor) noexcept {
  return (value + divisor - 1) / divisor;
}

}  // namespace

PeakPyramid PeakPyramid::build(const rt::Sample& sample) {
  PeakPyramid pyramid;
  pyramid.m_num_channels = sample.num_channels();
  pyramid.m_num_frames = sample.num_frames();

  if (sample.empty() || sample.num_channels() == 0) {
    return pyramid;  // a valid, empty pyramid: summarize() fills zeros
  }

  const auto channels = static_cast<std::size_t>(sample.num_channels());

  // Level 0, straight from the frames.
  {
    Level level;
    level.bin_frames = kBaseBinFrames;
    level.num_bins = divide_rounding_up(sample.num_frames(), kBaseBinFrames);
    level.bins.resize(level.num_bins * channels);

    for (std::size_t channel = 0; channel < channels; ++channel) {
      const std::span<const float> data = sample.channel(static_cast<std::uint16_t>(channel));
      for (std::size_t bin = 0; bin < level.num_bins; ++bin) {
        const std::size_t first = bin * kBaseBinFrames;
        level.bins[(channel * level.num_bins) + bin] =
            scan_frames(data, first, first + kBaseBinFrames);
      }
    }
    pyramid.m_levels.push_back(std::move(level));
  }

  // Each further level merges kLevelRatio bins of the one below. Building from
  // the previous level rather than re-scanning the sample keeps the whole
  // construction linear in the number of frames.
  while (pyramid.m_levels.back().num_bins > 1) {
    const Level& below = pyramid.m_levels.back();

    Level level;
    level.bin_frames = below.bin_frames * kLevelRatio;
    level.num_bins = divide_rounding_up(below.num_bins, kLevelRatio);
    level.bins.resize(level.num_bins * channels);

    for (std::size_t channel = 0; channel < channels; ++channel) {
      for (std::size_t bin = 0; bin < level.num_bins; ++bin) {
        const std::size_t first = bin * kLevelRatio;
        const std::size_t last = std::min(first + kLevelRatio, below.num_bins);

        PeakBin merged = below.bins[(channel * below.num_bins) + first];
        for (std::size_t source = first + 1; source < last; ++source) {
          merged = merge(merged, below.bins[(channel * below.num_bins) + source]);
        }
        level.bins[(channel * level.num_bins) + bin] = merged;
      }
    }
    pyramid.m_levels.push_back(std::move(level));
  }

  return pyramid;
}

std::size_t PeakPyramid::bin_frames(std::size_t level) const noexcept {
  return level < m_levels.size() ? m_levels[level].bin_frames : 0;
}

std::size_t PeakPyramid::num_bins(std::size_t level) const noexcept {
  return level < m_levels.size() ? m_levels[level].num_bins : 0;
}

std::span<const PeakBin> PeakPyramid::bins(std::size_t level,
                                           std::uint16_t channel) const noexcept {
  if (level >= m_levels.size() || channel >= m_num_channels) {
    return {};
  }
  const Level& chosen = m_levels[level];
  return std::span<const PeakBin>{chosen.bins}.subspan(
      static_cast<std::size_t>(channel) * chosen.num_bins, chosen.num_bins);
}

std::size_t PeakPyramid::level_for(std::size_t frames_per_column) const noexcept {
  std::size_t chosen = m_levels.size();  // "none fits; read raw frames"
  for (std::size_t level = 0; level < m_levels.size(); ++level) {
    if (m_levels[level].bin_frames > frames_per_column) {
      break;  // bin_frames is strictly increasing, so no later level fits either
    }
    chosen = level;
  }
  return chosen;
}

void PeakPyramid::summarize(const rt::Sample& sample, std::uint16_t channel,
                            std::size_t first_frame, std::size_t frame_count,
                            std::span<PeakBin> out) const noexcept {
  if (out.empty()) {
    return;
  }
  std::fill(out.begin(), out.end(), PeakBin{});

  if (m_levels.empty() || channel >= m_num_channels || frame_count == 0) {
    return;
  }
  assert(sample.num_frames() == m_num_frames &&
         "PeakPyramid::summarize: called with a different Sample than it was built from");
  assert(sample.num_channels() == m_num_channels &&
         "PeakPyramid::summarize: channel count changed since build()");

  const auto columns = static_cast<std::uint64_t>(out.size());
  const auto span_frames = static_cast<std::uint64_t>(frame_count);

  // 64-bit throughout: columns * frame_count is ~3e9 for a five-minute file at
  // 200 columns, which is fine here and would not be on a 32-bit size_t.
  const auto frames_per_column = static_cast<std::size_t>(span_frames / columns);
  const std::size_t level = level_for(frames_per_column);
  const bool use_raw_frames = level >= m_levels.size();

  const std::span<const float> data =
      use_raw_frames ? sample.channel(channel) : std::span<const float>{};
  const std::span<const PeakBin> level_bins =
      use_raw_frames ? std::span<const PeakBin>{} : bins(level, channel);
  const std::size_t level_bin_frames = use_raw_frames ? 0 : m_levels[level].bin_frames;

  for (std::uint64_t column = 0; column < columns; ++column) {
    // Integer boundaries rather than a float step: consecutive columns then
    // share an exact edge, so no frame is skipped and none is counted twice
    // just because a float landed on the wrong side of a boundary.
    const auto column_first =
        static_cast<std::size_t>(first_frame + ((column * span_frames) / columns));
    auto column_last =
        static_cast<std::size_t>(first_frame + (((column + 1) * span_frames) / columns));

    if (column_first >= m_num_frames) {
      continue;  // past the end of the sample: stays {0, 0}
    }

    // Zoomed in past one frame per column, the integer boundaries collapse and
    // every column but the last would come out empty -- a one-frame sample would
    // draw itself at the right-hand edge and nowhere else. Holding the frame
    // under the column is what "zoom to sample level" is supposed to look like,
    // and M3's zero-crossing nudging depends on it.
    if (column_last <= column_first) {
      column_last = column_first + 1;
    }
    const std::size_t clamped_last = std::min(column_last, m_num_frames);

    if (use_raw_frames) {
      out[static_cast<std::size_t>(column)] = scan_frames(data, column_first, clamped_last);
      continue;
    }

    // Every bin that OVERLAPS the column, not just those contained by it. That
    // is what makes the result a superset of the truth: a transient sitting in a
    // bin that straddles the boundary is reported by both neighbours rather than
    // by neither.
    const std::size_t first_bin = column_first / level_bin_frames;
    const std::size_t last_bin =
        std::min((clamped_last - 1) / level_bin_frames, level_bins.size() - 1);

    PeakBin merged = level_bins[first_bin];
    for (std::size_t bin = first_bin + 1; bin <= last_bin; ++bin) {
      merged = merge(merged, level_bins[bin]);
    }
    out[static_cast<std::size_t>(column)] = merged;
  }
}

}  // namespace ingest
