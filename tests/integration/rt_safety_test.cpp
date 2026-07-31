// The enforcement test CLAUDE.md names: "tests/integration/rt_safety_test.cpp
// runs the engine under the interposer".
//
// Everything else about the engine can be right while it still allocates once
// per block — which shows up as an occasional dropout on a loaded machine and is
// miserable to track down after the fact. This runs the real render path under
// the RT_SCOPE guard and requires zero violations.

#include "engine/engine.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/rt_scope.hpp"
#include "rt/sample.hpp"
#include "rt/sequencer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <random>
#include <span>
#include <utility>
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
  if constexpr (!rt::kAllocationDetectionEnabled) {
    SKIP("allocation detection is compiled out (TSan build) — see rt_scope.hpp");
  }

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

TEST_CASE("Engine::render allocates nothing while voices are playing", "[integration]") {
  // The M1 acceptance criterion: "no allocation in the callback" -- and the
  // callback now does far more than clear buffers. It drains a ring, copies
  // shared_ptrs, starts and steals voices, runs the interpolator, and hands
  // finished samples to the garbage ring. Every one of those is a place an
  // allocation could sneak in, and the test above would never notice because it
  // never triggers anything.
  if constexpr (!rt::kAllocationDetectionEnabled) {
    SKIP("allocation detection is compiled out (TSan build) — see rt_scope.hpp");
  }

  const engine::Engine::Config config{
      .sample_rate = 48'000, .num_channels = kChannels, .max_block_frames = kMaxBlock, .seed = 0};
  engine::Engine eng{config};

  // Loaded before the guard is armed: building samples allocates, and it is
  // supposed to -- it happens on a worker, not here.
  for (std::uint8_t pad = 0; pad < 4; ++pad) {
    auto sample = std::make_shared<rt::Sample>(
        44'100U, static_cast<std::uint16_t>(pad % 2 == 0 ? 1 : 2), std::size_t{2'000});
    for (std::uint16_t channel = 0; channel < sample->num_channels(); ++channel) {
      std::span<float> data = sample->mutable_channel(channel);
      for (std::size_t frame = 0; frame < data.size(); ++frame) {
        data[frame] = 0.1F * static_cast<float>((frame % 17) + 1);
      }
    }
    REQUIRE(eng.set_pad_sample(pad, std::move(sample)));
  }

  std::vector<std::vector<float>> storage(kChannels, std::vector<float>(kMaxBlock, 0.0F));
  std::vector<float*> channels(kChannels);
  for (std::uint16_t i = 0; i < kChannels; ++i) {
    channels[i] = storage[i].data();
  }

  std::mt19937 rng{4'242};
  std::uniform_int_distribution<std::size_t> block_sizes{1, kMaxBlock};
  std::vector<std::size_t> plan(2'000);
  for (std::size_t& size : plan) {
    size = block_sizes(rng);
  }

  const HandlerSwap swap;
  std::size_t collected = 0;
  for (std::size_t index = 0; index < plan.size(); ++index) {
    // Far more triggers than there are voices, so stealing and the
    // steal-plus-retire path run constantly rather than incidentally.
    const rt::PadEvent event{
        .pad = static_cast<std::uint8_t>(index % 4), .velocity = 0.75F, .frame_offset = 0};
    static_cast<void>(eng.trigger_pad(event));
    eng.render(std::span<float* const>{channels}, plan[index]);

    // The janitor runs on this thread here; collect_garbage() is outside the
    // callback and may allocate/free freely, which is the whole point.
    if (index % 8 == 0) {
      collected += eng.collect_garbage();
    }
  }
  collected += eng.collect_garbage();

  CHECK(HandlerSwap::count() == 0);
  CHECK(eng.frames_rendered() > 0);

  // Without this the test could pass while every trigger was silently dropped
  // and no voice ever ran -- zero allocations, and zero work.
  CHECK(collected > 0);
  CHECK(eng.garbage_overflows() == 0);
  CHECK(eng.dropped_events() == 0);
}

TEST_CASE("Engine::render allocates nothing while pads are being reassigned", "[integration]") {
  // The M3 acceptance criterion: "a pad reassigned while the stream runs makes
  // no allocation on the audio thread and destroys nothing there". TSan covers
  // the race in engine_threading_test.cpp; this covers the heap, and the two
  // cannot share a run because allocation detection is compiled out under TSan
  // (rt_scope.hpp).
  //
  // The two halves need two different mechanisms, and it matters which:
  //
  //   - "allocates nothing" is the RT guard, via HandlerSwap below.
  //   - "destroys nothing" is NOT. The guard checks allocation only and says so
  //     (rt_scope.cpp): a check inside operator delete cannot tell an object
  //     allocated inside the scope from one allocated before it, so it was left
  //     out deliberately. Asserting zero violations here would therefore say
  //     nothing at all about destruction — verified by dropping the displaced
  //     config on the audio thread instead of retiring it, which this test
  //     passed until the deleter below was added.
  //
  // So destruction is observed where it actually happens: a custom deleter that
  // asks rt::in_rt_scope() at the moment the last reference goes. Inside
  // render() that answer is true by construction, which is exactly the question.
  if constexpr (!rt::kAllocationDetectionEnabled) {
    SKIP("allocation detection is compiled out (TSan build) — see rt_scope.hpp");
  }

  // Declared BEFORE the engine, and it has to be. Locals are destroyed in
  // reverse order, so the engine dies first and releases the pad configs it
  // still holds — which runs the deleters below. Declared after, these counters
  // would already be gone by then: ASan reports it as a stack-use-after-scope,
  // and a dev build does not notice at all.
  constexpr std::size_t kConfigCount = 64;
  std::size_t destroyed_total = 0;
  std::size_t destroyed_in_rt_scope = 0;

  const engine::Engine::Config config{
      .sample_rate = 48'000, .num_channels = kChannels, .max_block_frames = kMaxBlock, .seed = 0};
  engine::Engine eng{config};

  std::vector<std::vector<float>> storage(kChannels, std::vector<float>(kMaxBlock, 0.0F));
  std::vector<float*> channels(kChannels);
  for (std::uint16_t i = 0; i < kChannels; ++i) {
    channels[i] = storage[i].data();
  }

  // The test keeps NO strong reference to the configs it publishes, deliberately.
  // With one, the engine would never hold the last one, nothing would ever be
  // destroyed, and the deleter below would never run.
  const HandlerSwap swap;
  std::size_t collected = 0;
  std::size_t adopted = 0;
  for (std::size_t index = 0; index < kConfigCount; ++index) {
    // Built and published on the control thread, inside the loop. Allocation
    // here is legal and expected: the guard only reports allocations made while
    // the thread is inside an RT_SCOPE, and only render() opens one.
    const auto pad = static_cast<std::uint8_t>(index % rt::kNumPads);
    auto sample = std::make_shared<rt::Sample>(44'100U, std::uint16_t{1}, std::size_t{600});
    std::span<float> data = sample->mutable_channel(0);
    for (std::size_t frame = 0; frame < data.size(); ++frame) {
      data[frame] = 0.05F * static_cast<float>((frame + index) % 19);
    }

    // new + a custom deleter rather than make_shared, so the moment of
    // destruction is observable. This is the "destroys nothing there" half.
    std::shared_ptr<const rt::PadConfig> pad_config{
        new rt::PadConfig{.sample = std::move(sample), .pad = pad},
        [&destroyed_total, &destroyed_in_rt_scope](const rt::PadConfig* victim) {
          ++destroyed_total;
          if (rt::in_rt_scope()) {
            ++destroyed_in_rt_scope;
          }
          delete victim;  // NOLINT(cppcoreguidelines-owning-memory)
        }};

    if (eng.publish_pad_config(std::move(pad_config))) {
      ++adopted;
    }
    static_cast<void>(
        eng.trigger_pad(rt::PadEvent{.pad = pad, .velocity = 0.9F, .frame_offset = 0}));

    // Two blocks per publish: the first adopts, swaps and retires; the second
    // proves the steady state has nothing left to do.
    eng.render(std::span<float* const>{channels}, 128);
    eng.render(std::span<float* const>{channels}, 128);

    // The janitor, on this thread and outside the callback, where freeing is
    // exactly what is supposed to happen.
    collected += eng.collect_garbage();
  }
  collected += eng.collect_garbage();

  CHECK(HandlerSwap::count() == 0);
  CHECK(adopted == kConfigCount);

  // THE assertion of this test.
  CHECK(destroyed_in_rt_scope == 0);

  // ...and its anti-vacuity guard. Sixteen pads start empty, so the first
  // sixteen publishes displace a null; every later one must have been destroyed
  // by now, somewhere. Zero destructions would make the line above true of an
  // empty room.
  CHECK(destroyed_total >= kConfigCount - rt::kNumPads);

  CHECK(collected >= kConfigCount - rt::kNumPads);
  CHECK(eng.garbage_overflows() == 0);
  CHECK(eng.rejected_pad_configs() == 0);
}

TEST_CASE("Engine::render allocates nothing while the sequencer is republished", "[integration]") {
  // The sequencer is new audio-thread state, published through the same handoff
  // protocol as pad configs, so it inherits both halves of the same obligation:
  // nothing allocated in the callback, and nothing destroyed there either.
  //
  // Worth its own case rather than folding into the pad one above, because the
  // failure modes differ. A SequencerState is ~8 KB where a PadConfig is a
  // handful of words, so a copy that slipped onto the audio thread would be an
  // 8 KB memcpy AND an allocation -- loud here, and easy to miss in a test whose
  // objects are small enough to fit anywhere.
  if constexpr (!rt::kAllocationDetectionEnabled) {
    SKIP("allocation detection is compiled out (TSan build) -- see rt_scope.hpp");
  }

  // Before the engine, for the reason spelled out in the pad case: the engine is
  // destroyed first and runs the deleters, so these must outlive it.
  constexpr std::size_t kStateCount = 32;
  std::size_t destroyed_total = 0;
  std::size_t destroyed_in_rt_scope = 0;

  const engine::Engine::Config config{
      .sample_rate = 48'000, .num_channels = kChannels, .max_block_frames = kMaxBlock, .seed = 0};
  engine::Engine eng{config};

  std::vector<std::vector<float>> storage(kChannels, std::vector<float>(kMaxBlock, 0.0F));
  std::vector<float*> channels(kChannels);
  for (std::uint16_t i = 0; i < kChannels; ++i) {
    channels[i] = storage[i].data();
  }

  const HandlerSwap swap;
  std::size_t collected = 0;
  std::size_t adopted = 0;
  for (std::size_t index = 0; index < kStateCount; ++index) {
    // Built on the control thread, where allocating 8 KB is exactly what is
    // supposed to happen.
    auto* built = new rt::SequencerState{};  // NOLINT(cppcoreguidelines-owning-memory)
    built->bpm_x100 = static_cast<std::uint32_t>(9'000 + (index * 137));
    built->selected_pattern = static_cast<std::uint8_t>(index % rt::kMaxPatterns);
    built->patterns[built->selected_pattern].steps[index % rt::kMaxSteps][0].on = true;

    std::shared_ptr<const rt::SequencerState> state{
        built, [&destroyed_total, &destroyed_in_rt_scope](const rt::SequencerState* victim) {
          ++destroyed_total;
          if (rt::in_rt_scope()) {
            ++destroyed_in_rt_scope;
          }
          delete victim;  // NOLINT(cppcoreguidelines-owning-memory)
        }};

    if (eng.publish_sequencer(std::move(state))) {
      ++adopted;
    }
    static_cast<void>(eng.send_transport(rt::TransportCommand{
        .kind = index % 4 == 0 ? rt::TransportCommandKind::kPlay : rt::TransportCommandKind::kSeek,
        .position_frames = index * 1'000}));

    eng.render(std::span<float* const>{channels}, 128);
    eng.render(std::span<float* const>{channels}, 128);
    collected += eng.collect_garbage();
  }

  // The engine still holds the newest state, so drop the control-side reference
  // and collect once more -- otherwise the last one is destroyed at scope exit
  // and destroyed_total is short by one for no interesting reason.
  collected += eng.collect_garbage();

  CHECK(HandlerSwap::count() == 0);
  CHECK(adopted == kStateCount);
  CHECK(destroyed_in_rt_scope == 0);

  // The anti-vacuity guard: only the first publish displaces a null, so every
  // other one must have died somewhere by now.
  CHECK(destroyed_total >= kStateCount - 2);

  // And they went through the janitor rather than being freed somewhere else --
  // which is the only route that keeps the deleter off the audio thread.
  CHECK(collected >= kStateCount - 2);
  CHECK(eng.garbage_overflows() == 0);
  CHECK(eng.rejected_sequencer_states() == 0);
}

TEST_CASE("the RT guard is armed during the render test", "[integration]") {
  if constexpr (!rt::kAllocationDetectionEnabled) {
    SKIP("allocation detection is compiled out (TSan build) — see rt_scope.hpp");
  }

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
