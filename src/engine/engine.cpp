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

  // Gain reduction is UNITY when there is none, and a default-constructed float
  // is zero -- which reads as "this strip is pulling everything down to
  // silence". Both the audio thread's copy and the published one, because the
  // published one is what the interface reads and nothing writes it until the
  // first block renders: an engine that has never rendered would otherwise show
  // sixteen strips crushed flat.
  //
  // BEFORE the early return below, so a degenerate config gets it too. The
  // interface reads telemetry from any engine it is handed, working graph or
  // not.
  m_strip_reduction.fill(1.0F);
  for (std::atomic<float>& reduction : m_published.strip_reduction) {
    reduction.store(1.0F, std::memory_order_relaxed);
  }

  // The mixer graph, allocated ONCE, here, on the control thread. This is the
  // whole reason Config carries max_block_frames: render() asserts against it,
  // so a buffer sized for it can never be overrun by a block the device asks
  // for, and the audio thread never has to consider growing anything.
  const auto channels = static_cast<std::size_t>(m_config.num_channels);
  const auto frames = static_cast<std::size_t>(m_config.max_block_frames);
  if (channels == 0 || frames == 0) {
    // Degenerate config. Leave the graph empty; node_channels() reports that
    // honestly and render()'s assertions catch the real problem, which is the
    // config rather than the graph.
    return;
  }

  m_graph_samples.assign(kNumGraphNodes * channels * frames, 0.0F);
  m_graph_channels.resize(kNumGraphNodes * channels);
  for (std::size_t index = 0; index < m_graph_channels.size(); ++index) {
    m_graph_channels[index] = m_graph_samples.data() + (index * frames);
  }

  // Filter state: one biquad per pad, per band, per channel. Here rather than in
  // StripConfig because a config is immutable and shared -- two voices holding
  // the same config would be sharing filter history, which is not a thing
  // filters can do.
  //
  // Sized from the real channel count rather than capped at two, so there is no
  // arbitrary limit to discover later. 16 x 4 x 2 = 128 sections at the
  // defaults, about 2 KB.
  m_eq_state.assign(rt::kNumPads * rt::kEqBands * channels, rt::Biquad{});

  // The limiter's lookahead delay line, sized for the longest lookahead it will
  // ever be asked for rather than for the current setting -- so changing the
  // lookahead is a different read offset rather than a reallocation.
  m_limiter.prepare(channels);
}

std::span<float* const> Engine::node_channels(std::size_t node) noexcept {
  if (m_graph_channels.empty()) {
    return {};
  }
  const auto channels = static_cast<std::size_t>(m_config.num_channels);
  assert(node < kNumGraphNodes && "node_channels: node out of range");
  return std::span<float* const>{m_graph_channels.data() + (node * channels), channels};
}

void Engine::clear_graph(std::size_t num_frames) noexcept {
  for (float* channel : m_graph_channels) {
    std::fill_n(channel, num_frames, 0.0F);
  }
}

namespace {

// AUDIO THREAD. destination += source, for num_frames of every channel both
// have. Free rather than a member because it knows nothing about the engine:
// it is the one arithmetic operation the whole graph is made of.
void mix_into(std::span<float* const> destination, std::span<float* const> source,
              std::size_t num_frames) noexcept {
  const std::size_t channels = std::min(destination.size(), source.size());
  for (std::size_t channel = 0; channel < channels; ++channel) {
    float* out = destination[channel];
    const float* in = source[channel];
    for (std::size_t frame = 0; frame < num_frames; ++frame) {
      out[frame] += in[frame];
    }
  }
}

// AUDIO THREAD. The fader. Both channels, one scalar.
//
// Applied unconditionally rather than skipped when it is unity. Multiplying by
// exactly 1.0f is bit-exact for every finite value, so the branch would buy a
// few multiplies at the price of a second code path through the strip -- and the
// path taken at unity would then be the one no test with a fader on it covers.
void apply_gain(std::span<float* const> channels, float gain, std::size_t num_frames) noexcept {
  for (float* samples : channels) {
    for (std::size_t frame = 0; frame < num_frames; ++frame) {
      samples[frame] *= gain;
    }
  }
}

// AUDIO THREAD. Balance, on a stereo strip.
//
// A NO-OP AT ANY OTHER CHANNEL COUNT, deliberately. Left and right is what
// balance means; there is no honest reading of it for one channel or for five,
// and inventing one -- "apply left to the even channels" -- would be a rule
// nobody asked for that silences half a mono strip when panned.
void apply_balance(std::span<float* const> channels, float balance,
                   std::size_t num_frames) noexcept {
  if (channels.size() != 2) {
    return;
  }
  const float left = rt::balance_left(balance);
  const float right = rt::balance_right(balance);
  for (std::size_t frame = 0; frame < num_frames; ++frame) {
    channels[0][frame] *= left;
    channels[1][frame] *= right;
  }
}

// AUDIO THREAD. Loudest magnitude in the buffer, across every channel.
//
// Every channel, unlike rt::Voice::peak which measures channel 0 only: a pad
// meter is one bar next to one pad, but a strip that has been panned hard right
// must not read as silent because channel 0 is.
[[nodiscard]] float peak_of(std::span<float* const> channels, std::size_t num_frames) noexcept {
  float peak = 0.0F;
  for (const float* samples : channels) {
    for (std::size_t frame = 0; frame < num_frames; ++frame) {
      const float value = samples[frame];
      peak = std::max(peak, value < 0.0F ? -value : value);
    }
  }
  return peak;
}

}  // namespace

void Engine::apply_eq(std::span<float* const> buffer, const rt::EqConfig& eq, std::size_t pad,
                      std::size_t num_frames) noexcept {
  if (m_eq_state.empty()) {
    return;
  }
  const auto channels = static_cast<std::size_t>(m_config.num_channels);

  // Bands in fixed order, every block, for every strip. Bands are cascaded, so
  // the order is part of the result -- two peaking bands overlapping do not
  // commute exactly in floating point even though they do in algebra.
  for (std::size_t band = 0; band < rt::kEqBands; ++band) {
    const rt::EqBand& settings = eq.bands[band];
    const std::size_t base = ((pad * rt::kEqBands) + band) * channels;

    if (!settings.enabled) {
      // BYPASS IS BIT-EXACT: the samples are not touched at all. Not multiplied
      // by a coefficient set that happens to be passthrough, not run through a
      // section with b0 = 1 -- untouched, because that is the only version of
      // "bypassed" a hash can confirm.
      //
      // The state is RESET rather than frozen. Frozen state would make the first
      // block after re-enabling depend on how long ago the band was switched
      // off, which is both a click and a determinism bug: the same session
      // played twice would differ if the toggles landed on different blocks.
      for (std::size_t channel = 0; channel < channels; ++channel) {
        m_eq_state[base + channel].reset();
      }
      continue;
    }

    for (std::size_t channel = 0; channel < channels && channel < buffer.size(); ++channel) {
      m_eq_state[base + channel].process_block(settings.coeffs,
                                               std::span<float>{buffer[channel], num_frames});
    }
  }
}

void Engine::apply_compressor(std::span<float* const> buffer, const rt::CompressorConfig& config,
                              std::size_t pad, std::size_t num_frames) noexcept {
  rt::Compressor& compressor = m_strip_compressor[pad];

  if (!config.enabled) {
    // Bit-exact bypass, and the envelope reset rather than frozen -- the same
    // rule as a bypassed EQ band, for the same reason: a frozen envelope would
    // make the first block after re-enabling depend on how long ago it was
    // switched off.
    compressor.reset();
    m_strip_reduction[pad] = 1.0F;
    return;
  }

  float lowest = 1.0F;
  for (std::size_t frame = 0; frame < num_frames; ++frame) {
    // ONE DETECTOR ACROSS THE CHANNELS, and one gain applied to all of them.
    // Per-channel compression would move a hard-panned snare toward the middle
    // every time it hit, which is the stereo-image problem docs/MIXER.md names.
    float detector = 0.0F;
    for (const float* samples : buffer) {
      const float value = samples[frame];
      detector = std::max(detector, value < 0.0F ? -value : value);
    }

    const float gain = compressor.process(config, detector);
    for (float* samples : buffer) {
      samples[frame] *= gain;
    }

    // Reported without the makeup, so the meter shows REDUCTION rather than the
    // net of two independent decisions -- a compressor pulling 6 dB down with
    // 6 dB of makeup is doing something, and a meter reading unity would say it
    // was not.
    lowest = std::min(lowest, config.makeup_gain > 0.0F ? gain / config.makeup_gain : gain);
  }
  m_strip_reduction[pad] = lowest;
}

rt::StripConfig Engine::strip_config(std::size_t pad) const noexcept {
  const std::shared_ptr<const rt::PadConfig>& config = m_pads[pad];
  return config == nullptr ? rt::StripConfig{} : config->strip;
}

bool Engine::any_soloed() const noexcept {
  for (const std::shared_ptr<const rt::PadConfig>& config : m_pads) {
    if (config != nullptr && config->strip.solo) {
      return true;
    }
  }
  return false;
}

void Engine::mix_graph(std::span<float* const> channels, std::size_t num_frames) noexcept {
  if (m_graph_channels.empty()) {
    return;
  }

  // Derived once for the whole block from all sixteen strips, not stored.
  // Solo is a property of the SET -- "is anything soloed" -- so a stored count
  // would be one more thing to keep in step every time a pad is reconfigured,
  // reloaded or cleared, and a leaked count silences the machine.
  const bool soloed = any_soloed();

  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    const rt::StripConfig strip = strip_config(pad);
    const std::span<float* const> buffer = node_channels(pad);

    // THE FIXED CHAIN: gain -> EQ -> compressor -> balance (docs/MIXER.md).
    // The EQ and the compressor arrive in the next two tasks and insert BETWEEN
    // these two calls. Two passes rather than one folded scalar precisely so
    // that there is a gap to insert into: the order is a specification, and
    // folding it now would mean unfolding it later and getting it wrong quietly.
    //
    // Run for every strip including inaudible ones. A muted strip whose EQ
    // stopped advancing would jump when unmuted, and skipping a silent strip
    // would truncate a filter's tail -- a determinism bug dressed as a saving.
    // Only the mix below is skipped, because that is the part that means
    // "audible".
    apply_gain(buffer, rt::clamp_strip_gain(strip.gain), num_frames);
    apply_eq(buffer, strip.eq, pad, num_frames);
    apply_compressor(buffer, strip.compressor, pad, num_frames);
    apply_balance(buffer, rt::clamp_strip_balance(strip.balance), num_frames);

    // POST-FADER, and zero when the strip is not reaching the mix. This meter
    // sits next to the fader and answers "what is this strip contributing",
    // which for a muted strip is nothing. Telemetry::pad_peak is the other
    // question -- "did this pad play" -- and still reports the hit.
    const bool audible = rt::strip_audible(strip, soloed);
    m_strip_peak[pad] = audible ? peak_of(buffer, num_frames) : 0.0F;
    if (!audible) {
      continue;
    }
    mix_into(node_channels(rt::kNumPads + rt::clamp_strip_bus(strip.bus)), buffer, num_frames);
  }

  // Buses into master, in bus order. Every bus, including any that nothing is
  // routed to: adding a silent bus adds exactly 0.0f, which changes no sample
  // and no hash, and a fixed walk is one fewer conditional in the callback and
  // one fewer way for the graph to differ between a live run and a bounce.
  for (std::size_t bus = 0; bus < rt::kNumBuses; ++bus) {
    const std::span<float* const> buffer = node_channels(rt::kNumPads + bus);

    // The bus fader, applied to the bus itself rather than folded into the sum,
    // so the meter below reads what the bus is actually sending -- the same
    // post-fader rule the strips follow.
    apply_gain(buffer, rt::clamp_bus_gain(m_master.bus_gain[bus]), num_frames);
    m_bus_peak[bus] = peak_of(buffer, num_frames);
    mix_into(channels, buffer, num_frames);
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

bool Engine::set_strip(std::uint8_t pad, const rt::StripConfig& strip) noexcept {
  if (pad >= rt::kNumPads) {
    return false;
  }
  // Read-modify-publish over the WHOLE PadConfig, which is the one-pointer rule
  // doing its job rather than getting in the way: there is no path that mutates
  // a published config, so a fader move is a new object like every other edit.
  // The sample and the slice come along by copy, which is what keeps a pad
  // playing the same material after its fader moves.
  const std::shared_ptr<const rt::PadConfig> current = pad_config(pad);
  rt::PadConfig next = current == nullptr ? rt::PadConfig{.pad = pad} : *current;
  next.strip = strip;
  return publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(next)));
}

rt::StripConfig Engine::strip(std::uint8_t pad) const noexcept {
  const std::shared_ptr<const rt::PadConfig> config = pad_config(pad);
  return config == nullptr ? rt::StripConfig{} : config->strip;
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

bool Engine::set_limiter(const rt::LimiterConfig& limiter) noexcept {
  rt::MasterConfig next = m_published_master;
  next.limiter = limiter;
  if (!m_master_commands.try_push(next)) {
    return false;
  }
  m_published_master = next;
  return true;
}

bool Engine::set_bus_gain(std::uint8_t bus, float gain) noexcept {
  if (bus >= rt::kNumBuses) {
    return false;
  }
  rt::MasterConfig next = m_published_master;
  next.bus_gain[bus] = gain;
  if (!m_master_commands.try_push(next)) {
    return false;
  }
  m_published_master = next;
  return true;
}

rt::LimiterConfig Engine::limiter() const noexcept {
  return m_published_master.limiter;
}

float Engine::bus_gain(std::uint8_t bus) const noexcept {
  return bus < rt::kNumBuses ? m_published_master.bus_gain[bus] : 1.0F;
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
    // BEFORE the pad range check, because this one does not name a pad. Reading
    // its `pad` at all would make a panic depend on a field nobody sets.
    if (event.kind == rt::PadEventKind::kStopAll) {
      m_voices.stop_all();
      continue;
    }

    if (event.pad >= rt::kNumPads) {
      continue;  // came from a key handler or MIDI; not trusted
    }

    if (event.kind == rt::PadEventKind::kStop) {
      // Not a note-off: a one-shot ignores those, and a one-shot too long to
      // wait out is exactly what this is for.
      m_voices.stop_pad(event.pad);
      continue;
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

void Engine::adopt_offline() noexcept {
  adopt_pad_configs();
  adopt_sequencer();
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

  // The graph gets the same treatment, and for the same reason: everything
  // downstream of a voice ACCUMULATES, so every node has to start at silence.
  clear_graph(num_frames);

  // Reconfigurations before triggers, so a pad assigned and played in the same
  // UI frame sounds the new material rather than one block of the old.
  adopt_pad_configs();
  adopt_sequencer();

  // The master limiter's settings, drained to the LAST one published. A value
  // ring, so there is nothing to retire and no ordering to preserve: only the
  // newest setting matters, and any earlier ones in the same block were
  // superseded before a sample was rendered with them.
  rt::MasterConfig master_update;
  while (m_master_commands.try_pop(master_update)) {
    m_master = master_update;
  }

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

  // THE GRAPH. Each pad's voices into its own strip, then strips into buses and
  // buses into the master -- which is `channels` itself, already cleared above.
  //
  // Pad order, ascending, every block. That is not incidental: it is the
  // summation order, and float addition is not associative, so the order IS part
  // of the output. Making it a plain ascending walk is what keeps an offline
  // bounce bit-identical to a live render.
  for (std::uint8_t pad = 0; pad < rt::kNumPads; ++pad) {
    m_voices.render_pad(pad, node_channels(pad), num_frames);
  }
  mix_graph(channels, num_frames);

  // After the voices and before the position advances: the click is placed by
  // the same block_start the steps were, so it lands on the beat it marks.
  //
  // AT MASTER, after the bus sum -- docs/MIXER.md. It is a monitoring signal
  // that belongs to the machine rather than to the music: giving it a strip
  // would give it a fader, a mute and an EQ it has no business having.
  mix_metronome(channels, num_frames);

  // LAST AT MASTER, after the metronome, because it is the one thing that must
  // be able to catch everything -- including the click. Putting the metronome
  // after it would make the click the only signal able to clip the output
  // (docs/MIXER.md).
  //
  // Bypassed entirely when disabled, not run with a unity gain: the lookahead is
  // a DELAY, so a disabled limiter that still ran would shift every sample by
  // 64 frames and move every committed hash in the project.
  if (m_master.limiter.enabled) {
    m_limiter.process(m_master.limiter, channels, num_frames);
  } else {
    // Reset rather than left holding old audio, so enabling it starts from
    // silence in the delay line instead of replaying 64 frames from whenever it
    // was last on. Same rule as a bypassed EQ band.
    m_limiter.reset();
  }

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

  // The mixer's meters, already measured in mix_graph() where audibility was
  // known. Same fall as every other meter in the program, from the same
  // constant, so nothing on screen decays at a rate of its own.
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    const float previous = m_published.strip_peak[pad].load(std::memory_order_relaxed);
    m_published.strip_peak[pad].store(std::max(m_strip_peak[pad], previous - fall),
                                      std::memory_order_relaxed);
  }
  for (std::size_t bus = 0; bus < rt::kNumBuses; ++bus) {
    const float previous = m_published.bus_peak[bus].load(std::memory_order_relaxed);
    m_published.bus_peak[bus].store(std::max(m_bus_peak[bus], previous - fall),
                                    std::memory_order_relaxed);
  }

  // Gain reduction is published as measured, with no fall applied: it is not a
  // peak that needs holding, it is the gain the compressor is applying right
  // now, and a decaying gain-reduction meter would show reduction that has
  // already stopped.
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    m_published.strip_reduction[pad].store(m_strip_reduction[pad], std::memory_order_relaxed);
  }
  m_published.limiter_gain.store(m_master.limiter.enabled ? m_limiter.gain() : 1.0F,
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
  snapshot.limiter_gain = m_published.limiter_gain.load(std::memory_order_relaxed);
  for (std::size_t bus = 0; bus < rt::kNumBuses; ++bus) {
    snapshot.bus_peak[bus] = m_published.bus_peak[bus].load(std::memory_order_relaxed);
  }

  const auto rate = static_cast<float>(m_config.sample_rate == 0 ? 1U : m_config.sample_rate);
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    snapshot.pad_peak[pad] = m_published.pad_peak[pad].load(std::memory_order_relaxed);
    snapshot.strip_peak[pad] = m_published.strip_peak[pad].load(std::memory_order_relaxed);
    snapshot.strip_reduction[pad] =
        m_published.strip_reduction[pad].load(std::memory_order_relaxed);

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
