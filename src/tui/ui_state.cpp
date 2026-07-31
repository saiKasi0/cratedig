#include "tui/ui_state.hpp"

#include <algorithm>
#include <cstddef>

namespace tui {
namespace {

// Everything below works in unsigned frame counts, where "subtract and clamp"
// is the operation that silently wraps if written the obvious way. One helper,
// used everywhere, rather than a saturating subtraction open-coded five times.
[[nodiscard]] std::size_t subtract_saturating(std::size_t value, std::size_t amount) noexcept {
  return amount > value ? 0 : value - amount;
}

}  // namespace

float glow_intensity(const PadState& pad) noexcept {
  if (!pad.triggered || pad.glow_seconds >= kGlowSeconds) {
    return 0.0F;
  }
  // Linear decay, scaled by how hard the pad was hit. Linear rather than
  // exponential for the same reason the meters are: on four discrete brightness
  // steps the curve is not distinguishable, and this runs every frame.
  const float remaining = 1.0F - (pad.glow_seconds / kGlowSeconds);
  return std::clamp(remaining * pad.glow_velocity, 0.0F, 1.0F);
}

void WaveView::fit(std::size_t total_frames) noexcept {
  first_frame = 0;
  frames_visible = total_frames;
  clamp(total_frames);
}

void WaveView::clamp(std::size_t total_frames) noexcept {
  if (total_frames == 0) {
    first_frame = 0;
    frames_visible = 0;
    return;
  }

  // A view narrower than the floor, or wider than the sample, is meaningless
  // rather than merely odd: both make column arithmetic divide by something it
  // should not.
  frames_visible =
      std::clamp(frames_visible, std::min(kMinFramesVisible, total_frames), total_frames);

  // Anchor the right edge at the end rather than letting the window hang past
  // it. Scrolling to the end and then zooming out should fill the panel with
  // audio, not with half a panel of nothing.
  if (first_frame + frames_visible > total_frames) {
    first_frame = subtract_saturating(total_frames, frames_visible);
  }
}

void WaveView::scroll_by(std::ptrdiff_t frames, std::size_t total_frames) noexcept {
  if (frames < 0) {
    // -frames on the most negative ptrdiff_t is undefined; going through the
    // unsigned type first is well defined for every input.
    const auto magnitude = static_cast<std::size_t>(-(frames + 1)) + 1;
    first_frame = subtract_saturating(first_frame, magnitude);
  } else {
    first_frame += static_cast<std::size_t>(frames);
  }
  clamp(total_frames);
}

void WaveView::zoom_by(double factor, std::size_t total_frames) noexcept {
  if (total_frames == 0 || factor <= 0.0) {
    return;
  }

  // The centre is computed and restored in frames, so the operation is an exact
  // inverse of itself apart from one frame of integer rounding -- which is what
  // makes zoom in / zoom out land back where it started.
  const std::size_t centre = first_frame + (frames_visible / 2);

  const double scaled = static_cast<double>(frames_visible) * factor;
  const auto wanted =
      static_cast<std::size_t>(std::clamp(scaled, 1.0, static_cast<double>(total_frames)));

  frames_visible = std::max(wanted, std::min(kMinFramesVisible, total_frames));
  frames_visible = std::min(frames_visible, total_frames);

  first_frame = subtract_saturating(centre, frames_visible / 2);
  clamp(total_frames);
}

std::size_t WaveView::column_of(std::size_t frame, std::size_t columns) const noexcept {
  if (columns == 0 || frames_visible == 0 || !contains(frame)) {
    return columns;  // "not on screen"
  }
  const std::size_t offset = frame - first_frame;
  const std::size_t column = (offset * columns) / frames_visible;
  return std::min(column, columns - 1);
}

}  // namespace tui
