#include "engine/engine.hpp"

#include "rt/rt_scope.hpp"

#include <algorithm>
#include <cassert>

namespace engine {

Engine::Engine(const Config& config) noexcept : m_config(config) {}

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

  m_frames_rendered += num_frames;
}

}  // namespace engine
