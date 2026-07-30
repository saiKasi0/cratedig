#include "engine/engine.hpp"

#include "rt/rt_scope.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace engine {

Engine::Engine(const Config& config) noexcept : m_config(config) {}

void Engine::set_pad_sample(std::uint8_t pad, std::shared_ptr<const rt::Sample> sample) noexcept {
  if (pad >= rt::kNumPads) {
    return;
  }
  m_pads[pad] = std::move(sample);
}

std::shared_ptr<const rt::Sample> Engine::pad_sample(std::uint8_t pad) const noexcept {
  if (pad >= rt::kNumPads) {
    return nullptr;
  }
  return m_pads[pad];
}

bool Engine::trigger_pad(const rt::PadEvent& event) noexcept {
  if (m_events.try_push(event)) {
    return true;
  }
  ++m_dropped_events;
  return false;
}

void Engine::render(std::span<float* const> channels, std::size_t num_frames) noexcept {
  // Opened first, so everything below is held to the real-time rules — including
  // anything added here later.
  RT_SCOPE();

  assert(channels.size() == m_config.num_channels && "render: channel count mismatch");
  assert(num_frames <= m_config.max_block_frames && "render: block larger than max_block_frames");

  for (float* channel : channels) {
    assert(channel != nullptr && "render: null channel pointer");
    // std::fill rather than memset: writing 0.0f is the intent, and for IEEE-754
    // floats it is exactly the all-zero bit pattern, so this is also the fastest
    // form the compiler can pick.
    std::fill_n(channel, num_frames, 0.0F);
  }

  // Drain first, so a hit that arrived during the previous block sounds in this
  // one. M1 starts every voice at the block boundary; PadEvent::frame_offset is
  // where M4 will make that sample-accurate.
  rt::PadEvent event{};
  while (m_events.try_pop(event)) {
    if (event.pad >= rt::kNumPads) {
      continue;  // came from a key handler or MIDI; not trusted
    }
    const std::shared_ptr<const rt::Sample>& sample = m_pads[event.pad];
    if (sample == nullptr) {
      continue;  // an unloaded pad is silent, not an error
    }
    // Copying the shared_ptr is an atomic increment — allocation-free and
    // lock-free, so it is legal here. The voice releases it through the garbage
    // ring, never by dropping it on this thread.
    if (!m_voices.trigger(sample, event.velocity, m_config.sample_rate, m_garbage, event.pad)) {
      m_published.dropped_triggers.fetch_add(1, std::memory_order_relaxed);
    }
  }

  m_voices.render_add(channels, num_frames);

  // Before reclaim(), which clears finished voices: a voice that ended inside
  // this block still has a position and a level worth showing for one frame.
  publish_telemetry(channels, num_frames);

  // After rendering, so a voice that ended inside this block is handed over in
  // the same block it finished. Failure here is not an error: the voice keeps
  // its reference and we retry next block (see VoicePool::reclaim).
  static_cast<void>(m_voices.reclaim(m_garbage));

  // relaxed: publishes progress to the UI's poll loop; nothing else is ordered
  // by it, and the audio thread is the only writer.
  m_published.frames_rendered.store(
      m_published.frames_rendered.load(std::memory_order_relaxed) + num_frames,
      std::memory_order_relaxed);
}

void Engine::publish_telemetry(std::span<float* const> channels, std::size_t num_frames) noexcept {
  // Linear fall rather than exponential: one multiply and a subtract, no
  // transcendental in the audio callback, and on a small bar meter the
  // difference is not visible. Deriving it from num_frames rather than from a
  // per-block constant keeps the fall time the same whatever block size the
  // device negotiated.
  const auto elapsed = static_cast<float>(num_frames) / static_cast<float>(m_config.sample_rate);
  const float fall = elapsed / kPeakFallSeconds;

  std::array<float, rt::kNumPads> pad_peak{};
  std::uint64_t newest_sequence = 0;
  std::uint64_t playhead = kNothingPlaying;

  for (const rt::Voice& voice : m_voices.voices()) {
    if (voice.pad < rt::kNumPads) {
      pad_peak[voice.pad] = std::max(pad_peak[voice.pad], voice.peak);
    }
    // The newest voice wins the playhead. With one pad it makes no difference;
    // with a chord it means the marker follows the hit you just played rather
    // than whichever slot the loop reached last.
    if (voice.active && (playhead == kNothingPlaying || voice.started_at >= newest_sequence)) {
      newest_sequence = voice.started_at;
      const std::uint64_t frame =
          static_cast<std::uint64_t>(voice.phase >> rt::kPhaseFractionBits) & kPlayheadFrameMask;
      playhead = (static_cast<std::uint64_t>(voice.pad) << kPlayheadPadShift) | frame;
    }
  }

  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    const float previous = m_published.pad_peak[pad].load(std::memory_order_relaxed);
    m_published.pad_peak[pad].store(std::max(pad_peak[pad], previous - fall),
                                    std::memory_order_relaxed);
  }

  float master = 0.0F;
  for (float* channel : channels) {
    for (std::size_t frame = 0; frame < num_frames; ++frame) {
      const float value = channel[frame];
      master = std::max(master, value < 0.0F ? -value : value);
    }
  }
  const float previous_master = m_published.master_peak.load(std::memory_order_relaxed);
  m_published.master_peak.store(std::max(master, previous_master - fall),
                                std::memory_order_relaxed);

  // relaxed everywhere: the UI wants a recent value, not a synchronised one, and
  // an acquire/release pair here would put a barrier in the audio thread's hot
  // path to make a meter one frame fresher.
  m_published.playhead.store(playhead, std::memory_order_relaxed);
}

Telemetry Engine::telemetry() const noexcept {
  Telemetry snapshot;

  const std::uint64_t playhead = m_published.playhead.load(std::memory_order_relaxed);
  snapshot.playing = playhead != kNothingPlaying;
  if (snapshot.playing) {
    snapshot.playhead_frame = playhead & kPlayheadFrameMask;
    snapshot.playhead_pad = static_cast<std::uint8_t>(playhead >> kPlayheadPadShift);
  }

  snapshot.master_peak = m_published.master_peak.load(std::memory_order_relaxed);
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    snapshot.pad_peak[pad] = m_published.pad_peak[pad].load(std::memory_order_relaxed);
  }
  return snapshot;
}

std::size_t Engine::collect_garbage() noexcept {
  return m_garbage.collect();
}

}  // namespace engine
