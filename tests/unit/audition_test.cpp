#include "engine/engine.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// The audition lane's stopping behaviour.
//
// Every case here was a live bug reported by a person using BROWSE, and each one
// is a claim the engine now makes rather than a claim about the interface: what
// the preview key does is a keymap decision, but "there is at most one preview"
// and "the panic key silences it" are properties of the engine and belong where
// they cannot be undone by a keymap.

namespace {

constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kBlock = 512;

[[nodiscard]] engine::Engine::Config test_config() {
  return engine::Engine::Config{
      .sample_rate = kRate, .num_channels = 2, .max_block_frames = kBlock, .seed = 0};
}

[[nodiscard]] std::shared_ptr<const rt::Sample> tone(std::size_t frames) {
  auto sample = std::make_shared<rt::Sample>(kRate, 1, frames);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    sample->mutable_channel(0)[frame] = 0.5F;
  }
  return sample;
}

[[nodiscard]] std::shared_ptr<const rt::PadConfig> preview(std::size_t frames = kRate) {
  return std::make_shared<const rt::PadConfig>(rt::PadConfig{.sample = tone(frames), .pad = 0});
}

// One engine and one block of silence to render into, since none of this cares
// what the samples are -- only how many voices are sounding.
class Harness {
 public:
  Harness() : m_left(kBlock), m_right(kBlock), m_channels{m_left.data(), m_right.data()} {}

  void render() { m_engine.render(std::span<float* const>{m_channels}, kBlock); }

  [[nodiscard]] engine::Engine& engine() noexcept { return m_engine; }

  [[nodiscard]] std::size_t auditions() const noexcept { return m_engine.active_auditions(); }

 private:
  engine::Engine m_engine{test_config()};
  std::vector<float> m_left;
  std::vector<float> m_right;
  std::array<float*, 2> m_channels;
};

}  // namespace

TEST_CASE("an audition replaces the audition rather than stacking on it", "[unit]") {
  // THE BUG AS REPORTED: pressing the preview key twice left two copies playing
  // over each other, and previewing a second file played it over the first.
  // A preview means "let me hear THIS", which has one answer at a time.
  Harness harness;

  REQUIRE(harness.engine().audition(preview()));
  harness.render();
  REQUIRE(harness.auditions() == 1);

  REQUIRE(harness.engine().audition(preview()));
  harness.render();
  CHECK(harness.auditions() == 1);

  // And it stays one however many arrive, rather than one-per-voice until the
  // pool is full. Two voices exist so the old one can FADE while the new one
  // starts; they are not two simultaneous previews.
  for (int again = 0; again < 6; ++again) {
    REQUIRE(harness.engine().audition(preview()));
    harness.render();
  }
  CHECK(harness.auditions() == 1);
}

TEST_CASE("the panic key silences an audition too", "[unit]") {
  // It did not until M5.5. `kStopAll` released every pad voice and left the
  // preview ringing -- and since nothing in the interface called
  // stop_audition() either, there was no way to stop one at all.
  Harness harness;

  REQUIRE(harness.engine().audition(preview()));
  harness.render();
  REQUIRE(harness.auditions() == 1);

  REQUIRE(harness.engine().trigger_pad(rt::PadEvent{.kind = rt::PadEventKind::kStopAll}));
  harness.render();
  CHECK(harness.auditions() == 0);
}

TEST_CASE("stopping an audition leaves the pads alone", "[unit]") {
  // The other direction, and the reason the audition lane is separate at all:
  // stopping a preview must not silence the pattern you are previewing over.
  Harness harness;

  REQUIRE(harness.engine().publish_pad_config(
      std::make_shared<const rt::PadConfig>(rt::PadConfig{.sample = tone(kRate), .pad = 0})));
  harness.render();
  REQUIRE(harness.engine().trigger_pad(
      rt::PadEvent{.pad = 0, .kind = rt::PadEventKind::kNoteOn, .velocity = 1.0F}));
  REQUIRE(harness.engine().audition(preview()));
  harness.render();
  REQUIRE(harness.engine().active_voices() == 1);
  REQUIRE(harness.auditions() == 1);

  REQUIRE(harness.engine().stop_audition());
  harness.render();
  CHECK(harness.auditions() == 0);
  CHECK(harness.engine().active_voices() == 1);  // the pad is still going
}

TEST_CASE("stopping an audition that is not sounding is not an error", "[unit]") {
  // The preview key is a toggle, so this is what the second press does when the
  // first one's sound has already finished. It must be ordinary rather than
  // something a caller has to check for.
  Harness harness;
  CHECK(harness.engine().stop_audition());
  harness.render();
  CHECK(harness.auditions() == 0);
}

TEST_CASE("the audition playhead follows the preview", "[unit]") {
  // What BROWSE draws the marker from. Separate from the pad playhead, which
  // packs a pad number alongside the frame and walks only the pad voices -- a
  // preview has no pad, which is the whole point of the lane.
  Harness harness;
  CHECK(harness.engine().audition_playhead() == engine::Engine::kNoAuditionPlayhead);

  REQUIRE(harness.engine().audition(preview(kRate)));
  harness.render();
  const std::uint64_t first = harness.engine().audition_playhead();
  REQUIRE(first != engine::Engine::kNoAuditionPlayhead);

  harness.render();
  const std::uint64_t second = harness.engine().audition_playhead();
  CHECK(second > first);

  // In frames of the SAMPLE, so a block of 512 moves it by about a block.
  // Approximate rather than exact: the position is 32.32 fixed point and the
  // voice may have started mid-block.
  CHECK(second - first == kBlock);
}

TEST_CASE("the audition playhead goes away when the preview does", "[unit]") {
  // Otherwise BROWSE would leave a marker crawling across a file nobody is
  // hearing.
  Harness harness;
  REQUIRE(harness.engine().audition(preview(kRate)));
  harness.render();
  REQUIRE(harness.engine().audition_playhead() != engine::Engine::kNoAuditionPlayhead);

  REQUIRE(harness.engine().stop_audition());
  // Long enough for the release to finish.
  for (int block = 0; block < 200; ++block) {
    harness.render();
  }
  CHECK(harness.engine().active_auditions() == 0);
  CHECK(harness.engine().audition_playhead() == engine::Engine::kNoAuditionPlayhead);
}

TEST_CASE("a preview that runs out stops reporting a position", "[unit]") {
  // The short-file case: nothing stopped it, it simply ended.
  Harness harness;
  REQUIRE(harness.engine().audition(preview(kBlock)));
  harness.render();
  for (int block = 0; block < 200; ++block) {
    harness.render();
  }
  CHECK(harness.engine().active_auditions() == 0);
  CHECK(harness.engine().audition_playhead() == engine::Engine::kNoAuditionPlayhead);
}
