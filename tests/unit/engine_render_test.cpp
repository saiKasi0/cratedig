#include "engine/engine.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kMaxBlock = 2'048;

engine::Engine::Config test_config() {
  return engine::Engine::Config{
      .sample_rate = 48'000, .num_channels = kChannels, .max_block_frames = kMaxBlock, .seed = 0};
}

// Holds the output of a whole render pass as one contiguous, channel-interleaved
// block so it can be hashed and compared byte for byte.
class RenderCapture {
 public:
  RenderCapture(std::size_t total_frames, std::uint16_t channels)
      : m_channels(channels), m_total_frames(total_frames), m_data(total_frames * channels, 0.0F) {}

  // Renders total_frames through the engine in blocks of the given sizes,
  // cycling through them, and captures everything.
  void render_in_blocks(engine::Engine& eng, std::span<const std::size_t> block_sizes) {
    std::vector<float> scratch(static_cast<std::size_t>(kMaxBlock) * m_channels, 0.0F);
    std::vector<float*> channel_pointers(m_channels);

    std::size_t done = 0;
    std::size_t next_size = 0;
    while (done < m_total_frames) {
      const std::size_t block =
          std::min(block_sizes[next_size % block_sizes.size()], m_total_frames - done);
      next_size++;
      if (block == 0) {
        continue;
      }

      // Dirty the scratch buffer before every call: if render() ever fails to
      // write a sample, the test must see the garbage rather than a leftover
      // zero that makes the bug invisible.
      std::fill(scratch.begin(), scratch.end(), -7.5F);
      for (std::uint16_t channel = 0; channel < m_channels; ++channel) {
        channel_pointers[channel] =
            scratch.data() + (static_cast<std::size_t>(channel) * kMaxBlock);
      }

      eng.render(std::span<float* const>{channel_pointers}, block);

      for (std::uint16_t channel = 0; channel < m_channels; ++channel) {
        float* source = channel_pointers[channel];
        float* destination =
            m_data.data() + (static_cast<std::size_t>(channel) * m_total_frames) + done;
        std::memcpy(destination, source, block * sizeof(float));
      }
      done += block;
    }
  }

  [[nodiscard]] std::span<const float> samples() const { return m_data; }

  // FNV-1a over the raw bytes. This is the golden-hash harness the DSP work in
  // M1+ inherits: a changed hash is a changed behaviour, to be explained rather
  // than re-baselined (docs/TESTING.md).
  [[nodiscard]] std::uint64_t hash() const {
    constexpr std::uint64_t kOffsetBasis = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(m_data.data());
    const std::size_t byte_count = m_data.size() * sizeof(float);

    std::uint64_t hash = kOffsetBasis;
    for (std::size_t i = 0; i < byte_count; ++i) {
      hash ^= bytes[i];
      hash *= kPrime;
    }
    return hash;
  }

 private:
  std::uint16_t m_channels;
  std::size_t m_total_frames;
  std::vector<float> m_data;
};

}  // namespace

TEST_CASE("Engine renders exact silence", "[unit]") {
  engine::Engine eng{test_config()};

  constexpr std::size_t kFrames = 4'096;
  RenderCapture capture{kFrames, kChannels};
  const std::array<std::size_t, 1> blocks{128};
  capture.render_in_blocks(eng, blocks);

  // Bit-exact, not approximate: a denormal or a -0.0f leaking through would be
  // inaudible now and a real bug later.
  const std::vector<float> expected_zeros(capture.samples().size(), 0.0F);
  CHECK(std::memcmp(capture.samples().data(), expected_zeros.data(),
                    expected_zeros.size() * sizeof(float)) == 0);
}

TEST_CASE("Engine output is invariant to block size", "[unit]") {
  // The property that actually matters: state must not leak across block
  // boundaries. An engine that behaves differently at 128 frames than at 1024 is
  // not deterministic no matter how repeatable each individual run is.
  constexpr std::size_t kFrames = 48'000;

  RenderCapture single{kFrames, kChannels};
  engine::Engine engine_single{test_config()};
  const std::array<std::size_t, 1> one_block{kMaxBlock};
  single.render_in_blocks(engine_single, one_block);

  RenderCapture chunked{kFrames, kChannels};
  engine::Engine engine_chunked{test_config()};
  const std::array<std::size_t, 1> small_blocks{128};
  chunked.render_in_blocks(engine_chunked, small_blocks);

  RenderCapture ragged{kFrames, kChannels};
  engine::Engine engine_ragged{test_config()};
  // A fixed seed, so "random" block sizes are the same on every run and on every
  // machine — a varying test input cannot prove determinism.
  std::mt19937 rng{12'345};
  std::uniform_int_distribution<std::size_t> sizes{1, kMaxBlock};
  std::array<std::size_t, 64> ragged_blocks{};
  for (std::size_t& size : ragged_blocks) {
    size = sizes(rng);
  }
  ragged.render_in_blocks(engine_ragged, ragged_blocks);

  CHECK(single.hash() == chunked.hash());
  CHECK(single.hash() == ragged.hash());
}

TEST_CASE("Engine renders the same bytes on every run", "[unit]") {
  constexpr std::size_t kFrames = 8'192;
  const std::array<std::size_t, 3> blocks{64, 512, 333};

  RenderCapture first{kFrames, kChannels};
  engine::Engine engine_first{test_config()};
  first.render_in_blocks(engine_first, blocks);

  RenderCapture second{kFrames, kChannels};
  engine::Engine engine_second{test_config()};
  second.render_in_blocks(engine_second, blocks);

  CHECK(first.hash() == second.hash());
  CHECK(std::memcmp(first.samples().data(), second.samples().data(),
                    first.samples().size() * sizeof(float)) == 0);

  // The committed golden value. M0 renders silence, so this is the FNV-1a hash
  // of 8192 frames x 2 channels of zero bytes. When the engine starts producing
  // audio this assertion changes — and that change must be justified in the
  // commit, never quietly re-baselined.
  CHECK(first.hash() == 0xEB05052EA5B62325ULL);
}

TEST_CASE("Engine counts rendered frames", "[unit]") {
  engine::Engine eng{test_config()};
  CHECK(eng.frames_rendered() == 0);

  RenderCapture capture{1'000, kChannels};
  const std::array<std::size_t, 2> blocks{100, 150};
  capture.render_in_blocks(eng, blocks);

  CHECK(eng.frames_rendered() == 1'000);
}

TEST_CASE("Engine keeps its configuration", "[unit]") {
  const engine::Engine eng{test_config()};

  CHECK(eng.config().sample_rate == 48'000);
  CHECK(eng.config().num_channels == kChannels);
  CHECK(eng.config().max_block_frames == kMaxBlock);
  CHECK(eng.config().seed == 0);
}
