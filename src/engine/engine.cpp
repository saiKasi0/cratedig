#include "engine/engine.hpp"

#include "rt/rt_scope.hpp"

#include <algorithm>
#include <cassert>
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
    if (!m_voices.trigger(sample, event.velocity, m_config.sample_rate, m_garbage)) {
      m_dropped_triggers.fetch_add(1, std::memory_order_relaxed);
    }
  }

  m_voices.render_add(channels, num_frames);

  // After rendering, so a voice that ended inside this block is handed over in
  // the same block it finished. Failure here is not an error: the voice keeps
  // its reference and we retry next block (see VoicePool::reclaim).
  static_cast<void>(m_voices.reclaim(m_garbage));

  // relaxed: publishes progress to the UI's poll loop; nothing else is ordered
  // by it, and the audio thread is the only writer.
  m_frames_rendered.store(m_frames_rendered.load(std::memory_order_relaxed) + num_frames,
                          std::memory_order_relaxed);
}

std::size_t Engine::collect_garbage() noexcept {
  return m_garbage.collect();
}

}  // namespace engine
