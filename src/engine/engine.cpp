#include "engine/engine.hpp"

#include "rt/rt_scope.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace engine {

Engine::Engine(const Config& config) noexcept : m_config(config) {
  // A default-constructed atomic<uint32_t> is zero, and zero decodes as "hit
  // just now, at velocity 0" -- which would light every pad on the first frame
  // of every session. relaxed: nothing is running yet.
  for (std::atomic<std::uint32_t>& glow : m_published.pad_glow) {
    glow.store(kNeverTriggered, std::memory_order_relaxed);
  }
}

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

bool Engine::publish_sequencer(std::shared_ptr<const rt::SequencerState> state) noexcept {
  if (state == nullptr) {
    return false;
  }
  // Kept before the publish, so a refusal leaves the control-side view matching
  // what the audio thread actually has -- same ordering as publish_pad_config.
  std::shared_ptr<const rt::SequencerState> keep = state;
  if (!m_sequencer_handoff.try_publish(std::move(state))) {
    return false;
  }
  m_published_sequencer = std::move(keep);
  return true;
}

std::shared_ptr<const rt::SequencerState> Engine::sequencer_state() const noexcept {
  return m_published_sequencer;
}

bool Engine::send_transport(const rt::TransportCommand& command) noexcept {
  return m_transport_commands.try_push(command);
}

void Engine::adopt_sequencer() noexcept {
  // The same shape as adopt_pad_configs(), and a member rather than a local for
  // the same reason: a displaced state the garbage ring refuses must survive to
  // the next block instead of dying on this thread's stack.
  if (m_retiring_sequencer != nullptr && !m_garbage.retire(std::move(m_retiring_sequencer))) {
    return;
  }

  while (m_sequencer_handoff.try_take(m_retiring_sequencer)) {
    m_retiring_sequencer.swap(m_sequencer);
    if (!m_garbage.retire(std::move(m_retiring_sequencer))) {
      return;  // hold it and try again next block
    }
  }
}

std::uint32_t Engine::current_step_word() const noexcept {
  if (m_sequencer == nullptr) {
    return 0;
  }
  const rt::SequencerState& state = *m_sequencer;

  // Resolved here rather than in the UI, because this is where both the position
  // and the tempo it was rendered at are known. A UI recomputing it would use
  // whatever tempo it holds now, which after a tempo change is not the one those
  // frames were played at.
  //
  // The same song_position_at() the step scan uses, so the readout and the audio
  // cannot disagree about which pattern is playing -- two implementations of
  // "where are we in the song" would eventually differ, and the one on screen
  // would be the one believed.
  const std::uint64_t absolute =
      rt::step_index_at(m_transport.position_frames, m_config.sample_rate, state.bpm_x100);
  const rt::SongPosition position = rt::song_position_at(state, absolute);

  return ((static_cast<std::uint32_t>(position.pattern) & kTransportFieldMask)
          << kTransportPatternShift) |
         ((static_cast<std::uint32_t>(position.slot) & kTransportFieldMask)
          << kTransportSlotShift) |
         ((static_cast<std::uint32_t>(position.step) & kTransportFieldMask) << kTransportStepShift);
}

void Engine::drain_transport() noexcept {
  rt::TransportCommand command{};
  while (m_transport_commands.try_pop(command)) {
    switch (command.kind) {
      case rt::TransportCommandKind::kStop:
        m_transport.playing = false;
        break;
      case rt::TransportCommandKind::kPlay:
        m_transport.playing = true;
        break;
      case rt::TransportCommandKind::kSeek:
        // Position only. Seeking while stopped must not start playback, and
        // seeking while playing must not stop it -- "play from the top" is a
        // seek followed by a play, which is why these are separate kinds rather
        // than one command with a flag nobody remembers the meaning of.
        m_transport.position_frames = command.position_frames;
        break;
    }
  }
}

void Engine::start_voice(std::uint8_t pad, float velocity, std::size_t frame_offset,
                         bool sequenced) noexcept {
  // One path for every producer -- the keyboard, MIDI, and the sequencer. Shared
  // rather than duplicated because the glow bookkeeping below is easy to get
  // subtly different in three places, and three pads that light differently
  // depending on what triggered them is the kind of bug that gets blamed on the
  // renderer.
  if (pad >= rt::kNumPads) {
    return;
  }
  const std::shared_ptr<const rt::PadConfig>& config = m_pads[pad];
  if (config == nullptr || config->sample == nullptr) {
    return;  // an unloaded pad is silent, not an error
  }

  // Copying the shared_ptr is an atomic increment — allocation-free and
  // lock-free, so it is legal here. The voice releases it through the garbage
  // ring, never by dropping it on this thread.
  if (!m_voices.trigger(config, velocity, m_config.sample_rate, m_garbage, frame_offset)) {
    m_published.dropped_triggers.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Glow is published from the TRIGGER, not from the audio it produces. That
  // is the whole point: the pad lights because the machine received the hit,
  // at whatever level the material happens to be. Aged to zero here and
  // advanced by publish_telemetry at the end of this same block.
  //
  // relaxed: the UI wants a recent value, not a synchronised one.
  //
  // std::lround rather than the (x + 0.5) truncation idiom: the idiom is
  // wrong for negatives, and although velocity is clamped non-negative here,
  // that is a fact about this line rather than about the reader. Once per
  // trigger, not per frame, and it allocates nothing and cannot throw.
  const auto quantised =
      static_cast<std::uint32_t>(std::lround(std::clamp(velocity, 0.0F, 1.0F) * 255.0F));
  const std::uint32_t word =
      (quantised << kGlowVelocityShift) | (sequenced ? kGlowSequencedBit : 0U);
  m_published.pad_glow[pad].store(word, std::memory_order_relaxed);
}

template <typename Ring>
void Engine::drain_pad_events(Ring& ring, std::size_t num_frames) noexcept {
  rt::PadEvent event{};
  while (ring.try_pop(event)) {
    if (event.pad >= rt::kNumPads) {
      continue;  // came from a key handler or MIDI; not trusted
    }

    if (event.kind == rt::PadEventKind::kNoteOff) {
      // Addressed to the PAD, not to a config: the voice decides whether it
      // cares, based on the config it is actually playing. A pad reassigned
      // between the note-on and the note-off must still let go of the note the
      // player is holding.
      m_voices.note_off(event.pad);
      continue;
    }

    // CLAMPED, not trusted and not dropped. frame_offset crossed a thread
    // boundary exactly as event.pad did, and a producer that disagrees with us
    // about the block length would otherwise place a hit past the end of it —
    // where the voice would sound a block late instead. Clamping costs the hit
    // at most a few hundred microseconds; dropping it would lose a note, which
    // is the worse answer for a bad number that is probably off by one.
    const std::size_t offset =
        num_frames == 0 ? 0 : std::min<std::size_t>(event.frame_offset, num_frames - 1);
    start_voice(event.pad, event.velocity, offset, /*sequenced=*/false);
  }
}

void Engine::fire_sequencer_steps(std::size_t num_frames) noexcept {
  if (!m_transport.playing || m_sequencer == nullptr || num_frames == 0) {
    return;
  }
  const rt::SequencerState& state = *m_sequencer;

  // The block this call covers. position_frames is still the START of it --
  // the transport advances after rendering, so this arithmetic sees the block
  // it is about to fill rather than the one it just filled.
  const std::uint64_t block_start = m_transport.position_frames;
  const std::uint64_t block_end = block_start + num_frames;

  // Walk forward from a deliberate under-estimate. step_scan_start() is one
  // lower than the exact index because a swung step can land inside this block
  // while its unswung base sits before it -- starting at the exact index would
  // silently drop that step, and a missing note is the hardest failure to trace
  // back to its cause.
  //
  // The walk terminates because step_frame() is strictly increasing (asserted in
  // sequencer_test.cpp) and a step is never shorter than a frame at the clamped
  // maximum tempo, so it runs at most num_frames + 2 times.
  for (std::uint64_t step = rt::step_scan_start(block_start, m_config.sample_rate, state.bpm_x100);;
       ++step) {
    // The position comes FIRST, because swing is a property of the pattern and a
    // song can chain patterns that swing differently. Resolving it depends only
    // on the step index, never on the frame, so there is no circularity -- but
    // computing the frame first would have to guess a swing, and in a song it
    // would guess wrong at every pattern boundary.
    const rt::SongPosition position = rt::song_position_at(state, step);
    const rt::Pattern& pattern = state.patterns[position.pattern];

    const std::uint64_t at =
        rt::step_frame(step, m_config.sample_rate, state.bpm_x100, pattern.swing);
    if (at >= block_end) {
      break;
    }
    if (at < block_start) {
      continue;  // behind us: the scan started early on purpose
    }

    // Placed inside the block rather than at its edge. This is the whole reason
    // Task 1 existed: quantising to the block boundary would make the groove
    // depend on the device's buffer size, and swing would round away entirely at
    // large blocks.
    const auto offset = static_cast<std::size_t>(at - block_start);

    // Ascending pad order, so which pad wins a steal when the pool is full is a
    // property of the pattern rather than of iteration order.
    for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
      const rt::Step& cell = pattern.steps[position.step][pad];
      if (!cell.on) {
        continue;
      }
      // 0..127 to linear 0..1, converted once here rather than stored as a
      // float: the pattern holds what MIDI and the UI both speak.
      const float velocity = static_cast<float>(cell.velocity) / 127.0F;
      start_voice(static_cast<std::uint8_t>(pad), velocity, offset, /*sequenced=*/true);
    }
  }
}

void Engine::mix_metronome(std::span<float* const> channels, std::size_t num_frames) noexcept {
  if (!m_transport.playing || m_sequencer == nullptr || num_frames == 0) {
    return;
  }
  const rt::SequencerState& state = *m_sequencer;
  if (!state.metronome) {
    return;
  }

  const std::uint64_t block_start = m_transport.position_frames;
  const std::uint64_t block_end = block_start + num_frames;

  // The earliest beat whose click could still be sounding, derived rather than
  // assumed: backing up "one beat" would be correct at every tempo this engine
  // allows, but it would be correct by arithmetic nobody re-checks when the
  // click length changes. Subtracting the click length says what it means.
  const std::uint64_t earliest =
      block_start > rt::kClickFrames ? block_start - rt::kClickFrames : 0;
  std::uint64_t beat =
      rt::step_index_at(earliest, m_config.sample_rate, state.bpm_x100) / rt::kStepsPerBeat;

  for (;; ++beat) {
    // UNSWUNG, deliberately: a beat is a multiple of kStepsPerBeat and therefore
    // an even step, which step_frame() never shifts anyway -- but the zero here
    // is the statement that a metronome marks the grid rather than the groove.
    // A swinging metronome is useless for the thing a metronome is for.
    const std::uint64_t at =
        rt::step_frame(beat * rt::kStepsPerBeat, m_config.sample_rate, state.bpm_x100, 0);
    if (at >= block_end) {
      break;
    }
    const std::uint64_t click_end = at + rt::kClickFrames;
    if (click_end <= block_start) {
      continue;  // finished before this block began
    }

    const bool accent = (beat % rt::kBeatsPerBar) == 0;
    const rt::ClickTable& table = accent ? rt::kClickAccent : rt::kClickBeat;
    const float gain = accent ? rt::kAccentGain : rt::kBeatGain;

    // Clipped to the block at both ends, so a click that straddles a boundary is
    // continued rather than restarted. That is what keeps the metronome
    // block-size invariant with no state carried between blocks: the table index
    // comes from the absolute frame, not from a counter.
    const std::uint64_t from = std::max(at, block_start);
    const std::uint64_t to = std::min(click_end, block_end);
    for (std::uint64_t frame = from; frame < to; ++frame) {
      const float value = table.samples[static_cast<std::size_t>(frame - at)] * gain;
      const auto into = static_cast<std::size_t>(frame - block_start);
      for (float* channel : channels) {
        channel[into] += value;
      }
    }
  }
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

bool Engine::submit_midi_event(const rt::PadEvent& event) noexcept {
  if (m_midi_events.try_push(event)) {
    return true;
  }
  // relaxed: a counter the UI reads for diagnostics; nothing is ordered by it.
  m_dropped_midi_events.fetch_add(1, std::memory_order_relaxed);
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
  adopt_sequencer();

  // Before the position advances, so a seek lands on this block rather than one
  // block late -- which for "play from the top" is the difference between the
  // first step sounding and being skipped.
  drain_transport();

  // Drain first, so a hit that arrived during the previous block sounds in this
  // one, placed inside it by PadEvent::frame_offset.
  //
  // TWO RINGS, drained one after the other: the keyboard's and MIDI's. They
  // carry the same type and mean the same thing, and are separate only because
  // they have different producer THREADS -- see kMidiRingCapacity. Draining
  // MIDI second is arbitrary but fixed, so which one wins a steal on a full
  // voice pool is reproducible rather than a matter of timing.
  drain_pad_events(m_events, num_frames);
  drain_pad_events(m_midi_events, num_frames);
  // The sequencer, AFTER the live events. Both start voices, so the order
  // decides which one wins a steal when the pool is full, and it has to be
  // fixed rather than incidental. Live first is the right way round: a hit the
  // player made is the one they will notice missing.
  fire_sequencer_steps(num_frames);

  m_voices.render_add(channels, num_frames);

  // After the voices and before the position advances: the click is placed by
  // the same block_start the steps were, so it lands on the beat it marks.
  mix_metronome(channels, num_frames);

  // The transport advances by exactly the block it just rendered, whatever that
  // block was. Nothing else tracks time: every step boundary is derived from
  // this number, so there is no second clock to disagree with it.
  if (m_transport.playing) {
    m_transport.position_frames += num_frames;
  }

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

  // Transport, packed. The position is masked rather than asserted: 63 bits is
  // six million years at 48 kHz, so the mask is a guarantee about the encoding
  // rather than a limit anyone reaches.
  const std::uint64_t position = m_transport.position_frames & kTransportFrameMask;
  m_published.transport.store(m_transport.playing ? (position | kTransportPlayingBit) : position,
                              std::memory_order_relaxed);
  m_published.transport_step.store(current_step_word(), std::memory_order_relaxed);

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

  const auto elapsed_frames =
      static_cast<std::uint32_t>(num_frames > kGlowFrameMax ? kGlowFrameMax : num_frames);

  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    const float previous = m_published.pad_peak[pad].load(std::memory_order_relaxed);
    m_published.pad_peak[pad].store(std::max(pad_peak[pad], previous - fall),
                                    std::memory_order_relaxed);

    // Age the glow by this block. Saturating rather than wrapping: a pad hit
    // once and then left alone for six minutes must keep reading "a long time
    // ago" instead of suddenly reading "just now".
    const std::uint32_t glow = m_published.pad_glow[pad].load(std::memory_order_relaxed);
    if (glow == kNeverTriggered) {
      continue;
    }
    const std::uint32_t age = glow & kGlowFrameMask;
    if (age >= kGlowFrameMax) {
      continue;
    }
    const std::uint32_t aged = std::min(age + elapsed_frames, kGlowFrameMax);
    m_published.pad_glow[pad].store((glow & ~kGlowFrameMask) | aged, std::memory_order_relaxed);
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

  const std::uint64_t transport = m_published.transport.load(std::memory_order_relaxed);
  snapshot.transport_playing = (transport & kTransportPlayingBit) != 0;
  snapshot.transport_frames = transport & kTransportFrameMask;

  const std::uint32_t step = m_published.transport_step.load(std::memory_order_relaxed);
  snapshot.transport_step = (step >> kTransportStepShift) & kTransportFieldMask;
  snapshot.transport_slot =
      static_cast<std::uint8_t>((step >> kTransportSlotShift) & kTransportFieldMask);
  snapshot.transport_pattern =
      static_cast<std::uint8_t>((step >> kTransportPatternShift) & kTransportFieldMask);

  snapshot.master_peak = m_published.master_peak.load(std::memory_order_relaxed);
  const auto rate = static_cast<float>(m_config.sample_rate == 0 ? 1U : m_config.sample_rate);
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    snapshot.pad_peak[pad] = m_published.pad_peak[pad].load(std::memory_order_relaxed);

    const std::uint32_t glow = m_published.pad_glow[pad].load(std::memory_order_relaxed);
    if (glow == kNeverTriggered) {
      continue;  // leaves the default-constructed PadGlow, which reads as untriggered
    }
    snapshot.pad_glow[pad].triggered = true;
    const bool sequenced = (glow & kGlowSequencedBit) != 0;
    snapshot.pad_glow[pad].sequenced = sequenced;
    snapshot.pad_glow[pad].velocity = static_cast<float>(glow >> kGlowVelocityShift) / 255.0F;

    // LISTENER TIME. A sequenced hit is not shown until the sound it made has
    // had time to reach the ear; a live one is shown at once, because its
    // reference is the finger that caused it. With the default latency of zero
    // this is exactly the age, which is why nothing about M3's glow behaviour
    // changes until M9 measures a real figure.
    const auto age_frames = static_cast<float>(glow & kGlowFrameMask);
    const float delay = sequenced ? static_cast<float>(m_config.output_latency_frames) : 0.0F;
    snapshot.pad_glow[pad].seconds_since_trigger = (age_frames - delay) / rate;
  }
  return snapshot;
}

std::size_t Engine::collect_garbage() noexcept {
  return m_garbage.collect();
}

}  // namespace engine
