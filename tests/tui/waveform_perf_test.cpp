// The M2 acceptance criterion, measured: "a 5-minute file scrolls at full frame
// rate".
//
// This is the test that decides whether the peak pyramid earned its existence.
// Without it, drawing one frame of a five-minute file means scanning 14.4
// million frames per column; with it, about five bins per column at any zoom.
// The difference is four orders of magnitude, and the only honest way to state
// it is to measure it.
//
// It NEVER SKIPS. The five-minute buffer is synthesised here rather than loaded,
// so the acceptance holds on a machine with no network, no fixtures and no
// sound card. A [fixture]-tagged variant runs the same measurement on the real
// long_form_drums.flac, which additionally exercises decode and the pyramid
// build over material nobody wrote to be convenient.

#include "ingest/decoder.hpp"
#include "ingest/peak_pyramid.hpp"
#include "rt/sample.hpp"
#include "tui/render.hpp"
#include "tui/ui_state.hpp"
#include "tui/waveform.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

// Under a sanitizer every load and store goes through instrumentation, so a
// wall-clock number measures the sanitizer rather than the code. The work still
// runs -- that is how the perf path gets checked for out-of-bounds reads -- but
// the budget is only asserted where it means something.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define CRATEDIG_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CRATEDIG_SANITIZED 1
#endif
#if defined(CRATEDIG_SANITIZED)
constexpr bool kTimingIsMeaningful = false;
#else
constexpr bool kTimingIsMeaningful = true;
#endif

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CRATEDIG_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define CRATEDIG_TSAN 1
#endif
#if defined(CRATEDIG_TSAN)
constexpr bool kUnderThreadSanitizer = true;
#else
constexpr bool kUnderThreadSanitizer = false;
#endif

constexpr std::uint32_t kRate = 48'000;

// Five minutes where the timing means something, thirty seconds under a
// sanitizer. The sanitizer run is not measuring anything -- it is checking the
// perf path for out-of-bounds reads -- and thirty seconds still builds eight
// pyramid levels and exercises every branch. At full length it took TSan from
// five seconds to eighty, which is a lot of CI time to spend on coverage
// peak_pyramid_test already provides.
constexpr std::size_t kMeasuredSeconds = kTimingIsMeaningful ? 300 : 30;
constexpr std::size_t kBufferFrames = kMeasuredSeconds * static_cast<std::size_t>(kRate);

// The design grid is 100 columns, so 98 of waveform and 196 peak bins.
constexpr std::size_t kColumns = 98;

// Frames per measured pass. Enough that a single scheduling hiccup cannot move
// the mean, few enough that the whole test stays under a couple of seconds.
constexpr std::size_t kFramesPerPass = kTimingIsMeaningful ? 200 : 20;

// 60 fps is 16.67 ms per frame for everything; the waveform is one panel of one
// frame, and the audio thread has a hard deadline of its own, so a display that
// ate a whole frame's worth of a core to redraw itself would be meeting this
// criterion and failing the product.
//
// The number is chosen from measurement, not from taste. On an M-series Mac,
// worst case over 200 scrolling frames at full zoom-out:
//
//     correct                       0.004 ms
//     level selection pinned to 0   0.089 ms   (correct, ~20x slower)
//     no pyramid, raw frames        6.84  ms   (what this budget catches)
//
// So 2 ms sits about 500x above the real figure and about 3x below the
// no-pyramid figure. It discriminates the regression that matters -- somebody
// "simplifying" away the pyramid -- and will not fire because a CI runner was
// busy. It deliberately does NOT catch a level-selection regression: pinning to
// level 0 is still correct and still 250x inside budget, and choosing a level
// COARSER than the column is caught by peak_pyramid_test's containment tests
// instead, which is where a correctness question belongs.
constexpr double kBudgetMillisecondsPerFrame = 2.0;

[[nodiscard]] rt::Sample synth_buffer() {
  rt::Sample sample{kRate, 2, kBufferFrames};
  for (std::uint16_t channel = 0; channel < 2; ++channel) {
    const std::span<float> data = sample.mutable_channel(channel);
    // An integer recurrence: no libm on 28.8 million samples, and identical on
    // every platform. The pattern is a hit every 0.25 s with a decaying tail,
    // so the peaks the display has to preserve are genuinely sparse.
    std::uint32_t state = 0x1234'5678U + channel;
    for (std::size_t frame = 0; frame < kBufferFrames; ++frame) {
      state = (state * 1'664'525U) + 1'013'904'223U;
      const std::size_t since_hit = frame % (kRate / 4);
      const auto decay = static_cast<float>(1.0 - (static_cast<double>(since_hit) / (kRate / 4.0)));
      const auto noise =
          static_cast<float>(static_cast<std::int32_t>(state >> 8U) % 2'000) / 2'000.0F;
      data[frame] = decay * decay * noise * 0.9F;
    }
  }
  return sample;
}

struct PassResult {
  double mean_milliseconds = 0.0;
  double worst_milliseconds = 0.0;
  bool drew_something = false;
};

// One measured pass: scroll across the file at a fixed zoom, redrawing every
// frame exactly as the app does -- summarise into per-column bins, then turn
// those into braille rows.
[[nodiscard]] PassResult scroll_pass(const rt::Sample& sample, const ingest::PeakPyramid& pyramid,
                                     std::size_t frames_visible) {
  tui::WaveView view;
  view.frames_visible = frames_visible;
  view.first_frame = 0;
  view.clamp(sample.num_frames());

  const std::size_t step = std::max<std::size_t>(sample.num_frames() / kFramesPerPass, 1);
  std::vector<ingest::PeakBin> bins(tui::bins_for_columns(kColumns));

  PassResult result;
  double total = 0.0;
  for (std::size_t frame = 0; frame < kFramesPerPass; ++frame) {
    const auto started = std::chrono::steady_clock::now();

    pyramid.summarize(sample, 0, view.first_frame, view.frames_visible, bins);
    const std::vector<std::string> rows =
        tui::waveform_rows(bins, tui::WaveformGeometry{.rows = 5, .gain = 1.0F});

    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    total += elapsed;
    result.worst_milliseconds = std::max(result.worst_milliseconds, elapsed);
    // Anti-vacuity: a summarize() that filled every bin with zeroes would meet
    // any frame budget effortlessly.
    result.drew_something =
        result.drew_something || std::any_of(rows.begin(), rows.end(), [](const std::string& row) {
          return row.find_first_not_of(' ') != std::string::npos;
        });

    view.scroll_by(static_cast<std::ptrdiff_t>(step), sample.num_frames());
  }
  result.mean_milliseconds = total / static_cast<double>(kFramesPerPass);
  return result;
}

void measure(const rt::Sample& sample, const ingest::PeakPyramid& pyramid, const char* what) {
  // Three zooms, because they take different paths: the whole file uses the
  // coarsest pyramid level, a second uses a middle one, and 5 ms is below the
  // base bin and reads raw frames.
  const std::vector<std::pair<const char*, std::size_t>> zooms{
      {"whole file", sample.num_frames()},
      {"one second", kRate},
      {"five milliseconds", kRate / 200},
  };

  for (const auto& [name, frames_visible] : zooms) {
    const PassResult pass = scroll_pass(sample, pyramid, frames_visible);
    INFO(what << " @ " << name << ": mean " << pass.mean_milliseconds << " ms, worst "
              << pass.worst_milliseconds << " ms per frame over " << kFramesPerPass << " frames");
    CHECK(pass.drew_something);
    if (kTimingIsMeaningful) {
      CHECK(pass.mean_milliseconds < kBudgetMillisecondsPerFrame);
    }
  }
}

}  // namespace

TEST_CASE("a five-minute file scrolls inside the frame budget", "[tui]") {
  const auto build_started = std::chrono::steady_clock::now();
  const rt::Sample sample = synth_buffer();
  const ingest::PeakPyramid pyramid = ingest::PeakPyramid::build(sample);
  const auto build_milliseconds =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build_started)
          .count();

  REQUIRE(sample.num_frames() == kBufferFrames);
  REQUIRE(pyramid.num_levels() > 5);
  INFO("synthesise + pyramid build: " << build_milliseconds << " ms for " << kMeasuredSeconds
                                      << " s stereo");

  // Startup cost, paid once at load. A second is already noticeable when opening
  // a file; ten would be a bug. Not asserted under a sanitizer for the same
  // reason as the frame budget.
  if (kTimingIsMeaningful) {
    CHECK(build_milliseconds < 10'000.0);
  }

  measure(sample, pyramid, "synthetic buffer");
}

TEST_CASE("the long-form fixture scrolls inside the frame budget", "[tui][fixture]") {
  if (kUnderThreadSanitizer) {
    // Decoding 5.5 minutes of FLAC and building a pyramid over 15.9 million
    // frames costs 28 seconds under TSan, and there is not a thread anywhere in
    // this test for it to inspect. The synthetic case above covers the same code
    // path, and asan/ubsan still run this one over real material.
    SKIP("skipped under TSan: single-threaded, and it costs 28 s of instrumented decode");
  }

  const std::filesystem::path path =
      std::filesystem::path{CRATEDIG_STARTER_PACK_DIR} / "long_form_drums.flac";
  if (!std::filesystem::exists(path)) {
    SKIP("missing " << path.string() << " — run scripts/fetch_starter_pack.sh");
  }

  const ingest::SampleLoad load = ingest::load_sample(path, kRate);
  REQUIRE(load.ok());
  REQUIRE(load.sample->num_frames() >
          static_cast<std::size_t>(kRate) * 3 * 60);  // the >3 min the manifest promises

  const ingest::PeakPyramid pyramid = ingest::PeakPyramid::build(*load.sample);
  measure(*load.sample, pyramid, "long_form_drums.flac");
}
