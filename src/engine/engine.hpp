#ifndef CRATEDIG_ENGINE_ENGINE_HPP
#define CRATEDIG_ENGINE_ENGINE_HPP

#include "rt/garbage_ring.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "rt/spsc_ring.hpp"
#include "rt/voice_pool.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace engine {

// The engine facade. Everything audible eventually happens behind render().
//
// This header must never see a device API. RtAudio and RtMidi live in src/io/
// and are the only place allowed to include their headers (CLAUDE.md). Keeping
// the engine device-free is what makes offline bounce, e2e tests, and Docker CI
// possible: render() works in a plain loop with no audio hardware present.
//
// Threading, in one place:
//   - CONTROL calls set_pad_sample() before the stream starts, then trigger_pad()
//     while it runs. Nothing else.
//   - AUDIO calls render(). It allocates nothing, locks nothing, and never runs
//     a Sample destructor.
//   - JANITOR calls collect_garbage(). The engine spawns no threads of its own;
//     whoever owns the run loop decides when collection happens, which is what
//     keeps offline rendering single-threaded and reproducible.
class Engine {
 public:
  // Sixteen voices is the M1 budget, matching the pad count so a full grid can
  // sound at once. The perf target (64 voices at 64 frames) is M8's problem.
  static constexpr std::size_t kMaxVoices = 16;

  // Deep enough to swallow a burst of key repeats or a dense MIDI chord between
  // two audio blocks without dropping anything.
  static constexpr std::size_t kEventRingCapacity = 256;

  // Sized well above kMaxVoices: every voice could finish in the same block, and
  // the janitor may be a whole UI frame behind.
  static constexpr std::size_t kGarbageRingCapacity = 64;

  struct Config {
    std::uint32_t sample_rate = 48'000;
    std::uint16_t num_channels = 2;
    std::uint32_t max_block_frames = 2'048;

    // Anchors the determinism contract (docs/TESTING.md). Nothing consumes it
    // until the first stochastic element lands — humanize, noise, dither in M6 —
    // but it is part of the signature from the start so that "same seed, same
    // input, same bytes" is a promise the API always made, not one retrofitted
    // after something started varying.
    std::uint64_t seed = 0;
  };

  explicit Engine(const Config& config) noexcept;

  // CONTROL THREAD, BEFORE THE AUDIO STREAM STARTS.
  //
  // Not safe to call while render() may be running: the audio thread reads this
  // table without synchronisation, which is what keeps triggering free of
  // atomics. Hot-swapping a loaded pad needs a publish protocol that arrives
  // with the ingest path in M2/M3; offering it now would be an API the tests
  // could not honestly check.
  void set_pad_sample(std::uint8_t pad, std::shared_ptr<const rt::Sample> sample) noexcept;

  [[nodiscard]] std::shared_ptr<const rt::Sample> pad_sample(std::uint8_t pad) const noexcept;

  // CONTROL THREAD. Queues a pad hit for the next block.
  //
  // Returns false if the ring is full, in which case the event is dropped and
  // dropped_events() counts it. Blocking here would push the caller's latency
  // into the UI; silently succeeding would hide a real overload.
  [[nodiscard]] bool trigger_pad(const rt::PadEvent& event) noexcept;

  // REAL-TIME. Called from the audio callback; opens its own RT_SCOPE.
  //
  // channels is a span of per-channel pointers (the CLAUDE.md `float** out`
  // reconciled with the std::span rule) — channels.size() must equal
  // config.num_channels, and each pointer must address at least num_frames
  // floats. num_frames must not exceed config.max_block_frames. All three are
  // debug-asserted; in release they are the caller's contract.
  void render(std::span<float* const> channels, std::size_t num_frames) noexcept;

  // JANITOR THREAD. Destroys everything the audio thread has retired, and
  // returns how many references were released.
  std::size_t collect_garbage() noexcept;

  [[nodiscard]] std::uint64_t frames_rendered() const noexcept { return m_frames_rendered; }

  [[nodiscard]] const Config& config() const noexcept { return m_config; }

  [[nodiscard]] std::size_t active_voices() const noexcept { return m_voices.active_count(); }

  // Diagnostics. Any of these being non-zero after a normal session is a bug
  // somewhere, so they are exposed rather than merely counted.
  [[nodiscard]] std::uint64_t dropped_events() const noexcept { return m_dropped_events; }

  [[nodiscard]] std::uint64_t dropped_triggers() const noexcept { return m_dropped_triggers; }

  [[nodiscard]] std::uint64_t garbage_overflows() const noexcept {
    return m_garbage.overflow_count();
  }

 private:
  Config m_config;
  std::uint64_t m_frames_rendered = 0;

  std::array<std::shared_ptr<const rt::Sample>, rt::kNumPads> m_pads{};

  rt::SpscRing<rt::PadEvent, kEventRingCapacity> m_events;
  rt::VoicePool<kMaxVoices> m_voices;
  rt::GarbageRing<kGarbageRingCapacity> m_garbage;

  // Written by the control thread only.
  std::uint64_t m_dropped_events = 0;
  // Written by the audio thread only.
  std::uint64_t m_dropped_triggers = 0;
};

}  // namespace engine

#endif  // CRATEDIG_ENGINE_ENGINE_HPP
