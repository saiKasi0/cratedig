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

bool Engine::publish_pad_config(std::shared_ptr<const rt::PadConfig> config) noexcept {
  if (config == nullptr || config->pad >= rt::kNumPads) {
    return false;
  }
  const std::uint8_t pad = config->pad;

  // The control-side view is updated only on success, so a rejected publish
  // leaves pad_config() reporting what is actually loaded rather than an edit
  // that never reached the audio thread.
  if (!m_pad_handoff.try_publish(std::shared_ptr<const rt::PadConfig>{config})) {
    return false;
  }
  m_published_pads[pad] = std::move(config);
  return true;
}

bool Engine::set_pad_sample(std::uint8_t pad, std::shared_ptr<const rt::Sample> sample) noexcept {
  if (pad >= rt::kNumPads) {
    return false;
  }
  // make_shared allocates. That is fine and expected: this is the control
  // thread, which is where building the new object is supposed to happen.
  return publish_pad_config(std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = std::move(sample), .pad = pad}));
}

std::shared_ptr<const rt::PadConfig> Engine::pad_config(std::uint8_t pad) const noexcept {
  if (pad >= rt::kNumPads) {
    return nullptr;
  }
  return m_published_pads[pad];
}

std::shared_ptr<const rt::Sample> Engine::pad_sample(std::uint8_t pad) const noexcept {
  const std::shared_ptr<const rt::PadConfig> config = pad_config(pad);
  return config == nullptr ? nullptr : config->sample;
}

void Engine::adopt_pad_configs() noexcept {
  // A handle held over from a previous block, because the garbage ring was full
  // when we tried to retire it. Nothing else can proceed until it is gone: the
  // only other place to put it would be this thread's stack, and letting it die
  // there is the one outcome the whole mechanism exists to prevent.
  if (m_retiring != nullptr && !m_garbage.retire(std::move(m_retiring))) {
    return;
  }

  while (m_pad_handoff.try_take(m_retiring)) {
    // Read BEFORE the swap: afterwards m_retiring points at the displaced
    // config, whose pad is not necessarily this one.
    //
    // Validated on the control thread already; re-checked here because m_pads
    // is indexed by it and a bad index would be a buffer overrun rather than a
    // silent nothing.
    const std::uint8_t pad = m_retiring->pad;
    if (pad < rt::kNumPads) {
      // The swap IS the reconfiguration: one pointer exchange, no allocation, no
      // lock, and afterwards m_retiring holds the OLD config instead of the new
      // one. Everything before this line was preparation and everything after
      // is cleanup.
      m_retiring.swap(m_pads[pad]);
    }

    // retire() of a null handle succeeds and consumes nothing, which is the
    // common case the first time a pad is loaded.
    if (!m_garbage.retire(std::move(m_retiring))) {
      return;  // hold it and try again next block
    }
  }
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

  // Reconfigurations before triggers, so a pad assigned and played in the same
  // UI frame sounds the new material rather than one block of the old.
  adopt_pad_configs();

  // Drain first, so a hit that arrived during the previous block sounds in this
  // one. M1 starts every voice at the block boundary; PadEvent::frame_offset is
  // where M4 will make that sample-accurate.
  rt::PadEvent event{};
  while (m_events.try_pop(event)) {
    if (event.pad >= rt::kNumPads) {
      continue;  // came from a key handler or MIDI; not trusted
    }
    const std::shared_ptr<const rt::PadConfig>& config = m_pads[event.pad];
    if (config == nullptr || config->sample == nullptr) {
      continue;  // an unloaded pad is silent, not an error
    }
    const std::shared_ptr<const rt::Sample>& sample = config->sample;
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
