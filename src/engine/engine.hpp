#ifndef CRATEDIG_ENGINE_ENGINE_HPP
#define CRATEDIG_ENGINE_ENGINE_HPP

#include "rt/arch.hpp"
#include "rt/garbage_ring.hpp"
#include "rt/handoff_ring.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "rt/spsc_ring.hpp"
#include "rt/voice_pool.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace engine {

// How long ago a pad was last hit, and how hard.
//
// The signal the pad grid lights from. NOT the peak level, which is already
// published and already decays and is nevertheless the wrong answer: peak
// follows the audio, so a quiet sample barely lights its pad and a pad triggered
// into near-silence does not light at all. What a player wants to see is that
// the machine received the hit -- an acknowledgement, not a meter.
struct PadGlow {
  // Since the most recent trigger. Grows without bound; the interface decides
  // how fast to fade, because that is a look rather than an engine fact.
  float seconds_since_trigger = 0.0F;

  // The velocity of that trigger, in [0, 1].
  float velocity = 0.0F;

  // False until the pad has been hit at least once. Distinguishes "hit a long
  // time ago" from "never hit", which otherwise look identical.
  bool triggered = false;
};

// What the interface needs to know about what the audio thread is doing, as one
// consistent snapshot.
//
// None of this feeds back into the audio path, so it is deliberately NOT part of
// the determinism contract: the peak fall rate depends on the block size, which
// varies with the device, while render()'s output does not.
struct Telemetry {
  // Position within the sample of the most recently started voice still
  // sounding, and which pad it came from. `playing` is false when nothing is.
  std::uint64_t playhead_frame = 0;
  std::uint8_t playhead_pad = 0;
  bool playing = false;

  // Loudest absolute output sample, with a fall time so a meter read at 30 Hz
  // does not show whichever 5 ms block it happened to land on.
  float master_peak = 0.0F;

  // Per-pad level, same fall behaviour. Indexed by pad.
  std::array<float, rt::kNumPads> pad_peak{};

  // Per-pad trigger acknowledgement. Indexed by pad. See PadGlow above for why
  // this is not the same thing as pad_peak.
  std::array<PadGlow, rt::kNumPads> pad_glow{};
};

// The engine facade. Everything audible eventually happens behind render().
//
// This header must never see a device API. RtAudio and RtMidi live in src/io/
// and are the only place allowed to include their headers (CLAUDE.md). Keeping
// the engine device-free is what makes offline bounce, e2e tests, and Docker CI
// possible: render() works in a plain loop with no audio hardware present.
//
// Threading, in one place:
//   - CONTROL calls publish_pad_config() and trigger_pad(), at any time, running
//     or not. Nothing else.
//   - AUDIO calls render(). It allocates nothing, locks nothing, and never runs
//     a Sample or PadConfig destructor.
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

  // Pad reconfigurations in flight, control -> audio. Sixteen is one full grid
  // reassigned between two blocks, which is what `:chop transient` does; a
  // human editing pads cannot outrun a 5 ms block, so this is a burst allowance
  // rather than a throughput budget.
  static constexpr std::size_t kPadHandoffCapacity = 16;

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

  // CONTROL THREAD, AT ANY TIME — running or not.
  //
  // Hands the audio thread a complete replacement for one pad. The config is
  // built here, where allocation and file I/O are legal; the audio thread swaps
  // a pointer and retires what it displaced. Nothing is constructed or destroyed
  // on the audio thread, which is what makes this safe mid-stream.
  //
  // This is the protocol M6's recording and M8's plugin chains reuse unchanged
  // (docs/ARCHITECTURE.md, "Live reconfiguration: one problem, one protocol").
  //
  // Returns false if the handoff ring is full, in which case THE CALLER STILL
  // OWNS `config` and the pad is unchanged — the edit did not happen, and
  // saying so is better than dropping it silently. `config` must be non-null and
  // name a pad below kNumPads; anything else is refused here rather than on the
  // audio thread, so a bad index costs a control-thread branch and not a block.
  [[nodiscard]] bool publish_pad_config(std::shared_ptr<const rt::PadConfig> config) noexcept;

  // CONTROL THREAD. Convenience for the common case: play this whole sample on
  // this pad, everything else default.
  //
  // Before M3 this wrote the pad table directly and was documented pre-start
  // only. It now goes through publish_pad_config() like everything else, so the
  // caveat is gone.
  [[nodiscard]] bool set_pad_sample(std::uint8_t pad,
                                    std::shared_ptr<const rt::Sample> sample) noexcept;

  // CONTROL THREAD. What this thread has most recently published for a pad —
  // NOT what the audio thread is currently using, which may still be one block
  // behind.
  //
  // That distinction is the honest one for a UI: the interface should show the
  // edit the moment it is made, and the block boundary is not a fact about the
  // pad. Reading the audio thread's table from here would be a data race, which
  // is exactly the problem this whole task exists to fix.
  [[nodiscard]] std::shared_ptr<const rt::PadConfig> pad_config(std::uint8_t pad) const noexcept;

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

  // Atomic because the audio thread writes it and the UI reads it live. Relaxed
  // on both sides: it orders nothing, it is a progress counter. Making it a
  // plain uint64_t was a real data race, and it did not present as a torn value
  // -- the compiler simply hoisted the load out of a poll loop, so a caller
  // watching for progress saw zero forever while the callback ran normally.
  [[nodiscard]] std::uint64_t frames_rendered() const noexcept {
    return m_published.frames_rendered.load(std::memory_order_relaxed);
  }

  // ANY THREAD. What the audio thread published at the end of its last block.
  //
  // Every field is a relaxed atomic: the UI wants a recent value, not a
  // synchronised one, and acquiring here would put a barrier in the audio
  // thread's hot path to make a meter one frame fresher.
  [[nodiscard]] Telemetry telemetry() const noexcept;

  // Full scale to silence in this long, when nothing louder arrives. Fast enough
  // to follow a drum pattern, slow enough that a 30 Hz UI never samples the gap
  // between two hits and reports silence.
  static constexpr float kPeakFallSeconds = 0.4F;

  [[nodiscard]] const Config& config() const noexcept { return m_config; }

  [[nodiscard]] std::size_t active_voices() const noexcept { return m_voices.active_count(); }

  // Diagnostics. Any of these being non-zero after a normal session is a bug
  // somewhere, so they are exposed rather than merely counted.
  [[nodiscard]] std::uint64_t dropped_events() const noexcept { return m_dropped_events; }

  // Also audio-thread-written and UI-read; same reasoning as frames_rendered().
  [[nodiscard]] std::uint64_t dropped_triggers() const noexcept {
    return m_published.dropped_triggers.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t garbage_overflows() const noexcept {
    return m_garbage.overflow_count();
  }

  // publish_pad_config() calls refused because the handoff ring was full. A
  // non-zero value means pad edits were rejected — either nothing is rendering,
  // so the ring never drains, or something is republishing in a loop.
  [[nodiscard]] std::uint64_t rejected_pad_configs() const noexcept {
    return m_pad_handoff.rejected_count();
  }

 private:
  // The playhead is pad and frame packed into ONE 64-bit word rather than two
  // atomics, so the UI can never read a frame position from one voice with the
  // pad label of another. Low 56 bits are the frame -- 2.3e16 of them, about
  // 15 000 years at 48 kHz -- and the top 8 are the pad.
  static constexpr std::uint64_t kNothingPlaying = std::numeric_limits<std::uint64_t>::max();
  static constexpr std::uint32_t kPlayheadPadShift = 56;
  static constexpr std::uint64_t kPlayheadFrameMask = (std::uint64_t{1} << kPlayheadPadShift) - 1;

  // Glow is likewise ONE packed word per pad rather than two atomics: age in the
  // low 24 bits, quantised velocity in the top 8. Same reasoning as the playhead
  // -- two atomics would let the UI pair one hit's age with another's velocity,
  // and a pad that flashes at the wrong brightness is a visible wrong answer
  // rather than a rounding.
  //
  // 24 bits of frames is 349 seconds at 48 kHz, far past any glow. The count
  // saturates one short of the mask so that the all-ones sentinel below stays
  // unreachable however long the program runs.
  static constexpr std::uint32_t kGlowVelocityShift = 24;
  static constexpr std::uint32_t kGlowFrameMask = (std::uint32_t{1} << kGlowVelocityShift) - 1;
  static constexpr std::uint32_t kGlowFrameMax = kGlowFrameMask - 1;
  static constexpr std::uint32_t kNeverTriggered = 0xFFFF'FFFFU;

  // Everything the audio thread writes and the UI reads, on its own cache lines.
  //
  // Grouped rather than scattered through the class because these are written
  // every block and the rings below are touched by the control thread: left
  // interleaved, a UI poll would keep invalidating the line the event ring lives
  // on. Same reason CLAUDE.md asks for alignas(kCacheLine) on shared state.
  struct alignas(rt::kCacheLine) Published {
    std::atomic<std::uint64_t> frames_rendered{0};
    std::atomic<std::uint64_t> dropped_triggers{0};

    std::atomic<std::uint64_t> playhead{kNothingPlaying};

    std::atomic<float> master_peak{0.0F};
    std::array<std::atomic<float>, rt::kNumPads> pad_peak{};

    // Written on trigger and aged once per block. Initialised in the Engine
    // constructor rather than here, because a default-constructed
    // std::atomic<uint32_t> is zero and zero means "hit just now at velocity 0".
    std::array<std::atomic<std::uint32_t>, rt::kNumPads> pad_glow{};
  };

  // AUDIO THREAD, at the top of every block. Adopts whatever the control thread
  // has published and retires what it displaced.
  void adopt_pad_configs() noexcept;

  // AUDIO THREAD, once per block, after rendering.
  void publish_telemetry(std::span<float* const> channels, std::size_t num_frames) noexcept;

  Config m_config;
  Published m_published;

  // AUDIO THREAD ONLY. Read every block, written only by adopt_pad_configs().
  // No synchronisation is needed on it precisely because the control thread
  // never touches it — that is the whole point of the handoff ring.
  std::array<std::shared_ptr<const rt::PadConfig>, rt::kNumPads> m_pads{};

  // AUDIO THREAD ONLY, and a MEMBER rather than a local in adopt_pad_configs()
  // on purpose.
  //
  // A displaced config that the garbage ring refuses has to survive to the next
  // block: it cannot stay on the audio thread's stack, because letting a local
  // shared_ptr go out of scope there would run the destructor this whole
  // mechanism exists to avoid. Holding it here means the retry is free and the
  // failure mode is "one block late", not "freed in the callback".
  std::shared_ptr<const rt::PadConfig> m_retiring;

  rt::SpscRing<rt::PadEvent, kEventRingCapacity> m_events;
  rt::HandoffRing<rt::PadConfig, kPadHandoffCapacity> m_pad_handoff;
  rt::VoicePool<kMaxVoices> m_voices;
  rt::GarbageRing<kGarbageRingCapacity> m_garbage;

  // CONTROL THREAD ONLY: what this thread has published, so pad_config() can
  // answer without reading the audio thread's table. Not a cache of m_pads — it
  // is deliberately one block *ahead* of it.
  std::array<std::shared_ptr<const rt::PadConfig>, rt::kNumPads> m_published_pads{};

  // Written by the control thread only, and read by it -- no cross-thread access,
  // so no atomic.
  std::uint64_t m_dropped_events = 0;
};

}  // namespace engine

#endif  // CRATEDIG_ENGINE_ENGINE_HPP
