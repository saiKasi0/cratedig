// The three lanes running at once, which is the only configuration that proves
// anything about the message path.
//
// Single-threaded tests can exercise every line of SpscRing and GarbageRing and
// still tell you nothing about whether the memory ordering is right. TSan is the
// authority here (docs/TESTING.md); this test exists to give it something to
// watch: a control thread pushing pad events, an audio thread draining them,
// starting and stealing voices, and retiring samples, and a janitor thread
// destroying them.
//
// Sample destruction happening on the janitor rather than the audio thread is
// the specific invariant CLAUDE.md rule 5 demands, and it is only observable
// with all three running.

#include "engine/engine.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kBlockFrames = 256;
// Enough blocks for the three lanes to interleave thousands of times, which is
// what gives TSan something to find. Verified to still be sensitive: weakening
// GarbageRing's release store to relaxed makes TSan report eight data races on
// every run at this size, so the speedup below did not cost the test its teeth.
constexpr std::size_t kBlocks = 2'000;

// Bounded so the control thread's runtime does not depend on the scheduler.
constexpr std::uint64_t kEventsToSend = 4'000;

std::shared_ptr<const rt::Sample> make_short_sample(std::uint32_t rate, std::size_t frames) {
  auto sample = std::make_shared<rt::Sample>(rate, static_cast<std::uint16_t>(1), frames);
  std::span<float> data = sample->mutable_channel(0);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    data[frame] = static_cast<float>(frame % 32) / 32.0F;
  }
  return sample;
}

}  // namespace

TEST_CASE("Engine survives control, audio and janitor threads at once", "[stress]") {
  const engine::Engine::Config config{.sample_rate = 48'000,
                                      .num_channels = kChannels,
                                      .max_block_frames = kBlockFrames,
                                      .seed = 0};
  engine::Engine eng{config};

  // Pads are loaded before any thread starts, which was a requirement until M3
  // and is now merely the simple case — the reconfiguration test below is the
  // one that loads them while the audio thread runs.
  std::vector<std::shared_ptr<const rt::Sample>> loaded;
  for (std::uint8_t pad = 0; pad < 4; ++pad) {
    auto sample = make_short_sample(44'100U + pad, 300 + (pad * 50U));
    loaded.push_back(sample);
    // Evaluated before the assertion rather than inside it: REQUIRE expands to a
    // loop, and clang-analyzer reads a std::move inside one as a move that could
    // happen twice.
    const bool published = eng.set_pad_sample(pad, std::move(sample));
    REQUIRE(published);
  }

  std::vector<std::vector<float>> storage(kChannels, std::vector<float>(kBlockFrames, 0.0F));
  std::vector<float*> channels(kChannels);
  for (std::uint16_t i = 0; i < kChannels; ++i) {
    channels[i] = storage[i].data();
  }

  std::atomic<bool> audio_done{false};
  std::atomic<std::uint64_t> events_sent{0};
  std::atomic<std::size_t> collected{0};
  std::atomic<std::uint64_t> telemetry_reads{0};
  std::atomic<bool> telemetry_saw_playing{false};

  std::thread audio([&] {
    for (std::size_t block = 0; block < kBlocks; ++block) {
      eng.render(std::span<float* const>{channels}, kBlockFrames);
    }
    // release: everything this thread retired must be visible to the janitor's
    // final sweep below, which acquires on this flag.
    audio_done.store(true, std::memory_order_release);
  });

  // Both helper threads do a BOUNDED amount of work and sleep between attempts,
  // rather than spinning until the audio thread finishes.
  //
  // The spinning version of this test ran in 5 s, 70 s and 287 s on three
  // consecutive runs of identical work, and timed out under TSan. There is no
  // deadlock and no lock: the audio thread is the only one whose progress ends
  // the test, and two threads calling yield() in a tight loop consume whole
  // cores without ever sleeping, so they starve it. Under TSan, where every ring
  // operation is instrumented, that starvation dominates completely. Sleeping
  // costs these threads nothing they need and leaves the CPU to the thread doing
  // the actual work.
  std::thread control([&] {
    std::uint64_t sent = 0;
    for (std::uint64_t i = 0; i < kEventsToSend; ++i) {
      const rt::PadEvent event{
          .pad = static_cast<std::uint8_t>(i % 4), .velocity = 0.5F, .frame_offset = 0};
      while (!eng.trigger_pad(event)) {
        if (audio_done.load(std::memory_order_acquire)) {
          events_sent.store(sent, std::memory_order_relaxed);
          return;  // nothing left to drain the ring; stop rather than spin forever
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      ++sent;
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    events_sent.store(sent, std::memory_order_relaxed);
  });

  std::thread janitor([&] {
    std::size_t total = 0;
    while (!audio_done.load(std::memory_order_acquire)) {
      total += eng.collect_garbage();
      // Roughly a UI frame's worth of lag, which is what the real janitor will
      // have once the shell owns this loop.
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    total += eng.collect_garbage();  // final sweep, after the audio thread stopped
    collected.store(total, std::memory_order_relaxed);
  });

  // A fourth lane: the UI, reading telemetry while the audio thread publishes it.
  //
  // This is what makes the telemetry block's atomics testable at all. Every
  // field is written by the audio thread every block and read here with no
  // synchronisation beyond relaxed atomics, so if any of them were a plain
  // float or uint64_t this is where TSan would say so.
  std::thread ui([&] {
    std::uint64_t reads = 0;
    bool saw_playing = false;
    while (!audio_done.load(std::memory_order_acquire)) {
      const engine::Telemetry state = eng.telemetry();
      saw_playing = saw_playing || state.playing;
      // Touch every field, so nothing is elided and every atomic is genuinely
      // read on this thread.
      volatile float sink = state.master_peak;
      for (const float level : state.pad_peak) {
        sink += level;
      }
      // The glow block too, and for the same reason: an atomic nobody reads on a
      // second thread is an atomic TSan has no opinion about, so a plain uint32_t
      // there would go unreported.
      for (const engine::PadGlow& glow : state.pad_glow) {
        sink += glow.seconds_since_trigger + glow.velocity;
        sink += glow.triggered ? 1.0F : 0.0F;
      }
      static_cast<void>(sink);
      static_cast<void>(state.playhead_frame);
      ++reads;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    telemetry_reads.store(reads, std::memory_order_relaxed);
    telemetry_saw_playing.store(saw_playing, std::memory_order_relaxed);
  });

  audio.join();
  control.join();
  janitor.join();
  ui.join();

  CHECK(eng.frames_rendered() == static_cast<std::uint64_t>(kBlocks) * kBlockFrames);

  // Anti-vacuity guard: a UI thread that never ran would leave TSan nothing to
  // inspect on this path. The read count is the right guard because the loop
  // touches every telemetry field unconditionally -- TSan instruments the loads,
  // not their values.
  CHECK(telemetry_reads.load(std::memory_order_relaxed) > 0);

  // Deliberately reported, not asserted. Whether a 200 us poll ever lands while
  // a ~2-block voice is sounding depends entirely on the scheduler: the audio
  // thread here renders as fast as it can rather than in real time, so the whole
  // run can be over in a few milliseconds. Asserting it produced a test that
  // passed on dev, asan and tsan and failed on ubsan for no reason but timing.
  INFO("telemetry polls: " << telemetry_reads.load(std::memory_order_relaxed) << ", saw a voice: "
                           << telemetry_saw_playing.load(std::memory_order_relaxed));

  // Quiesce the engine before counting references, on this thread, with every
  // other one joined and nothing left in the event ring.
  //
  // This is not tidying: without it the reference count below is a scheduling
  // outcome rather than a property. The control thread can land a trigger that
  // the audio thread drains in its very last block, which leaves a voice
  // sounding and holding a Sample reference when render() stops being called —
  // and the count is then 3, not 2, through nobody's fault. Rendering until the
  // pool is empty makes the end state the same on every run.
  //
  // The samples here are at most 450 frames against a 256-frame block, so two
  // blocks drain any voice; the bound is far above that and is asserted rather
  // than assumed, so a pool that never empties fails loudly instead of quietly
  // passing the checks below.
  constexpr std::size_t kQuiesceBlocks = 32;
  std::size_t quiesce_blocks = 0;
  while (eng.active_voices() > 0 && quiesce_blocks < kQuiesceBlocks) {
    eng.render(std::span<float* const>{channels}, kBlockFrames);
    ++quiesce_blocks;
  }
  REQUIRE(eng.active_voices() == 0);

  // Nothing must be left holding a reference: every retired sample reached the
  // janitor, and no voice is stuck waiting for ring space.
  static_cast<void>(eng.collect_garbage());
  CHECK(eng.collect_garbage() == 0);

  // The strongest assertion available here: every reference a voice ever took
  // has been released, and released by the janitor. Each sample is down to two
  // owners -- this vector, and the one PadConfig the engine holds for that pad.
  for (const std::shared_ptr<const rt::Sample>& sample : loaded) {
    CHECK(sample.use_count() == 2);
  }

  // Guards against the whole test passing while doing nothing. If the control
  // thread never got an event through, or no voice ever finished, the assertions
  // above are all trivially true and TSan had nothing to inspect.
  CHECK(events_sent.load(std::memory_order_relaxed) > 0);
  CHECK(collected.load(std::memory_order_relaxed) > 0);

  // Deliberately NOT asserting garbage_overflows() == 0.
  //
  // How many triggers land, and how far behind the janitor falls, depends on how
  // the scheduler interleaves three threads — so an overflow count is not a
  // property this test can pin down. When the control thread does outrun the
  // janitor, the garbage ring fills, retire() starts refusing, and the pool
  // correctly declines to steal rather than destroying a Sample on the audio
  // thread. That is the back-pressure mechanism working.
  //
  // What must hold under any load is that nothing is lost or freed on the wrong
  // thread, which is what the use_count checks above establish. Zero overflow at
  // a bounded, realistic rate is asserted by the deterministic test below.
}

TEST_CASE("Engine reassigns pads while the audio thread is rendering", "[stress]") {
  // The M3 acceptance criterion: "a pad reassigned while the stream runs makes
  // no allocation on the audio thread and destroys nothing there".
  //
  // This is the TSan half. The RT-guard half — that the swap and the retire
  // allocate nothing — is in rt_safety_test.cpp, because allocation detection is
  // compiled out under TSan and the two halves therefore cannot live in the same
  // run (see rt_scope.hpp).
  //
  // Before M3 this test could not have been written at all: the audio thread
  // read the pad table with no synchronisation, so writing to it from another
  // thread was a data race by construction. That it is now writable from the
  // control thread mid-render IS the feature.
  const engine::Engine::Config config{.sample_rate = 48'000,
                                      .num_channels = kChannels,
                                      .max_block_frames = kBlockFrames,
                                      .seed = 0};
  engine::Engine eng{config};

  std::vector<std::vector<float>> storage(kChannels, std::vector<float>(kBlockFrames, 0.0F));
  std::vector<float*> channels(kChannels);
  for (std::uint16_t i = 0; i < kChannels; ++i) {
    channels[i] = storage[i].data();
  }

  std::atomic<bool> audio_done{false};
  std::atomic<std::uint64_t> published{0};
  std::atomic<std::size_t> collected{0};

  std::thread audio([&] {
    for (std::size_t block = 0; block < kBlocks; ++block) {
      eng.render(std::span<float* const>{channels}, kBlockFrames);
    }
    audio_done.store(true, std::memory_order_release);  // release: see the first test
  });

  // The control thread builds a brand-new Sample and PadConfig for every publish
  // — allocating freely, which is the entire point of doing it here — and hands
  // it over while the audio thread is mid-flight.
  std::thread control([&] {
    std::uint64_t sent = 0;
    while (!audio_done.load(std::memory_order_acquire) && sent < 400) {
      const auto pad = static_cast<std::uint8_t>(sent % rt::kNumPads);
      if (eng.set_pad_sample(pad, make_short_sample(44'100U, 200 + (sent % 100)))) {
        ++sent;
        // Also play the pad we just replaced, so voices are holding references
        // to configs that are being displaced underneath them.
        static_cast<void>(
            eng.trigger_pad(rt::PadEvent{.pad = pad, .velocity = 0.5F, .frame_offset = 0}));
      }
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    published.store(sent, std::memory_order_relaxed);
  });

  std::thread janitor([&] {
    std::size_t total = 0;
    while (!audio_done.load(std::memory_order_acquire)) {
      total += eng.collect_garbage();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    total += eng.collect_garbage();
    collected.store(total, std::memory_order_relaxed);
  });

  audio.join();
  control.join();
  janitor.join();

  // Anti-vacuity: a control thread that published nothing, or an audio thread
  // that never adopted anything, would leave TSan nothing to look at and every
  // assertion below trivially true.
  CHECK(published.load(std::memory_order_relaxed) > 0);
  CHECK(collected.load(std::memory_order_relaxed) > 0);

  // Every pad the control thread actually reached reads back as loaded. Bounded
  // by the publish count rather than asserted for all sixteen: the audio thread
  // renders as fast as it can rather than in real time, so how far round the
  // grid the control thread gets before it stops is a scheduling outcome, not a
  // property. This is the control-side view, which is deliberately one step
  // ahead of the audio thread's — see Engine::pad_config.
  const auto reached = static_cast<std::uint8_t>(
      std::min<std::uint64_t>(published.load(std::memory_order_relaxed), rt::kNumPads));
  for (std::uint8_t pad = 0; pad < reached; ++pad) {
    CHECK(eng.pad_config(pad) != nullptr);
  }

  // Deliberately reported rather than asserted, for the same reason as
  // garbage_overflows() in the first test: the audio thread here renders as fast
  // as it can rather than in real time, so it can finish while the control
  // thread is still publishing, and the last publish or two then find a ring
  // nobody is draining any more. That is the back-pressure mechanism working,
  // not a defect — and the caller keeping ownership on refusal is asserted
  // directly in handoff_ring_test.cpp, where it is a property rather than a race.
  INFO("pad configs published: " << published.load(std::memory_order_relaxed)
                                 << ", refused: " << eng.rejected_pad_configs()
                                 << ", garbage overflows: " << eng.garbage_overflows());
}

TEST_CASE("Engine does not overflow the garbage ring at a realistic rate", "[integration]") {
  // Single-threaded and deterministic, so this is an assertion about capacity
  // rather than about scheduling. The rate here is already aggressive: a hit on
  // every block at 256 frames / 48 kHz is ~188 hits per second, far faster than
  // a player or a 16th-note pattern at any sane tempo.
  const engine::Engine::Config config{.sample_rate = 48'000,
                                      .num_channels = kChannels,
                                      .max_block_frames = kBlockFrames,
                                      .seed = 0};
  engine::Engine eng{config};
  for (std::uint8_t pad = 0; pad < 4; ++pad) {
    REQUIRE(eng.set_pad_sample(pad, make_short_sample(44'100U + pad, 300 + (pad * 50U))));
  }

  std::vector<std::vector<float>> storage(kChannels, std::vector<float>(kBlockFrames, 0.0F));
  std::vector<float*> channels(kChannels);
  for (std::uint16_t i = 0; i < kChannels; ++i) {
    channels[i] = storage[i].data();
  }

  std::size_t collected = 0;
  for (std::size_t block = 0; block < 5'000; ++block) {
    const rt::PadEvent event{
        .pad = static_cast<std::uint8_t>(block % 4), .velocity = 1.0F, .frame_offset = 0};
    REQUIRE(eng.trigger_pad(event));
    eng.render(std::span<float* const>{channels}, kBlockFrames);

    // The janitor lagging by a few blocks is normal; it runs on the UI thread,
    // which wakes far less often than the audio callback.
    if (block % 4 == 3) {
      collected += eng.collect_garbage();
    }
  }
  collected += eng.collect_garbage();

  CHECK(collected > 0);
  CHECK(eng.garbage_overflows() == 0);
  CHECK(eng.dropped_events() == 0);
  CHECK(eng.dropped_triggers() == 0);
}
