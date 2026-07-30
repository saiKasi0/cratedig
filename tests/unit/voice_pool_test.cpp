#include "rt/voice_pool.hpp"

#include "rt/garbage_ring.hpp"
#include "rt/sample.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
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
  [[nodiscard]] bool retire(std::shared_ptr<T>&& sample) noexcept {
    ++refusals;
    static_cast<void>(sample);  // deliberately NOT consumed: retire() must leave it with the caller
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

  const auto sample = make_sample(8, 1, kEngineRate, 0.25F);
  REQUIRE(pool.trigger(sample, 1.0F, kEngineRate, garbage));

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

  const auto sample = make_sample(8, 1, kEngineRate, 1.0F);
  REQUIRE(pool.trigger(sample, 0.5F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 8);

  CHECK(buffers.channel(0)[0] == 0.5F);
}

TEST_CASE("VoicePool sums voices additively", "[unit]") {
  // render_add must accumulate, not assign: the engine clears the buffer once
  // and every source adds into it.
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 8};

  const auto sample = make_sample(8, 1, kEngineRate, 1.0F);
  REQUIRE(pool.trigger(sample, 0.25F, kEngineRate, garbage));
  REQUIRE(pool.trigger(sample, 0.5F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 8);

  CHECK(buffers.channel(0)[0] == 0.75F);
}

TEST_CASE("VoicePool feeds a mono sample to every output channel", "[unit]") {
  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{2, 8};

  const auto sample = make_sample(8, 1, kEngineRate, 0.5F);
  REQUIRE(pool.trigger(sample, 1.0F, kEngineRate, garbage));
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

  REQUIRE(pool.trigger(std::shared_ptr<const rt::Sample>{sample}, 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 8);

  CHECK(buffers.channel(0)[0] == 0.25F);
  CHECK(buffers.channel(1)[0] == 0.75F);
}

TEST_CASE("VoicePool allocates voices deterministically", "[unit]") {
  // Lowest free index, always. Voice identity has to be reproducible for the
  // offline renderer to match the live one.
  rt::VoicePool<3> pool;
  rt::GarbageRing<8> garbage;

  const auto sample = make_sample(64);
  REQUIRE(pool.trigger(sample, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(sample, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(sample, 1.0F, kEngineRate, garbage));
  CHECK(pool.active_count() == 3);
  CHECK(pool.triggers_started() == 3);

  // Pool is full: the next trigger steals rather than failing.
  REQUIRE(pool.trigger(sample, 1.0F, kEngineRate, garbage));
  CHECK(pool.active_count() == 3);
  CHECK(pool.triggers_started() == 4);
}

TEST_CASE("VoicePool steals the oldest voice", "[unit]") {
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;
  Buffers buffers{1, 4};

  const auto quiet = make_sample(1'000, 1, kEngineRate, 0.1F);
  const auto loud = make_sample(1'000, 1, kEngineRate, 1.0F);

  REQUIRE(pool.trigger(quiet, 1.0F, kEngineRate, garbage));  // oldest
  REQUIRE(pool.trigger(loud, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(loud, 1.0F, kEngineRate, garbage));  // must displace `quiet`

  pool.render_add(buffers.channels(), 4);
  // 0.1 is gone; two `loud` voices remain.
  CHECK(buffers.channel(0)[0] == 2.0F);
}

TEST_CASE("VoicePool generates no garbage when retriggering the same sample", "[unit]") {
  // Rolling one pad is the most common thing anyone does with a sampler. If each
  // retrigger retired the displaced sample, a fast roll would fill the garbage
  // ring and start dropping hits — so a steal that lands on a voice already
  // holding this exact sample reuses the reference in place.
  rt::VoicePool<2> pool;
  rt::GarbageRing<8> garbage;

  const auto sample = make_sample(10'000);
  for (int hit = 0; hit < 500; ++hit) {
    REQUIRE(pool.trigger(sample, 1.0F, kEngineRate, garbage));
  }

  CHECK(garbage.overflow_count() == 0);
  CHECK(garbage.size_approx() == 0);  // nothing was ever retired
  CHECK(pool.active_count() == 2);
  CHECK(sample.use_count() == 3);  // ours plus one per voice, never more
}

TEST_CASE("VoicePool still retires when the stolen voice holds a different sample", "[unit]") {
  rt::VoicePool<1> pool;
  rt::GarbageRing<8> garbage;

  const auto first = make_sample(10'000, 1, kEngineRate, 0.25F);
  const auto second = make_sample(10'000, 1, kEngineRate, 0.75F);

  REQUIRE(pool.trigger(first, 1.0F, kEngineRate, garbage));
  REQUIRE(pool.trigger(second, 1.0F, kEngineRate, garbage));

  CHECK(garbage.size_approx() == 1);
  CHECK(garbage.collect() == 1);
  CHECK(first.use_count() == 1);   // the voice's reference was released, on the janitor
  CHECK(second.use_count() == 2);  // ours plus the voice's
}

TEST_CASE("VoicePool refuses to drop a reference when the janitor has stalled", "[unit]") {
  // The invariant that matters most: retire() failing must never turn into
  // "release it here anyway". A Sample destructor on the audio thread is exactly
  // what GarbageRing exists to prevent.
  rt::VoicePool<1> pool;
  RefusingSink sink;

  // Two *different* samples on purpose. Retriggering the same one reuses the
  // voice's reference in place and never touches the sink, so it would exercise
  // the wrong path entirely.
  const auto playing = make_sample(1'000, 1, kEngineRate, 0.25F);
  const auto incoming = make_sample(1'000, 1, kEngineRate, 0.75F);

  REQUIRE(pool.trigger(playing, 1.0F, kEngineRate, sink));
  CHECK(playing.use_count() == 2);  // ours plus the voice's

  // Pool is full, the displaced sample differs, and the sink refuses — so the
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

  const auto sample = make_sample(4);
  REQUIRE(pool.trigger(sample, 1.0F, kEngineRate, garbage));
  pool.render_add(buffers.channels(), 8);
  CHECK(pool.active_count() == 0);

  // Finished, but the sink refuses: the voice must keep its reference and stay
  // un-reusable rather than leaking it or dropping it.
  CHECK(pool.reclaim(refusing) == 0);
  CHECK(pool.pending_reclaim_count() == 1);
  CHECK(sample.use_count() == 2);

  // A working janitor clears it.
  CHECK(pool.reclaim(garbage) == 1);
  CHECK(pool.pending_reclaim_count() == 0);
  CHECK(garbage.collect() == 1);
  CHECK(sample.use_count() == 1);
}

TEST_CASE("VoicePool playback is invariant to block size", "[unit]") {
  // The property the 32.32 fixed-point phase exists to guarantee. Uses a
  // fractional rate ratio so the accumulator actually carries a fraction --
  // at ratio 1.0 this would pass for a broken implementation too.
  const auto sample = make_sample(500, 1, 44'100);

  auto render_in_blocks = [&sample](std::size_t block) {
    rt::VoicePool<4> pool;
    rt::GarbageRing<8> garbage;
    std::vector<float> output(1'024, 0.0F);

    REQUIRE(pool.trigger(sample, 1.0F, kEngineRate, garbage));

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

  const auto empty = std::make_shared<const rt::Sample>(kEngineRate, static_cast<std::uint16_t>(1),
                                                        static_cast<std::size_t>(0));
  CHECK_FALSE(pool.trigger(empty, 1.0F, kEngineRate, garbage));

  const auto sample = make_sample(16);
  CHECK_FALSE(pool.trigger(sample, 1.0F, 0, garbage));

  CHECK(pool.active_count() == 0);
}
