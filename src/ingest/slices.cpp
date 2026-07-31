#include "ingest/slices.hpp"

#include "ingest/onset.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ingest {
namespace {

// The mono sum at one frame, computed on demand.
//
// On demand rather than by building a whole downmix buffer: the snap looks at a
// few hundred frames around each boundary, and materialising a copy of a
// five-minute file to read 0.01% of it is not a trade worth making.
[[nodiscard]] float mono_at(const rt::Sample& sample, std::size_t frame) {
  const std::uint16_t channels = sample.num_channels();
  if (channels == 0 || frame >= sample.num_frames()) {
    return 0.0F;
  }
  float sum = 0.0F;
  for (std::uint16_t channel = 0; channel < channels; ++channel) {
    sum += sample.channel(channel)[frame];
  }
  return sum / static_cast<float>(channels);
}

// True when the signal changes sign between frame-1 and frame.
//
// Zero itself counts as a crossing: a boundary placed exactly on a zero sample
// is already what the snap is looking for.
[[nodiscard]] bool crosses_at(const rt::Sample& sample, std::size_t frame) {
  if (frame == 0 || frame >= sample.num_frames()) {
    return false;
  }
  const float previous = mono_at(sample, frame - 1);
  const float current = mono_at(sample, frame);
  if (current == 0.0F) {
    return true;
  }
  return (previous < 0.0F && current > 0.0F) || (previous > 0.0F && current < 0.0F);
}

// Applies the snap to one boundary and records how far it moved.
void snap_boundary(const rt::Sample& sample, const SnapParams& params, std::size_t& frame,
                   std::ptrdiff_t& moved) {
  if (!params.enabled) {
    moved = 0;
    return;
  }
  const std::size_t snapped = snap_to_zero_crossing(sample, frame, params.radius);
  moved = static_cast<std::ptrdiff_t>(snapped) - static_cast<std::ptrdiff_t>(frame);
  frame = snapped;
}

// Turns a list of ascending boundaries into slices, snapping each one.
//
// The boundaries are snapped BEFORE the slices are formed, so adjacent slices
// share a boundary exactly and there is no gap or overlap between them. Snapping
// each slice's own start and end independently would move the same instant to
// two different places.
[[nodiscard]] std::vector<Slice> slices_from_boundaries(const rt::Sample& sample,
                                                        std::vector<std::size_t> boundaries,
                                                        const SnapParams& snap) {
  std::vector<Slice> slices;
  if (boundaries.size() < 2) {
    return slices;
  }

  std::vector<std::ptrdiff_t> moved(boundaries.size(), 0);
  for (std::size_t index = 0; index < boundaries.size(); ++index) {
    snap_boundary(sample, snap, boundaries[index], moved[index]);
  }

  slices.reserve(boundaries.size() - 1);
  for (std::size_t index = 0; index + 1 < boundaries.size(); ++index) {
    // A snap can in principle push two boundaries onto each other; the detector's
    // refractory period makes that impossible in practice, but a zero-length
    // slice would be an un-triggerable pad, so it is dropped rather than shipped.
    if (boundaries[index + 1] <= boundaries[index]) {
      continue;
    }
    slices.push_back(Slice{.start_frame = boundaries[index],
                           .end_frame = boundaries[index + 1],
                           .start_snap = moved[index],
                           .end_snap = moved[index + 1]});
  }
  return slices;
}

}  // namespace

std::size_t snap_to_zero_crossing(const rt::Sample& sample, std::size_t at, std::size_t radius) {
  const std::size_t frames = sample.num_frames();
  if (frames == 0 || radius == 0) {
    return at;
  }
  if (at >= frames) {
    return frames;
  }
  if (crosses_at(sample, at)) {
    return at;
  }

  // Outward from `at`, so the FIRST crossing found is the nearest one and the
  // boundary moves as little as possible. Backwards is checked before forwards
  // at equal distance: moving a slice start earlier keeps the whole transient,
  // while moving it later begins to clip the attack.
  for (std::size_t distance = 1; distance <= radius; ++distance) {
    if (at >= distance && crosses_at(sample, at - distance)) {
      return at - distance;
    }
    if (at + distance < frames && crosses_at(sample, at + distance)) {
      return at + distance;
    }
  }
  return at;
}

SliceSet chop_transient(const rt::Sample& sample, const OnsetParams& onset,
                        const SnapParams& snap) {
  SliceSet set;
  set.algorithm = ChopAlgorithm::kTransient;
  if (sample.empty()) {
    return set;
  }

  const OnsetResult onsets = detect_onsets(sample, onset);
  if (onsets.frames.empty()) {
    // No transients anywhere -- a pad, a drone, a field recording. One slice
    // covering everything is more useful than none, and it is what makes
    // `:chop transient` on unsuitable material behave like a no-op rather than
    // like a failure.
    set.slices.push_back(Slice{.start_frame = 0, .end_frame = sample.num_frames()});
    return set;
  }

  std::vector<std::size_t> boundaries = onsets.frames;
  boundaries.push_back(sample.num_frames());  // the last slice runs to the end
  set.slices = slices_from_boundaries(sample, std::move(boundaries), snap);
  return set;
}

SliceSet chop_grid(const rt::Sample& sample, std::size_t parts, const SnapParams& snap) {
  SliceSet set;
  set.algorithm = ChopAlgorithm::kGrid;
  if (sample.empty() || parts == 0) {
    return set;
  }

  const std::size_t frames = sample.num_frames();
  std::vector<std::size_t> boundaries;
  boundaries.reserve(parts + 1);
  for (std::size_t part = 0; part <= parts; ++part) {
    // Computed from the part index rather than accumulated, so rounding cannot
    // drift and the last boundary is exactly the end of the sample.
    boundaries.push_back((frames * part) / parts);
  }
  set.slices = slices_from_boundaries(sample, std::move(boundaries), snap);
  return set;
}

}  // namespace ingest
