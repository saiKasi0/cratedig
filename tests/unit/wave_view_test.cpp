// Zoom and scroll, as arithmetic.
//
// WaveView has no UI in it on purpose: navigating a waveform is a set of
// properties -- reversible, bounded, never empty -- and properties are much
// easier to state about a struct than about a screen.

#include "tui/ui_state.hpp"

#include <cstddef>
#include <limits>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::size_t kTotal = 1U << 20;  // 1 048 576 frames, ~21 s at 48 kHz

[[nodiscard]] tui::WaveView fitted(std::size_t total = kTotal) {
  tui::WaveView view;
  view.fit(total);
  return view;
}

}  // namespace

TEST_CASE("fit shows the whole sample", "[unit]") {
  const tui::WaveView view = fitted();
  CHECK(view.first_frame == 0);
  CHECK(view.frames_visible == kTotal);
  CHECK(view.last_frame() == kTotal);
}

TEST_CASE("an empty sample is a safe view, not a division by zero", "[unit]") {
  tui::WaveView view = fitted(0);
  CHECK(view.frames_visible == 0);
  CHECK(view.column_of(0, 80) == 80);  // nothing is on screen

  // None of these may do anything strange with no frames to navigate.
  view.scroll_by(1'000, 0);
  view.zoom_by(0.5, 0);
  view.zoom_by(4.0, 0);
  CHECK(view.first_frame == 0);
  CHECK(view.frames_visible == 0);
}

TEST_CASE("the view never inverts, empties, or hangs past the end", "[unit]") {
  // Clamp is called after every mutation, so this is the invariant everything
  // else is allowed to assume.
  tui::WaveView view = fitted();

  SECTION("narrower than the floor is widened") {
    view.frames_visible = 1;
    view.clamp(kTotal);
    CHECK(view.frames_visible == tui::WaveView::kMinFramesVisible);
  }

  SECTION("wider than the sample is narrowed") {
    view.frames_visible = kTotal * 4;
    view.clamp(kTotal);
    CHECK(view.frames_visible == kTotal);
  }

  SECTION("a window hanging past the end is pulled back") {
    view.frames_visible = 1'000;
    view.first_frame = kTotal - 10;
    view.clamp(kTotal);
    CHECK(view.last_frame() == kTotal);
    CHECK(view.first_frame == kTotal - 1'000);
  }

  SECTION("clamp is idempotent") {
    view.frames_visible = 12'345;
    view.first_frame = kTotal - 3;
    view.clamp(kTotal);
    const tui::WaveView once = view;
    view.clamp(kTotal);
    CHECK(view == once);
  }

  SECTION("a sample shorter than the floor is still fully visible") {
    tui::WaveView tiny = fitted(4);
    CHECK(tiny.frames_visible == 4);
    CHECK(tiny.first_frame == 0);
  }
}

TEST_CASE("scrolling saturates at both ends", "[unit]") {
  tui::WaveView view = fitted();
  view.frames_visible = 1'000;
  view.first_frame = 500'000;
  view.clamp(kTotal);

  SECTION("left") {
    view.scroll_by(-1'000'000'000, kTotal);
    CHECK(view.first_frame == 0);
    CHECK(view.frames_visible == 1'000);
  }

  SECTION("right") {
    view.scroll_by(1'000'000'000, kTotal);
    CHECK(view.last_frame() == kTotal);
    CHECK(view.frames_visible == 1'000);
  }

  SECTION("the most negative offset does not wrap") {
    // Negating std::ptrdiff_t's minimum is undefined; the implementation goes
    // through the unsigned type for exactly this input.
    view.scroll_by(std::numeric_limits<std::ptrdiff_t>::min(), kTotal);
    CHECK(view.first_frame == 0);
  }
}

TEST_CASE("zoom is reversible", "[unit]") {
  // THE property. Anchoring zoom anywhere but the centre makes scroll-then-zoom
  // drift, and the drift only becomes obvious once it has already lost your
  // place in the file.
  tui::WaveView view = fitted();
  view.first_frame = kTotal / 4;
  view.frames_visible = kTotal / 2;
  view.clamp(kTotal);
  const tui::WaveView before = view;

  for (int step = 0; step < 8; ++step) {
    view.zoom_by(0.5, kTotal);
  }
  CHECK(view != before);
  for (int step = 0; step < 8; ++step) {
    view.zoom_by(2.0, kTotal);
  }
  CHECK(view == before);
}

TEST_CASE("zoom holds the centre of the view", "[unit]") {
  tui::WaveView view = fitted();
  view.first_frame = 300'000;
  view.frames_visible = 200'000;
  view.clamp(kTotal);

  const std::size_t centre = view.first_frame + (view.frames_visible / 2);
  view.zoom_by(0.25, kTotal);

  const std::size_t new_centre = view.first_frame + (view.frames_visible / 2);
  INFO("centre was " << centre << ", now " << new_centre);
  CHECK(new_centre >= centre - 1);
  CHECK(new_centre <= centre + 1);
  CHECK(view.contains(centre));
}

TEST_CASE("zoom stops at both limits", "[unit]") {
  tui::WaveView view = fitted();

  SECTION("in, at the frame floor") {
    for (int step = 0; step < 40; ++step) {
      view.zoom_by(0.5, kTotal);
    }
    CHECK(view.frames_visible == tui::WaveView::kMinFramesVisible);
  }

  SECTION("out, at the whole file") {
    view.frames_visible = 64;
    view.first_frame = 1'000;
    view.clamp(kTotal);
    for (int step = 0; step < 40; ++step) {
      view.zoom_by(2.0, kTotal);
    }
    CHECK(view.frames_visible == kTotal);
    CHECK(view.first_frame == 0);
  }

  SECTION("a zero or negative factor is ignored rather than obeyed") {
    const tui::WaveView before = view;
    view.zoom_by(0.0, kTotal);
    view.zoom_by(-2.0, kTotal);
    CHECK(view == before);
  }
}

TEST_CASE("column_of places a frame under the right column", "[unit]") {
  tui::WaveView view;
  view.first_frame = 1'000;
  view.frames_visible = 800;

  constexpr std::size_t kColumns = 80;
  CHECK(view.column_of(1'000, kColumns) == 0);
  CHECK(view.column_of(1'400, kColumns) == 40);
  CHECK(view.column_of(1'799, kColumns) == kColumns - 1);

  // Off screen either way reports "not on screen" rather than an edge column,
  // so a playhead before the view does not stick to column zero.
  CHECK(view.column_of(999, kColumns) == kColumns);
  CHECK(view.column_of(1'800, kColumns) == kColumns);
  CHECK(view.column_of(1'400, 0) == 0);
}
