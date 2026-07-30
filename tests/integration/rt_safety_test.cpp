// The enforcement test CLAUDE.md names: "tests/integration/rt_safety_test.cpp
// runs the engine under the interposer".
//
// Everything else about the engine can be right while it still allocates once
// per block — which shows up as an occasional dropout on a loaded machine and is
// miserable to track down after the fact. This runs the real render path under
// the RT_SCOPE guard and requires zero violations.

#include "engine/engine.hpp"
#include "rt/rt_scope.hpp"

#include <atomic>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

std::atomic<int> g_violation_count{0};

void counting_handler(const char* /*what*/) {
  g_violation_count.fetch_add(1, std::memory_order_relaxed);
}

class HandlerSwap {
 public:
  HandlerSwap() : m_previous(rt::set_violation_handler(&counting_handler)) {
    g_violation_count.store(0, std::memory_order_relaxed);
  }

  ~HandlerSwap() { rt::set_violation_handler(m_previous); }

  HandlerSwap(const HandlerSwap&) = delete;
  HandlerSwap& operator=(const HandlerSwap&) = delete;
  HandlerSwap(HandlerSwap&&) = delete;
  HandlerSwap& operator=(HandlerSwap&&) = delete;

  [[nodiscard]] static int count() { return g_violation_count.load(std::memory_order_relaxed); }

 private:
  rt::ViolationHandler m_previous;
};

constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kMaxBlock = 1'024;

}  // namespace

TEST_CASE("Engine::render allocates nothing", "[integration]") {
  const engine::Engine::Config config{
      .sample_rate = 48'000, .num_channels = kChannels, .max_block_frames = kMaxBlock, .seed = 0};
  engine::Engine eng{config};

  // Every buffer this test needs is allocated up front, the way the audio
  // callback's buffers are provided by the device layer.
  std::vector<std::vector<float>> storage(kChannels, std::vector<float>(kMaxBlock, 0.0F));
  std::vector<float*> channels(kChannels);
  for (std::uint16_t i = 0; i < kChannels; ++i) {
    channels[i] = storage[i].data();
  }

  std::mt19937 rng{99};
  std::uniform_int_distribution<std::size_t> block_sizes{1, kMaxBlock};

  // Precompute the block sizes: drawing from the RNG inside the measured region
  // would be measuring the RNG.
  std::vector<std::size_t> plan(2'000);
  for (std::size_t& size : plan) {
    size = block_sizes(rng);
  }

  const HandlerSwap swap;
  for (const std::size_t block : plan) {
    eng.render(std::span<float* const>{channels}, block);
  }

  CHECK(HandlerSwap::count() == 0);
  CHECK(eng.frames_rendered() > 0);
}

TEST_CASE("the RT guard is armed during the render test", "[integration]") {
  // Without this, a build that silently lost the guard would make the test above
  // pass by measuring nothing at all.
  const HandlerSwap swap;
  {
    RT_SCOPE();
    void* block = ::operator new(64);
    static_cast<void>(block);
    ::operator delete(block);
  }
  CHECK(HandlerSwap::count() > 0);
}
