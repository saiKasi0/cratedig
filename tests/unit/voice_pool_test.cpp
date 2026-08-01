#include "rt/voice_pool.hpp"

#include "rt/garbage_ring.hpp"
#include "rt/pad_config.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint32_t kEngineRate = 48'000;

std::shared_ptr<const rt::Sample> make_sample(std::size_t frames, std::uint16_t channels = 1,
                                              std::uint32_t rate = kEngineRate,
                                              float value = 1.0F) {
  auto sample = std::make_shared<rt::Sample>(rate, channels, frames);
  for (std::uint16_t channel = 0; channel < channels; ++channel) {
    std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      data[frame] = value;
    }
  }
  return sample;
}

// Wraps a sample in a config.
//
// Fades default to ZERO here, not to rt::kDefaultFadeFrames. Most of these cases
// assert exact sample values to pin down the interpolator, the channel mapping
// and the mixing, and a ramp across the first and last 32 frames would obscure
// precisely the frames they check. The declick cases set them explicitly, which
// is the right way round: a test of one thing should not depend silently on
// another.
std::shared_ptr<const rt::PadConfig> make_config(std::shared_ptr<const rt::Sample> sample,
                                                 std::uint8_t pad = 0) {
  return std::make_shared<const rt::PadConfig>(rt::PadConfig{
      .sample = std::move(sample), .pad = pad, .fade_in_frames = 0, .fade_out_frames = 0});
}

// Holds channel buffers and the pointer array render_add() expects.
class Buffers {
 public:
  Buffers(std::size_t channels, std::size_t frames)
      : m_storage(channels, std::vector<float>(frames, 0.0F)), m_pointers(channels, nullptr) {
    for (std::size_t channel = 0; channel < channels; ++channel) {
      m_pointers[channel] = m_storage[channel].data();
    }
  }

  [[nodiscard]] std::span<float* const> channels() noexcept { return m_pointers; }

  [[nodiscard]] const std::vector<float>& channel(std::size_t index) const {
    return m_storage[index];
  }

  void clear() {
    for (std::vector<float>& channel : m_storage) {
      std::fill(channel.begin(), channel.end(), 0.0F);
    }
  }

 private:
  std::vector<std::vector<float>> m_storage;
  std::vector<float*> m_pointers;
};

// A garbage sink that always refuses, for exercising the "janitor has stalled"
// paths that a real GarbageRing almost never reaches.
struct RefusingSink {
  std::size_t refusals = 0;

  template <typename T>
  [[nodiscard]] bool retire(std::shared_ptr<T>&& config) noexcept {
    ++refusals;
    static_cast<void>(config);  // deliberately NOT consumed: retire() must leave it with the caller
    return false;
  }
};

}  // namespace

TEST_CASE("VoicePool plays a sample at the engine rate bit-exactly", "[unit]") {
  // Rate matches, so the phase step is exactly 1.0 and the fractional part is
  // always zero -- the interpolator must hand back input frames untouched.
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 16};

  const auto config = make_config(make_sample(8, 1, kEngineRate, 0.25F));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));

  pool.render_add(buffers.channels(), 16);

  for (std::size_t frame = 0; frame < 8; ++frame) {
    CHECK(buffers.channel(0)[frame] == 0.25F);
  }
  // The voice ends exactly at its last frame; nothing after it.
  for (std::size_t frame = 8; frame < 16; ++frame) {
    CHECK(buffers.channel(0)[frame] == 0.0F);
  }
  CHECK(pool.active_count() == 0);
}

TEST_CASE("VoicePool applies velocity as gain", "[unit]") {
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 8};

  const auto config = make_config(make_sample(8, 1, kEngineRate, 1.0F));
  REQUIRE(pool.trigger(config, 0.5F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 8);

  CHECK(buffers.channel(0)[0] == 0.5F);
}

TEST_CASE("VoicePool multiplies velocity by the pad's own gain", "[unit]") {
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 8};

  auto config = std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = make_sample(8, 1, kEngineRate, 1.0F),
                    .fade_in_frames = 0,
                    .fade_out_frames = 0,
                    .gain = 0.5F});
  REQUIRE(pool.trigger(config, 0.5F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 8);

  CHECK(buffers.channel(0)[0] == 0.25F);
}

TEST_CASE("VoicePool sums voices additively", "[unit]") {
  // render_add must accumulate, not assign: the engine clears the buffer once
  // and every source adds into it.
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 8};

  const auto config = make_config(make_sample(8, 1, kEngineRate, 1.0F));
  REQUIRE(pool.trigger(config, 0.25F, kEngineRate, garbage));
  REQUIRE(pool.trigger(config, 0.5F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 8);

  CHECK(buffers.channel(0)[0] == 0.75F);
}

TEST_CASE("VoicePool feeds a mono sample to every output channel", "[unit]") {
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{2, 8};

  const auto config = make_config(make_sample(8, 1, kEngineRate, 0.5F));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 8);

  CHECK(buffers.channel(0)[0] == 0.5F);
  CHECK(buffers.channel(1)[0] == 0.5F);
}

TEST_CASE("VoicePool keeps stereo channels matched", "[unit]") {
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{2, 8};

  auto sample = std::make_shared<rt::Sample>(kEngineRate, static_cast<std::uint16_t>(2), 8);
  std::fill(sample->mutable_channel(0).begin(), sample->mutable_channel(0).end(), 0.25F);
  std::fill(sample->mutable_channel(1).begin(), sample->mutable_channel(1).end(), 0.75F);

  REQUIRE(pool.trigger(make_config(std::move(sample)), 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 8);

  CHECK(buffers.channel(0)[0] == 0.25F);
  CHECK(buffers.channel(1)[0] == 0.75F);
}

TEST_CASE("VoicePool allocates voices deterministically", "[unit]") {
  // Lowest free index, always. Voice identity has to be reproducible for the
  // offline renderer to match the live one.
  rt::VoicePool<3> pool;
  rt::GarbageRing<8> garbage;

  const auto config = make_config(make_sample(64));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  CHECK(pool.active_count() == 3);
  CHECK(pool.triggers_started() == 3);

  // Pool is full: the next trigger steals rather than failing.
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  CHECK(pool.active_count() == 3);
  CHECK(pool.triggers_started() == 4);
}

TEST_CASE("VoicePool steals the oldest voice", "[unit]") {
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 4};

  const auto quiet = make_config(make_sample(1'000, 1, kEngineRate, 0.1F));
  const auto loud = make_config(make_sample(1'000, 1, kEngineRate, 1.0F));

  REQUIRE(pool.trigger(quiet, 1.0F, kEngineRate, garbage));  // oldest
  REQUIRE(pool.trigger(loud, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(loud, 1.0F, kEngineRate, garbage));  // must displace `quiet`

  pool.render_add(buffers.channels(), 4);
  // 0.1 is gone; two `loud` voices remain.
  CHECK(buffers.channel(0)[0] == 2.0F);
}

TEST_CASE("VoicePool generates no garbage when retriggering the same pad", "[unit]") {
  // Rolling one pad is the most common thing anyone does with a sampler. If each
  // retrigger retired the displaced config, a fast roll would fill the garbage
  // ring and start dropping hits — so a steal that lands on a voice already
  // holding this exact config reuses the reference in place.
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;

  const auto config = make_config(make_sample(10'000));
  for (int hit = 0; hit < 500; ++hit) {
    REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  }

  CHECK(garbage.overflow_count() == 0);
  CHECK(garbage.size_approx() == 0);  // nothing was ever retired
  CHECK(pool.active_count() == 2);
  CHECK(config.use_count() == 3);  // ours plus one per voice, never more
}

TEST_CASE("VoicePool still retires when the stolen voice holds a different config", "[unit]") {
  rt::VoicePool<1> pool;
  rt::GarbageRing<8> garbage;

  const auto first = make_config(make_sample(10'000, 1, kEngineRate, 0.25F));
  const auto second = make_config(make_sample(10'000, 1, kEngineRate, 0.75F));

  REQUIRE(pool.trigger(first, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(second, 1.0F, kEngineRate, garbage));

  CHECK(garbage.size_approx() == 1);
  CHECK(garbage.collect() == 1);
  CHECK(first.use_count() == 1);   // the voice's reference was released, on the janitor
  CHECK(second.use_count() == 2);  // ours plus the voice's
}

TEST_CASE("VoicePool reuses a slot only for the identical config", "[unit]") {
  // Two configs over the SAME sample. Before M3 the pool compared samples, and
  // this would have reused the slot in place -- leaving the voice playing the
  // old slice bounds and envelope after a live reassignment. It has to compare
  // configs, so this must retire.
  rt::VoicePool<1> pool;
  rt::GarbageRing<8> garbage;

  const auto sample = make_sample(10'000);
  const auto before = make_config(sample);
  const auto after = make_config(sample);

  REQUIRE(pool.trigger(before, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(after, 1.0F, kEngineRate, garbage));

  CHECK(garbage.size_approx() == 1);
  CHECK(garbage.collect() == 1);
  CHECK(before.use_count() == 1);
}

TEST_CASE("VoicePool refuses to drop a reference when the janitor has stalled", "[unit]") {
  // The invariant that matters most: retire() failing must never turn into
  // "release it here anyway". A destructor on the audio thread is exactly what
  // GarbageRing exists to prevent.
  rt::VoicePool<1> pool;
  RefusingSink sink;

  // Two *different* configs on purpose. Retriggering the same one reuses the
  // voice's reference in place and never touches the sink, so it would exercise
  // the wrong path entirely.
  const auto playing = make_config(make_sample(1'000, 1, kEngineRate, 0.25F));
  const auto incoming = make_config(make_sample(1'000, 1, kEngineRate, 0.75F));

  REQUIRE(pool.trigger(playing, 1.0F, kEngineRate, sink));
  CHECK(playing.use_count() == 2);  // ours plus the voice's

  // Pool is full, the displaced config differs, and the sink refuses — so the
  // steal must not happen. Proceeding would mean releasing `playing` here.
  CHECK_FALSE(pool.trigger(incoming, 1.0F, kEngineRate, sink));
  CHECK(sink.refusals == 1);
  CHECK(pool.active_count() == 1);
  CHECK(playing.use_count() == 2);   // still exactly one voice reference, none lost
  CHECK(incoming.use_count() == 1);  // and the new one was never taken
}

TEST_CASE("VoicePool retries reclaim until the janitor accepts", "[unit]") {
  rt::VoicePool<2> pool;
  RefusingSink refusing;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 8};

  const auto config = make_config(make_sample(4));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 8);
  CHECK(pool.active_count() == 0);

  // Finished, but the sink refuses: the voice must keep its reference and stay
  // un-reusable rather than leaking it or dropping it.
  CHECK(pool.reclaim(refusing) == 0);
  CHECK(pool.pending_reclaim_count() == 1);
  CHECK(config.use_count() == 2);

  // A working janitor clears it.
  CHECK(pool.reclaim(garbage) == 1);
  CHECK(pool.pending_reclaim_count() == 0);
  CHECK(garbage.collect() == 1);
  CHECK(config.use_count() == 1);
}

TEST_CASE("VoicePool playback is invariant to block size", "[unit]") {
  // The property the 32.32 fixed-point phase exists to guarantee. Uses a
  // fractional rate ratio so the accumulator actually carries a fraction --
  // at ratio 1.0 this would pass for a broken implementation too.
  //
  // The envelope and the declick fades are in the signal path here on purpose:
  // both are frame-denominated for exactly this reason, and if either ever
  // acquired a per-block term this is the test that would say so.
  auto sample = std::make_shared<rt::Sample>(44'100U, static_cast<std::uint16_t>(1),
                                             static_cast<std::size_t>(500));
  std::span<float> data = sample->mutable_channel(0);
  std::fill(data.begin(), data.end(), 1.0F);
  const auto config = std::make_shared<const rt::PadConfig>(rt::PadConfig{
      .sample = std::move(sample),
      .env = rt::AdsrFrames{.attack = 64, .decay = 128, .sustain = 0.6F, .release = 32}});

  auto render_in_blocks = [&config](std::size_t block) {
    rt::VoicePool<4> pool;
    rt::GarbageRing<8> garbage;
    std::vector<float> output(1'024, 0.0F);

    REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));

    std::size_t done = 0;
    while (done < output.size()) {
      const std::size_t frames = std::min(block, output.size() - done);
      float* pointer = output.data() + done;
      std::array<float*, 1> channels{pointer};
      pool.render_add(std::span<float* const>{channels}, frames);
      done += frames;
    }
    return output;
  };

  const std::vector<float> whole = render_in_blocks(1'024);
  const std::vector<float> small = render_in_blocks(1);
  const std::vector<float> medium = render_in_blocks(37);

  CHECK(whole == small);
  CHECK(whole == medium);
  // Guard against all three being silence, which would satisfy the equality
  // above while proving nothing.
  CHECK(std::any_of(whole.begin(), whole.end(), [](float value) { return value != 0.0F; }));
}

TEST_CASE("VoicePool ignores triggers it cannot honour", "[unit]") {
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;

  CHECK_FALSE(pool.trigger(nullptr, 1.0F, kEngineRate, garbage));

  CHECK_FALSE(pool.trigger(std::make_shared<const rt::PadConfig>(), 1.0F, kEngineRate, garbage));

  const auto empty = make_config(std::make_shared<const rt::Sample>(
      kEngineRate, static_cast<std::uint16_t>(1), static_cast<std::size_t>(0)));
  CHECK_FALSE(pool.trigger(empty, 1.0F, kEngineRate, garbage));

  const auto config = make_config(make_sample(16));
  CHECK_FALSE(pool.trigger(config, 1.0F, 0, garbage));

  CHECK(pool.active_count() == 0);
}

// --- slices ------------------------------------------------------------------

namespace {

// A ramp, so every frame is identifiable by its value: frame n holds n/1000.
std::shared_ptr<const rt::Sample> make_ramp(std::size_t frames) {
  auto sample = std::make_shared<rt::Sample>(kEngineRate, static_cast<std::uint16_t>(1), frames);
  std::span<float> data = sample->mutable_channel(0);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    data[frame] = static_cast<float>(frame) / 1'000.0F;
  }
  return sample;
}

std::shared_ptr<const rt::PadConfig> slice_config(std::size_t start, std::size_t end,
                                                  std::size_t frames = 1'000) {
  return std::make_shared<const rt::PadConfig>(rt::PadConfig{.sample = make_ramp(frames),
                                                             .start_frame = start,
                                                             .end_frame = end,
                                                             .fade_in_frames = 0,
                                                             .fade_out_frames = 0});
}

}  // namespace

TEST_CASE("VoicePool plays exactly the slice it was given", "[unit]") {
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 64};

  REQUIRE(pool.trigger(slice_config(100, 120), 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 64);

  // First output frame is source frame 100, last is 119 -- half-open, so 120 is
  // NOT played.
  CHECK(buffers.channel(0)[0] == 0.1F);
  CHECK(buffers.channel(0)[19] == 0.119F);
  CHECK(buffers.channel(0)[20] == 0.0F);
  CHECK(pool.active_count() == 0);
}

TEST_CASE("VoicePool treats end_frame 0 as the whole sample", "[unit]") {
  // What makes PadConfig{sample, pad} -- all set_pad_sample() builds -- mean the
  // obvious thing.
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 64};

  REQUIRE(pool.trigger(slice_config(0, 0, 32), 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 64);

  CHECK(buffers.channel(0)[31] == 0.031F);
  CHECK(buffers.channel(0)[32] == 0.0F);
}

TEST_CASE("VoicePool clamps a slice that runs past the sample", "[unit]") {
  // The config crossed a thread boundary; a range past the end must be clamped
  // rather than trusted, or the voice reads off the end of the buffer.
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 64};

  REQUIRE(pool.trigger(slice_config(20, 10'000, 32), 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 64);

  CHECK(buffers.channel(0)[11] == 0.031F);  // source frame 31, the last one
  CHECK(buffers.channel(0)[12] == 0.0F);
}

TEST_CASE("VoicePool refuses an empty or inverted slice", "[unit]") {
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;

  CHECK_FALSE(pool.trigger(slice_config(100, 100), 1.0F, kEngineRate, garbage));
  CHECK_FALSE(pool.trigger(slice_config(200, 100), 1.0F, kEngineRate, garbage));
  // Start past the end of the sample clamps to the end, which is then empty.
  CHECK_FALSE(pool.trigger(slice_config(5'000, 6'000, 32), 1.0F, kEngineRate, garbage));
  CHECK(pool.active_count() == 0);
}

TEST_CASE("VoicePool tunes a pad by scaling the phase step", "[unit]") {
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 64};

  auto config = std::make_shared<const rt::PadConfig>(rt::PadConfig{
      .sample = make_ramp(1'000), .fade_in_frames = 0, .fade_out_frames = 0, .pitch_ratio = 2.0F});
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 64);

  // An octave up: output frame n is source frame 2n.
  CHECK(buffers.channel(0)[1] == 0.002F);
  CHECK(buffers.channel(0)[10] == 0.020F);
}

TEST_CASE("VoicePool ignores a nonsensical pitch ratio", "[unit]") {
  // Zero or negative would make the phase stop or run backwards, and this value
  // came from another thread. Clamped rather than asserted: the audio thread
  // does not get to abort on bad input.
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 64};

  auto config = std::make_shared<const rt::PadConfig>(rt::PadConfig{
      .sample = make_ramp(1'000), .fade_in_frames = 0, .fade_out_frames = 0, .pitch_ratio = 0.0F});
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 64);

  CHECK(buffers.channel(0)[1] == 0.001F);  // fell back to 1.0, not stuck at frame 0
}

// --- envelope, choke and note-off --------------------------------------------

TEST_CASE("VoicePool applies the pad's amplitude envelope", "[unit]") {
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 64};

  auto config = std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = make_sample(64, 1, kEngineRate, 1.0F),
                    .env = rt::AdsrFrames{.attack = 8, .decay = 0, .sustain = 1.0F, .release = 0},
                    .fade_in_frames = 0,
                    .fade_out_frames = 0});
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 64);

  CHECK(buffers.channel(0)[0] == 0.0F);  // attack starts at silence
  CHECK(buffers.channel(0)[4] == 0.5F);  // half way up
  CHECK(buffers.channel(0)[8] == 1.0F);  // arrived, and held by a unit sustain
  CHECK(buffers.channel(0)[32] == 1.0F);
}

TEST_CASE("VoicePool chokes other voices in the same group", "[unit]") {
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 16};

  // A HARD CUT, asked for explicitly. What this case is about is whether the
  // choke happened at all, not the shape of the fall -- and since M4.5 the fall
  // has a shape by default (release_floor_frames), so "gone by the next frame"
  // has to be requested rather than assumed. The floored version is its own case
  // below.
  auto in_group = [](std::uint8_t pad, std::uint8_t group) {
    return std::make_shared<const rt::PadConfig>(
        rt::PadConfig{.sample = make_sample(4'000, 1, kEngineRate, 1.0F),
                      .pad = pad,
                      .fade_in_frames = 0,
                      .fade_out_frames = 0,
                      .release_floor_frames = 0,
                      .choke_group = group});
  };

  const auto open_hat = in_group(0, 1);
  const auto closed_hat = in_group(1, 1);

  REQUIRE(pool.trigger(open_hat, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(closed_hat, 1.0F, kEngineRate, garbage));

  // Zero-length release, so the choked voice is gone by the next frame rather
  // than merely quieter -- the point being tested is that it was released at
  // all, not the shape of the fall.
  pool.render_add(buffers.channels(), 16);
  CHECK(pool.active_count() == 1);
  CHECK(buffers.channel(0)[0] == 1.0F);  // only the closed hat sounding
}

TEST_CASE("VoicePool chokes an earlier hit on the same pad", "[unit]") {
  // A closed hi-hat cuts itself, not just its neighbours. The new voice is
  // excluded by identity rather than by pad for exactly this reason.
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 16};

  const auto hat = std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = make_sample(4'000, 1, kEngineRate, 1.0F),
                    .fade_in_frames = 0,
                    .fade_out_frames = 0,
                    .release_floor_frames = 0,  // a hard cut, so "gone" is one frame
                    .choke_group = 3});

  REQUIRE(pool.trigger(hat, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(hat, 1.0F, kEngineRate, garbage));

  pool.render_add(buffers.channels(), 16);
  CHECK(pool.active_count() == 1);
  CHECK(buffers.channel(0)[0] == 1.0F);
}

TEST_CASE("VoicePool group 0 chokes nothing", "[unit]") {
  // Otherwise every unconfigured pad would cut every other one.
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 16};

  const auto config = make_config(make_sample(4'000, 1, kEngineRate, 1.0F));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));

  pool.render_add(buffers.channels(), 16);
  CHECK(pool.active_count() == 2);
  CHECK(buffers.channel(0)[0] == 2.0F);
}

TEST_CASE("VoicePool note_off releases gate voices and ignores one-shots", "[unit]") {
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 16};

  auto pad_config = [](std::uint8_t pad, rt::TriggerMode mode) {
    return std::make_shared<const rt::PadConfig>(
        rt::PadConfig{.sample = make_sample(4'000, 1, kEngineRate, 1.0F),
                      .pad = pad,
                      .fade_in_frames = 0,
                      .fade_out_frames = 0,
                      .release_floor_frames = 0,  // a hard cut, as above
                      .trigger = mode});
  };

  REQUIRE(pool.trigger(pad_config(0, rt::TriggerMode::kGate), 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(pad_config(1, rt::TriggerMode::kOneShot), 1.0F, kEngineRate, garbage));
  CHECK(pool.active_count() == 2);

  pool.note_off(0);
  pool.note_off(1);
  pool.render_add(buffers.channels(), 16);

  // The gate voice let go; the one-shot did not, which is the definition of
  // one-shot rather than an oversight.
  CHECK(pool.active_count() == 1);
  CHECK(buffers.channel(0)[0] == 1.0F);
}

TEST_CASE("VoicePool releases through the declick floor by default", "[unit]") {
  // The click this fixes, at the level a player meets it: a choke on a pad with
  // no envelope configured used to cut from full scale to nothing in one frame.
  // The three cases above ask for that on purpose; this is what happens when
  // nobody asks for anything, which is the common case and was the broken one.
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 16};

  auto in_group = [](std::uint8_t pad) {
    return std::make_shared<const rt::PadConfig>(
        rt::PadConfig{.sample = make_sample(4'000, 1, kEngineRate, 1.0F),
                      .pad = pad,
                      .fade_in_frames = 0,
                      .fade_out_frames = 0,
                      .choke_group = 1});  // release_floor_frames left at its default
  };

  REQUIRE(pool.trigger(in_group(0), 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(in_group(1), 1.0F, kEngineRate, garbage));

  // Both are still sounding: the choked one is falling, not gone.
  pool.render_add(buffers.channels(), 16);
  CHECK(pool.active_count() == 2);

  // The sum starts at 2.0 -- the surviving voice at full scale plus the choked
  // one still at the level it was cut from -- and falls monotonically toward the
  // survivor alone. A hard cut would have started at 1.0.
  const std::span<const float> out = buffers.channel(0);
  CHECK(out[0] == 2.0F);
  for (std::size_t frame = 1; frame < 16; ++frame) {
    INFO("frame " << frame);
    CHECK(out[frame] <= out[frame - 1]);
    CHECK(out[frame] >= 1.0F);
  }

  // And it is gone by the end of the floor rather than hanging about.
  Buffers rest{1, static_cast<int>(rt::kDefaultFadeFrames)};
  pool.render_add(rest.channels(), rt::kDefaultFadeFrames);
  CHECK(pool.active_count() == 1);
}

// --- declick -----------------------------------------------------------------

namespace {

// A sine, so a slice boundary lands somewhere with a real value on both sides of
// it. A constant-valued sample would show the step just as well, but nothing
// about it would resemble the material this is for.
std::shared_ptr<const rt::Sample> make_sine(std::size_t frames, double cycles) {
  auto sample = std::make_shared<rt::Sample>(kEngineRate, static_cast<std::uint16_t>(1), frames);
  std::span<float> data = sample->mutable_channel(0);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const double phase =
        2.0 * std::numbers::pi * cycles * static_cast<double>(frame) / static_cast<double>(frames);
    data[frame] = static_cast<float>(std::sin(phase));
  }
  return sample;
}

// The largest jump between consecutive output frames, counting the step up from
// silence into the first frame and back down out of the last.
[[nodiscard]] float largest_step(const std::vector<float>& output) {
  float worst = 0.0F;
  float previous = 0.0F;
  for (const float value : output) {
    worst = std::max(worst, std::abs(value - previous));
    previous = value;
  }
  return std::max(worst, std::abs(previous));
}

}  // namespace

TEST_CASE("declick turns a slice boundary from a step into a ramp", "[unit]") {
  // The measurement behind rt::kDefaultFadeFrames, and the reason declick is
  // positional rather than part of the envelope.
  //
  // The slice starts at the sine's positive peak and ends at its negative one:
  // the worst case a chop can produce, and one that zero-crossing snap
  // deliberately cannot fix because there is no zero crossing to snap to
  // nearby.
  constexpr std::size_t kFrames = 4'000;
  constexpr std::size_t kCycles = 40;
  constexpr std::size_t kPeriod = kFrames / kCycles;                     // 100 frames
  constexpr std::size_t kStart = kPeriod / 4;                            // first positive peak
  constexpr std::size_t kEnd = kStart + (10 * kPeriod) + (kPeriod / 2);  // a negative peak

  const auto sample = make_sine(kFrames, kCycles);

  auto render_with = [&sample](std::size_t fade) {
    rt::VoicePool<2> pool;
    rt::GarbageRing<8> garbage;
    std::vector<float> output(2'048, 0.0F);
    auto config = std::make_shared<const rt::PadConfig>(rt::PadConfig{.sample = sample,
                                                                      .start_frame = kStart,
                                                                      .end_frame = kEnd,
                                                                      .fade_in_frames = fade,
                                                                      .fade_out_frames = fade});
    REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
    float* pointer = output.data();
    std::array<float*, 1> channels{pointer};
    pool.render_add(std::span<float* const>{channels}, output.size());
    return output;
  };

  const float without = largest_step(render_with(0));
  const float with = largest_step(render_with(rt::kDefaultFadeFrames));

  INFO("largest inter-frame step: " << without << " without declick, " << with << " with "
                                    << rt::kDefaultFadeFrames << " frames of fade");

  // Cutting at a peak means the output steps the full amplitude in one frame.
  CHECK(without > 0.9F);

  // With the fade the step is bounded by the ramp's own slope (1/32 of full
  // scale) plus the signal's own, which for this 480 Hz sine is 2*pi/100 per
  // frame. Measured: 1.0 without, 0.069 with -- a factor of 14.5, of which the
  // sine's natural slope is most of what remains rather than anything the fade
  // failed to smooth.
  //
  // This case is its own negative control: render_with(0) IS the
  // implementation without declick, and it is the number on the left.
  CHECK(with < without / 10.0F);
}

TEST_CASE("declick never eats more than half a slice", "[unit]") {
  // A slice shorter than two fades must still reach full amplitude somewhere in
  // the middle rather than fading in and out past each other, which would leave
  // it quieter than it should be or silent altogether.
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 64};

  auto config = std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = make_sample(1'000, 1, kEngineRate, 1.0F),
                    .start_frame = 0,
                    .end_frame = 10,
                    .fade_in_frames = 1'000,
                    .fade_out_frames = 1'000});
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 64);

  const std::vector<float>& out = buffers.channel(0);
  CHECK(out[0] == 0.0F);  // still starts silent
  CHECK(*std::max_element(out.begin(), out.begin() + 10) == 1.0F);
}

TEST_CASE("a trigger with a frame offset starts exactly there", "[unit]") {
  // Sample-accurate triggering, which is the whole point of PadEvent::frame_offset
  // and the thing M4's sequencer is built on. Before this, every hit landed on a
  // block boundary and a step placed 3 ms into a block sounded at 0 ms.
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 64};

  constexpr std::size_t kOffset = 17;  // deliberately not a round number
  const auto config = make_config(make_sample(128, 1, kEngineRate, 0.5F));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage, kOffset));
  pool.render_add(buffers.channels(), 64);

  const std::vector<float>& out = buffers.channel(0);
  for (std::size_t frame = 0; frame < kOffset; ++frame) {
    INFO("frame " << frame << " is before the offset and must be silent");
    REQUIRE(out[frame] == 0.0F);
  }
  // And it starts on the very next frame, rather than one late -- an off-by-one
  // here is inaudible and would quietly bias every sequenced hit by a sample.
  CHECK(out[kOffset] == 0.5F);
}

TEST_CASE("an offset trigger is the same audio, moved", "[unit]") {
  // The strong form: offsetting a hit must SHIFT it, not alter it. If the
  // envelope, the declick fade or the phase advanced during the skipped frames,
  // the two runs below would differ somewhere after the offset -- which is
  // exactly the bug that a "silent until the offset" check alone would miss.
  constexpr std::size_t kOffset = 23;
  constexpr std::size_t kFrames = 256;

  const auto config = std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = make_sample(512, 1, kEngineRate, 0.75F),
                    .env = rt::AdsrFrames{.attack = 40, .decay = 30, .sustain = 0.4F},
                    .fade_in_frames = 16,
                    .fade_out_frames = 16});

  rt::VoicePool<4> aligned_pool;
  rt::GarbageRing<8> aligned_garbage;
  Buffers aligned{1, kFrames};
  REQUIRE(aligned_pool.trigger(config, 1.0F, kEngineRate, aligned_garbage));
  aligned_pool.render_add(aligned.channels(), kFrames);

  rt::VoicePool<4> offset_pool;
  rt::GarbageRing<8> offset_garbage;
  Buffers offset{1, kFrames};
  REQUIRE(offset_pool.trigger(config, 1.0F, kEngineRate, offset_garbage, kOffset));
  offset_pool.render_add(offset.channels(), kFrames);

  for (std::size_t frame = 0; frame + kOffset < kFrames; ++frame) {
    INFO("frame " << frame << " aligned vs " << (frame + kOffset) << " offset");
    REQUIRE(aligned.channel(0)[frame] == offset.channel(0)[frame + kOffset]);
  }
}

TEST_CASE("a frame offset applies to one block only", "[unit]") {
  // start_offset describes a block, not a voice. A voice that survives into the
  // next block must render it from frame 0 -- leaving the offset set would make
  // every subsequent block of a long note skip its first samples, which sounds
  // like a stutter and would be blamed on the interpolator.
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 32};

  const auto config = make_config(make_sample(512, 1, kEngineRate, 1.0F));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage, 30));
  pool.render_add(buffers.channels(), 32);
  CHECK(buffers.channel(0)[29] == 0.0F);
  CHECK(buffers.channel(0)[30] == 1.0F);

  buffers.clear();
  pool.render_add(buffers.channels(), 32);
  for (std::size_t frame = 0; frame < 32; ++frame) {
    INFO("second block, frame " << frame);
    REQUIRE(buffers.channel(0)[frame] == 1.0F);
  }
}

TEST_CASE("an offset past the block starts the voice in the next one", "[unit]") {
  // The floor rather than a behaviour to rely on: the engine clamps before this
  // can happen. What matters is that it degrades to a block-late hit instead of
  // reading past the end of the buffer.
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 16};

  const auto config = make_config(make_sample(128, 1, kEngineRate, 1.0F));
  REQUIRE(pool.trigger(config, 1.0F, kEngineRate, garbage, 999));
  pool.render_add(buffers.channels(), 16);
  for (const float value : buffers.channel(0)) {
    REQUIRE(value == 0.0F);
  }

  buffers.clear();
  pool.render_add(buffers.channels(), 16);
  CHECK(buffers.channel(0)[0] == 1.0F);
}
