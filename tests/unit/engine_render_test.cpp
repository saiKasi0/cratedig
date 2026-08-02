#include "engine/engine.hpp"
#include "rt/click.hpp"
#include "rt/garbage_ring.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "rt/voice_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kMaxBlock = 2'048;

engine::Engine::Config test_config() {
  return engine::Engine::Config{
      .sample_rate = 48'000, .num_channels = kChannels, .max_block_frames = kMaxBlock, .seed = 0};
}

// Holds the output of a whole render pass as one contiguous block so it can be
// hashed and compared byte for byte.
//
// Layout is CHANNEL-MAJOR, matching rt::Sample and the engine's own buffers:
// channel c occupies [c * total_frames, (c + 1) * total_frames). Nothing here
// interleaves, because nothing downstream of the device layer ever does.
class RenderCapture {
 public:
  RenderCapture(std::size_t total_frames, std::uint16_t channels)
      : m_channels(channels), m_total_frames(total_frames), m_data(total_frames * channels, 0.0F) {}

  // Renders total_frames through the engine in blocks of the given sizes,
  // cycling through them, and captures everything.
  void render_in_blocks(engine::Engine& eng, std::span<const std::size_t> block_sizes) {
    std::vector<float> scratch(static_cast<std::size_t>(kMaxBlock) * m_channels, 0.0F);
    std::vector<float*> channel_pointers(m_channels);

    std::size_t done = 0;
    std::size_t next_size = 0;
    while (done < m_total_frames) {
      const std::size_t block =
          std::min(block_sizes[next_size % block_sizes.size()], m_total_frames - done);
      next_size++;
      if (block == 0) {
        continue;
      }

      // Dirty the scratch buffer before every call: if render() ever fails to
      // write a sample, the test must see the garbage rather than a leftover
      // zero that makes the bug invisible.
      std::fill(scratch.begin(), scratch.end(), -7.5F);
      for (std::uint16_t channel = 0; channel < m_channels; ++channel) {
        channel_pointers[channel] =
            scratch.data() + (static_cast<std::size_t>(channel) * kMaxBlock);
      }

      eng.render(std::span<float* const>{channel_pointers}, block);

      for (std::uint16_t channel = 0; channel < m_channels; ++channel) {
        float* source = channel_pointers[channel];
        float* destination =
            m_data.data() + (static_cast<std::size_t>(channel) * m_total_frames) + done;
        std::memcpy(destination, source, block * sizeof(float));
      }
      done += block;
    }
  }

  [[nodiscard]] std::span<const float> samples() const { return m_data; }

  // FNV-1a over the raw bytes. This is the golden-hash harness the DSP work in
  // M1+ inherits: a changed hash is a changed behaviour, to be explained rather
  // than re-baselined (docs/TESTING.md).
  [[nodiscard]] std::uint64_t hash() const {
    constexpr std::uint64_t kOffsetBasis = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(m_data.data());
    const std::size_t byte_count = m_data.size() * sizeof(float);

    std::uint64_t hash = kOffsetBasis;
    for (std::size_t i = 0; i < byte_count; ++i) {
      hash ^= bytes[i];
      hash *= kPrime;
    }
    return hash;
  }

 private:
  std::uint16_t m_channels;
  std::size_t m_total_frames;
  std::vector<float> m_data;
};

}  // namespace

TEST_CASE("Engine renders exact silence", "[unit]") {
  engine::Engine eng{test_config()};

  constexpr std::size_t kFrames = 4'096;
  RenderCapture capture{kFrames, kChannels};
  const std::array<std::size_t, 1> blocks{128};
  capture.render_in_blocks(eng, blocks);

  // Bit-exact, not approximate: a denormal or a -0.0f leaking through would be
  // inaudible now and a real bug later.
  const std::vector<float> expected_zeros(capture.samples().size(), 0.0F);
  CHECK(std::memcmp(capture.samples().data(), expected_zeros.data(),
                    expected_zeros.size() * sizeof(float)) == 0);
}

TEST_CASE("Engine output is invariant to block size", "[unit]") {
  // The property that actually matters: state must not leak across block
  // boundaries. An engine that behaves differently at 128 frames than at 1024 is
  // not deterministic no matter how repeatable each individual run is.
  constexpr std::size_t kFrames = 48'000;

  RenderCapture single{kFrames, kChannels};
  engine::Engine engine_single{test_config()};
  const std::array<std::size_t, 1> one_block{kMaxBlock};
  single.render_in_blocks(engine_single, one_block);

  RenderCapture chunked{kFrames, kChannels};
  engine::Engine engine_chunked{test_config()};
  const std::array<std::size_t, 1> small_blocks{128};
  chunked.render_in_blocks(engine_chunked, small_blocks);

  RenderCapture ragged{kFrames, kChannels};
  engine::Engine engine_ragged{test_config()};
  // A fixed seed, so "random" block sizes are the same on every run and on every
  // machine — a varying test input cannot prove determinism.
  std::mt19937 rng{12'345};
  std::uniform_int_distribution<std::size_t> sizes{1, kMaxBlock};
  std::array<std::size_t, 64> ragged_blocks{};
  for (std::size_t& size : ragged_blocks) {
    size = sizes(rng);
  }
  ragged.render_in_blocks(engine_ragged, ragged_blocks);

  CHECK(single.hash() == chunked.hash());
  CHECK(single.hash() == ragged.hash());
}

TEST_CASE("Engine renders the same bytes on every run", "[unit]") {
  constexpr std::size_t kFrames = 8'192;
  const std::array<std::size_t, 3> blocks{64, 512, 333};

  RenderCapture first{kFrames, kChannels};
  engine::Engine engine_first{test_config()};
  first.render_in_blocks(engine_first, blocks);

  RenderCapture second{kFrames, kChannels};
  engine::Engine engine_second{test_config()};
  second.render_in_blocks(engine_second, blocks);

  CHECK(first.hash() == second.hash());
  CHECK(std::memcmp(first.samples().data(), second.samples().data(),
                    first.samples().size() * sizeof(float)) == 0);

  // The committed golden value. M0 renders silence, so this is the FNV-1a hash
  // of 8192 frames x 2 channels of zero bytes. When the engine starts producing
  // audio this assertion changes — and that change must be justified in the
  // commit, never quietly re-baselined.
  CHECK(first.hash() == 0xEB05052EA5B62325ULL);
}

TEST_CASE("Engine counts rendered frames", "[unit]") {
  engine::Engine eng{test_config()};
  CHECK(eng.frames_rendered() == 0);

  RenderCapture capture{1'000, kChannels};
  const std::array<std::size_t, 2> blocks{100, 150};
  capture.render_in_blocks(eng, blocks);

  CHECK(eng.frames_rendered() == 1'000);
}

TEST_CASE("Engine keeps its configuration", "[unit]") {
  const engine::Engine eng{test_config()};

  CHECK(eng.config().sample_rate == 48'000);
  CHECK(eng.config().num_channels == kChannels);
  CHECK(eng.config().max_block_frames == kMaxBlock);
  CHECK(eng.config().seed == 0);
}

namespace {

// A synthetic sample, built in the test rather than fetched.
//
// Determinism is the property under test, so it must not depend on a fixture
// being present, on a decoder being correct, or on the network. The fixture
// -backed tests live in tests/unit/decode_test.cpp and are allowed to skip;
// these are not.
//
// The default rate is 44.1 kHz against a 48 kHz engine, giving a phase step of
// 0.91875 -- a genuinely fractional accumulator. At a 1:1 rate the fraction is
// always zero and block-size invariance would hold even for a broken
// implementation.
std::shared_ptr<const rt::Sample> make_test_sample(std::uint32_t rate = 44'100,
                                                   std::uint16_t channels = 2,
                                                   std::size_t frames = 3'000) {
  auto sample = std::make_shared<rt::Sample>(rate, channels, frames);
  for (std::uint16_t channel = 0; channel < channels; ++channel) {
    std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      // Deterministic and non-trivial: an integer recurrence, so there is no
      // libm call whose last bit could differ between platforms.
      const auto phase = static_cast<float>((frame * (7 + channel)) % 211);
      data[frame] = ((phase / 105.0F) - 1.0F) * 0.5F;
    }
  }
  return sample;
}

// Loads in place rather than returning an Engine: the engine owns an SpscRing
// and a GarbageRing, both of which delete their move constructors on purpose, so
// Engine is neither copyable nor movable.
void load_pads(engine::Engine& eng) {
  REQUIRE(eng.set_pad_sample(0, make_test_sample()));
  REQUIRE(eng.set_pad_sample(1, make_test_sample(48'000, 1, 1'500)));
}

void trigger(engine::Engine& eng, std::uint8_t pad, float velocity) {
  const rt::PadEvent event{.pad = pad, .velocity = velocity, .frame_offset = 0};
  REQUIRE(eng.trigger_pad(event));
}

}  // namespace

TEST_CASE("Engine plays a triggered pad", "[unit]") {
  engine::Engine eng{test_config()};
  load_pads(eng);
  trigger(eng, 0, 1.0F);

  RenderCapture capture{2'048, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);

  const std::span<const float> samples = capture.samples();
  CHECK(std::any_of(samples.begin(), samples.end(), [](float v) { return v != 0.0F; }));
  CHECK(eng.active_voices() == 1);
}

TEST_CASE("Engine leaves unloaded and out-of-range pads silent", "[unit]") {
  engine::Engine eng{test_config()};
  load_pads(eng);

  trigger(eng, 7, 1.0F);    // in range, no sample loaded
  trigger(eng, 200, 1.0F);  // out of range entirely

  RenderCapture capture{512, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);

  const std::vector<float> expected_zeros(capture.samples().size(), 0.0F);
  CHECK(std::memcmp(capture.samples().data(), expected_zeros.data(),
                    expected_zeros.size() * sizeof(float)) == 0);
  CHECK(eng.active_voices() == 0);
}

TEST_CASE("Engine output with voices is invariant to block size", "[unit]") {
  // The M0 version of this test rendered silence, which is block-invariant no
  // matter how badly the phase is handled. This is the version that can fail:
  // a voice at a fractional rate ratio, rendered three different ways.
  constexpr std::size_t kFrames = 16'384;

  auto render = [](std::span<const std::size_t> blocks) {
    engine::Engine eng{test_config()};
    load_pads(eng);
    trigger(eng, 0, 0.8F);
    trigger(eng, 1, 0.6F);
    RenderCapture capture{kFrames, kChannels};
    capture.render_in_blocks(eng, blocks);
    return capture;
  };

  const std::array<std::size_t, 1> one_block{kMaxBlock};
  const std::array<std::size_t, 1> small_blocks{64};
  std::mt19937 rng{777};
  std::uniform_int_distribution<std::size_t> sizes{1, kMaxBlock};
  std::array<std::size_t, 64> ragged_blocks{};
  for (std::size_t& size : ragged_blocks) {
    size = sizes(rng);
  }

  const RenderCapture whole = render(one_block);
  const RenderCapture chunked = render(small_blocks);
  const RenderCapture ragged = render(ragged_blocks);

  CHECK(whole.hash() == chunked.hash());
  CHECK(whole.hash() == ragged.hash());

  // Guards against all three being silence, which would satisfy the equalities
  // above while proving nothing at all.
  const std::span<const float> samples = whole.samples();
  CHECK(std::any_of(samples.begin(), samples.end(), [](float v) { return v != 0.0F; }));
}

TEST_CASE("Engine renders the same audio on every run", "[unit]") {
  constexpr std::size_t kFrames = 8'192;
  const std::array<std::size_t, 3> blocks{64, 512, 333};

  auto render = [&blocks]() {
    engine::Engine eng{test_config()};
    load_pads(eng);
    trigger(eng, 0, 0.8F);
    trigger(eng, 1, 0.6F);
    RenderCapture capture{kFrames, kChannels};
    capture.render_in_blocks(eng, blocks);
    return capture;
  };

  const RenderCapture first = render();
  const RenderCapture second = render();

  CHECK(first.hash() == second.hash());
  CHECK(std::memcmp(first.samples().data(), second.samples().data(),
                    first.samples().size() * sizeof(float)) == 0);
}

TEST_CASE("Engine reports a full event ring rather than blocking", "[unit]") {
  engine::Engine eng{test_config()};
  load_pads(eng);

  std::size_t accepted = 0;
  for (std::size_t i = 0; i < engine::Engine::kEventRingCapacity + 10; ++i) {
    if (eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F, .frame_offset = 0})) {
      ++accepted;
    }
  }

  CHECK(accepted == engine::Engine::kEventRingCapacity);
  CHECK(eng.dropped_events() == 10);
}

TEST_CASE("Engine plays only the slice a pad config names", "[unit]") {
  // The whole point of M3, through the real path: a config published from the
  // control thread, adopted by render(), and heard as a sub-range.
  engine::Engine eng{test_config()};

  // A ramp, so every source frame is identifiable by its value.
  constexpr std::size_t kFrames = 1'000;
  auto sample = std::make_shared<rt::Sample>(48'000U, static_cast<std::uint16_t>(1), kFrames);
  std::span<float> data = sample->mutable_channel(0);
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    data[frame] = static_cast<float>(frame) / 1'000.0F;
  }

  REQUIRE(eng.publish_pad_config(
      std::make_shared<const rt::PadConfig>(rt::PadConfig{.sample = std::move(sample),
                                                          .pad = 0,
                                                          .start_frame = 400,
                                                          .end_frame = 420,
                                                          .fade_in_frames = 0,
                                                          .fade_out_frames = 0})));
  trigger(eng, 0, 1.0F);

  constexpr std::size_t kCaptured = 256;
  RenderCapture capture{kCaptured, kChannels};
  const std::array<std::size_t, 1> blocks{kCaptured};
  capture.render_in_blocks(eng, blocks);

  // Channel-major: channel 1 starts at kCaptured. A mono source feeds both.
  const std::span<const float> samples = capture.samples();
  CHECK(samples[0] == 0.4F);          // output frame 0 is source frame 400
  CHECK(samples[19] == 0.419F);       // ...and frame 19 is source 419, the last
  CHECK(samples[20] == 0.0F);         // half-open: source 420 is not played
  CHECK(samples[kCaptured] == 0.4F);  // channel 1 matches
  CHECK(eng.active_voices() == 0);
}

TEST_CASE("Engine honours note-off for a gate pad only", "[unit]") {
  // The note-off path exists from M3 with one producer (the Kitty keyboard
  // protocol). M4's MIDI and M5's sequencer become further producers of an event
  // the engine already understands, rather than each inventing their own idea of
  // what letting go means -- which is why it is tested here and not there.
  auto make_pad = [](std::uint8_t pad, rt::TriggerMode mode) {
    auto sample = std::make_shared<rt::Sample>(48'000U, static_cast<std::uint16_t>(1),
                                               static_cast<std::size_t>(48'000));
    std::span<float> data = sample->mutable_channel(0);
    for (float& value : data) {
      value = 0.5F;
    }
    return std::make_shared<const rt::PadConfig>(rt::PadConfig{.sample = std::move(sample),
                                                               .pad = pad,
                                                               .fade_in_frames = 0,
                                                               .fade_out_frames = 0,
                                                               .trigger = mode});
  };

  engine::Engine eng{test_config()};
  REQUIRE(eng.publish_pad_config(make_pad(0, rt::TriggerMode::kGate)));
  REQUIRE(eng.publish_pad_config(make_pad(1, rt::TriggerMode::kOneShot)));
  trigger(eng, 0, 1.0F);
  trigger(eng, 1, 1.0F);

  RenderCapture capture{4'096, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);
  REQUIRE(eng.active_voices() == 2);

  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .kind = rt::PadEventKind::kNoteOff}));
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 1, .kind = rt::PadEventKind::kNoteOff}));
  capture.render_in_blocks(eng, blocks);

  // The gate pad let go; the one-shot kept playing its full second of audio.
  CHECK(eng.active_voices() == 1);
}

TEST_CASE("Engine chokes a group through the event path", "[unit]") {
  auto make_hat = [](std::uint8_t pad) {
    auto sample = std::make_shared<rt::Sample>(48'000U, static_cast<std::uint16_t>(1),
                                               static_cast<std::size_t>(48'000));
    std::span<float> data = sample->mutable_channel(0);
    for (float& value : data) {
      value = 0.5F;
    }
    return std::make_shared<const rt::PadConfig>(rt::PadConfig{.sample = std::move(sample),
                                                               .pad = pad,
                                                               .fade_in_frames = 0,
                                                               .fade_out_frames = 0,
                                                               .choke_group = 1});
  };

  engine::Engine eng{test_config()};
  REQUIRE(eng.publish_pad_config(make_hat(0)));
  REQUIRE(eng.publish_pad_config(make_hat(1)));

  trigger(eng, 0, 1.0F);
  RenderCapture capture{4'096, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);
  REQUIRE(eng.active_voices() == 1);

  trigger(eng, 1, 1.0F);
  capture.render_in_blocks(eng, blocks);

  // The open hat was cut by the closed one, rather than both ringing on for a
  // second.
  CHECK(eng.active_voices() == 1);
}

TEST_CASE("Engine retires finished samples to the janitor", "[unit]") {
  // The audio thread must never run a Sample destructor. Here the engine is the
  // only other owner, so watching use_count() shows exactly when the reference
  // is released -- and that it happens in collect_garbage(), not in render().
  auto sample = make_test_sample(48'000, 1, 64);
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(0, sample));
  CHECK(sample.use_count() == 2);  // ours plus the pad table

  trigger(eng, 0, 1.0F);

  RenderCapture capture{512, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);

  CHECK(eng.active_voices() == 0);
  CHECK(eng.collect_garbage() == 1);
  CHECK(sample.use_count() == 2);  // the voice's reference is gone; the pad still holds one
  CHECK(eng.garbage_overflows() == 0);
}

TEST_CASE("Engine places a trigger inside the block", "[unit]") {
  // The engine half of sample-accurate triggering: PadEvent::frame_offset has
  // been carried since M1 and ignored until M4.
  //
  // Compared against the SAME trigger at offset zero rather than against a
  // hard-coded frame. set_pad_sample() builds a config with the default 32-frame
  // declick fade, so a voice's first frames are legitimately zero and "the first
  // non-zero sample" is not where the voice started -- an assertion phrased that
  // way tests the fade, not the offset.
  constexpr std::uint32_t kOffset = 41;
  constexpr std::size_t kFrames = 1'024;

  const auto render_at = [](std::uint32_t offset) {
    engine::Engine eng{test_config()};
    load_pads(eng);
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 1, .velocity = 1.0F, .frame_offset = offset}));
    RenderCapture capture{kFrames, kChannels};
    const std::array<std::size_t, 1> blocks{kFrames};  // one block, so the offset is inside it
    capture.render_in_blocks(eng, blocks);
    std::vector<float> out(capture.samples().begin(), capture.samples().end());
    return out;
  };

  const std::vector<float> aligned = render_at(0);
  const std::vector<float> offset = render_at(kOffset);

  for (std::size_t frame = 0; frame < kOffset; ++frame) {
    INFO("frame " << frame << " precedes the offset and must be untouched");
    REQUIRE(offset[frame] == 0.0F);
  }
  // Shifted, not merely delayed: every frame after the offset is bit-identical
  // to the un-offset render, which is what shows the envelope and the fade
  // advanced with the audio rather than during the skipped frames.
  for (std::size_t frame = 0; frame + kOffset < kFrames; ++frame) {
    INFO("aligned frame " << frame << " vs offset frame " << (frame + kOffset));
    REQUIRE(aligned[frame] == offset[frame + kOffset]);
  }
  // And it is not silence being compared with silence.
  CHECK(std::any_of(aligned.begin(), aligned.end(), [](float v) { return v != 0.0F; }));
}

TEST_CASE("Engine clamps a frame offset past the end of the block", "[unit]") {
  // frame_offset crossed a thread boundary exactly as event.pad did, so a
  // producer that disagrees with us about the block length must not be able to
  // push a voice past the end of it.
  //
  // What is asserted here is that the hit SURVIVES -- clamped rather than
  // dropped, because a note slightly late beats a note silently lost. Where
  // exactly it lands is pinned at the pool level, in voice_pool_test.cpp, where
  // there is no fade in the way.
  engine::Engine eng{test_config()};
  load_pads(eng);
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 1, .velocity = 1.0F, .frame_offset = 100'000}));

  RenderCapture capture{2'048, kChannels};
  const std::array<std::size_t, 1> blocks{512};
  capture.render_in_blocks(eng, blocks);

  const std::span<const float> samples = capture.samples();
  CHECK(std::any_of(samples.begin(), samples.end(), [](float v) { return v != 0.0F; }));

  // Not at the top of the block either: a clamp that collapsed to zero would
  // turn every bad offset into an on-the-beat hit, which is the failure mode
  // that would never be noticed.
  CHECK(samples[0] == 0.0F);
  CHECK(samples[1] == 0.0F);
}

TEST_CASE("Engine output with offset triggers is invariant to block size", "[unit]") {
  // The property that matters most for M4: the sequencer will place hits at
  // exact frames, and if a hit lands on a different frame under a different
  // block size then the offline bounce cannot reproduce the live render -- which
  // is the whole acceptance.
  //
  // Offsets are chosen to straddle a 64-frame boundary, so under small blocks
  // they are NOT all inside the first block and the clamp has to behave.
  constexpr std::size_t kFrames = 8'192;

  const auto render_with = [](std::span<const std::size_t> blocks) {
    engine::Engine eng{test_config()};
    load_pads(eng);
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 1, .velocity = 1.0F, .frame_offset = 0}));
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 0.5F, .frame_offset = 37}));
    RenderCapture capture{kFrames, kChannels};
    capture.render_in_blocks(eng, blocks);
    return capture.hash();
  };

  const std::array<std::size_t, 1> one_block{kMaxBlock};
  const std::array<std::size_t, 1> small{64};
  const std::array<std::size_t, 3> mixed{128, 512, 333};

  // Every event is queued before the first render() call, so all of them are
  // drained in block one whatever its size -- which is what makes this a fair
  // comparison rather than a test of when the events happened to be pushed.
  const std::uint64_t reference = render_with(one_block);
  CHECK(render_with(small) == reference);
  CHECK(render_with(mixed) == reference);
}

// --- transport and sequencer publication ------------------------------------

namespace {

// Renders `frames` and throws the audio away. Several transport cases care only
// about how far the position moved.
void spin(engine::Engine& eng, std::size_t frames, std::size_t block = 256) {
  RenderCapture capture{frames, kChannels};
  const std::array<std::size_t, 1> blocks{block};
  capture.render_in_blocks(eng, blocks);
}

}  // namespace

TEST_CASE("Engine advances the transport only while playing", "[unit]") {
  engine::Engine eng{test_config()};

  // Stopped is the starting state, and rendering must not move the position --
  // otherwise every session would begin mid-pattern.
  spin(eng, 4'096);
  CHECK(eng.telemetry().transport_frames == 0);
  CHECK_FALSE(eng.telemetry().transport_playing);

  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
  spin(eng, 4'096);
  CHECK(eng.telemetry().transport_playing);
  CHECK(eng.telemetry().transport_frames == 4'096);

  // Exactly the frames rendered, not approximately: the position IS the clock,
  // and every step boundary is derived from it.
  spin(eng, 1'000);
  CHECK(eng.telemetry().transport_frames == 5'096);

  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kStop}));
  spin(eng, 4'096);
  CHECK_FALSE(eng.telemetry().transport_playing);
  CHECK(eng.telemetry().transport_frames == 5'096);  // frozen where it stopped
}

TEST_CASE("Engine transport position is invariant to block size", "[unit]") {
  // The position must be a property of how many frames were rendered, not of how
  // they were divided up -- the same rule the audio itself obeys, and the reason
  // the sequencer can be reproduced offline.
  const auto position_after = [](std::size_t block) {
    engine::Engine eng{test_config()};
    REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
    spin(eng, 12'288, block);
    return eng.telemetry().transport_frames;
  };

  CHECK(position_after(2'048) == 12'288);
  CHECK(position_after(64) == 12'288);
  CHECK(position_after(333) == 12'288);
}

TEST_CASE("Engine seeks without starting or stopping", "[unit]") {
  // Seek is position only. "Play from the top" is a seek then a play, which is
  // why they are separate commands rather than one with a flag whose meaning
  // nobody remembers.
  engine::Engine eng{test_config()};

  REQUIRE(eng.send_transport(
      rt::TransportCommand{.kind = rt::TransportCommandKind::kSeek, .position_frames = 96'000}));
  spin(eng, 512);
  CHECK(eng.telemetry().transport_frames == 96'000);  // seeking did not start it
  CHECK_FALSE(eng.telemetry().transport_playing);

  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
  spin(eng, 512);
  CHECK(eng.telemetry().transport_frames == 96'512);

  // And seeking while playing keeps it playing.
  REQUIRE(eng.send_transport(
      rt::TransportCommand{.kind = rt::TransportCommandKind::kSeek, .position_frames = 0}));
  spin(eng, 256);
  CHECK(eng.telemetry().transport_playing);
  CHECK(eng.telemetry().transport_frames == 256);
}

TEST_CASE("Engine adopts a published sequencer state", "[unit]") {
  engine::Engine eng{test_config()};

  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = 12'000;  // 120 bpm, 6000 frames per step
  state->selected_pattern = 3;
  state->patterns[3].length = 8;
  REQUIRE(eng.publish_sequencer(state));

  // The control-side view is available immediately, deliberately one block ahead
  // of what the audio thread has -- the same distinction pad_config() draws.
  REQUIRE(eng.sequencer_state() != nullptr);
  CHECK(eng.sequencer_state()->selected_pattern == 3);

  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  // Six steps in: 6 * 6000 = 36000 frames. With a length of 8 that wraps to 6.
  spin(eng, 36'000 + 10);
  const engine::Telemetry telemetry = eng.telemetry();
  CHECK(telemetry.transport_pattern == 3);
  CHECK(telemetry.transport_step == 6);

  // Ten steps in wraps: 10 % 8 == 2.
  spin(eng, 24'000);
  CHECK(eng.telemetry().transport_step == 2);
  CHECK(eng.rejected_sequencer_states() == 0);
}

TEST_CASE("Engine reports a refused sequencer publish", "[unit]") {
  // Nothing renders here, so nothing drains the ring -- the same situation
  // --no-audio puts the program in. The refusal has to be visible, because the
  // control-side view must not claim an edit the audio thread never received.
  engine::Engine eng{test_config()};

  std::size_t accepted = 0;
  for (std::size_t attempt = 0; attempt < engine::Engine::kSequencerHandoffCapacity + 4;
       ++attempt) {
    if (eng.publish_sequencer(std::make_shared<rt::SequencerState>())) {
      ++accepted;
    }
  }
  CHECK(accepted == engine::Engine::kSequencerHandoffCapacity);
  CHECK(eng.rejected_sequencer_states() == 4);

  // And a null state is refused without being counted as an overload: it carries
  // nothing to act on, so it is a caller error rather than back-pressure.
  CHECK_FALSE(eng.publish_sequencer(nullptr));
  CHECK(eng.rejected_sequencer_states() == 4);
}

namespace {

// A sequencer state with `pads` firing on the given steps of pattern 0.
std::shared_ptr<const rt::SequencerState> make_pattern(std::initializer_list<std::size_t> steps,
                                                       std::uint8_t pad = 1,
                                                       std::uint32_t bpm_x100 = 12'000,
                                                       std::uint8_t swing = 0) {
  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = bpm_x100;
  state->selected_pattern = 0;
  state->patterns[0].length = 16;
  state->patterns[0].swing = swing;
  for (const std::size_t step : steps) {
    state->patterns[0].steps[step][pad].on = true;
    state->patterns[0].steps[step][pad].velocity = 127;
  }
  return state;
}

}  // namespace

TEST_CASE("the sequencer fires a step at its exact frame", "[unit]") {
  // 120 bpm gives 6000 frames per step. Pad 1 is the 48 kHz sample, so the phase
  // step is exactly one frame and there is no interpolation to blur the onset.
  engine::Engine eng{test_config()};
  load_pads(eng);
  REQUIRE(eng.publish_sequencer(make_pattern({2})));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  RenderCapture capture{24'000, kChannels};
  const std::array<std::size_t, 1> blocks{2'048};
  capture.render_in_blocks(eng, blocks);

  // Step 2 is at frame 12000. Everything before it is silence -- nothing else is
  // playing, and a step that fired early would show up here.
  const std::span<const float> samples = capture.samples();
  for (std::size_t frame = 0; frame < 12'000; ++frame) {
    INFO("frame " << frame << " precedes step 2");
    REQUIRE(samples[frame] == 0.0F);
  }
  CHECK(std::any_of(samples.begin() + 12'000, samples.end(),
                    [](float value) { return value != 0.0F; }));
}

TEST_CASE("the sequencer is invariant to block size", "[unit]") {
  // THE M4 acceptance, in miniature. The sequencer runs on the audio thread, so
  // if a step landed on a block boundary rather than its own frame, the same
  // pattern would render differently at 64 frames and at 2048 -- and the offline
  // bounce could never reproduce the live render.
  //
  // A tempo whose frames-per-step is NOT an integer, deliberately: at 120 bpm
  // every step lands on a round number and a block-quantising implementation
  // would pass at 64 and 2048 by luck.
  const auto hash_at = [](std::size_t block) {
    engine::Engine eng{test_config()};
    load_pads(eng);
    REQUIRE(eng.publish_sequencer(make_pattern({0, 3, 6, 7, 11, 14}, 1, 13'700, 0)));
    REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
    RenderCapture capture{48'000, kChannels};
    const std::array<std::size_t, 1> blocks{block};
    capture.render_in_blocks(eng, blocks);
    return capture.hash();
  };

  const std::uint64_t reference = hash_at(2'048);
  CHECK(hash_at(64) == reference);
  CHECK(hash_at(128) == reference);
  CHECK(hash_at(333) == reference);
  CHECK(hash_at(1'000) == reference);
}

TEST_CASE("a swung pattern is invariant to block size", "[unit]") {
  // Swing is where block quantisation does the most damage: it moves a step by a
  // fraction of a step, which is exactly the resolution a block boundary throws
  // away. At 2048-frame blocks a quantised implementation would round the swing
  // out of existence entirely.
  const auto hash_at = [](std::size_t block) {
    engine::Engine eng{test_config()};
    load_pads(eng);
    REQUIRE(eng.publish_sequencer(make_pattern({0, 1, 2, 3, 4, 5, 6, 7}, 1, 11'300, 62)));
    REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
    RenderCapture capture{48'000, kChannels};
    const std::array<std::size_t, 1> blocks{block};
    capture.render_in_blocks(eng, blocks);
    return capture.hash();
  };

  const std::uint64_t reference = hash_at(2'048);
  CHECK(hash_at(64) == reference);
  CHECK(hash_at(377) == reference);
}

TEST_CASE("swing actually changes the audio", "[unit]") {
  // The anti-vacuity guard for the case above: three identical hashes would be
  // three identical silences if the swing were being ignored, and the invariance
  // test could not tell the difference.
  const auto hash_with = [](std::uint8_t swing) {
    engine::Engine eng{test_config()};
    load_pads(eng);
    REQUIRE(eng.publish_sequencer(make_pattern({0, 1, 2, 3}, 1, 12'000, swing)));
    REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
    RenderCapture capture{48'000, kChannels};
    const std::array<std::size_t, 1> blocks{256};
    capture.render_in_blocks(eng, blocks);
    return capture.hash();
  };

  CHECK(hash_with(0) != hash_with(50));
}

TEST_CASE("the sequencer is silent while the transport is stopped", "[unit]") {
  // A pattern full of steps must make no sound until play is pressed. Otherwise
  // loading a project would start playing it.
  engine::Engine eng{test_config()};
  load_pads(eng);
  REQUIRE(eng.publish_sequencer(make_pattern({0, 1, 2, 3, 4, 5, 6, 7})));

  RenderCapture capture{48'000, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);

  for (const float value : capture.samples()) {
    REQUIRE(value == 0.0F);
  }
}

TEST_CASE("the sequencer wraps at the pattern length", "[unit]") {
  // A step on 0 of a 4-step pattern fires every 4 steps: at frames 0, 24000,
  // 48000... rather than once. Getting the modulo wrong gives a pattern that
  // plays through once and then goes quiet, which sounds like a stuck transport.
  engine::Engine eng{test_config()};
  load_pads(eng);

  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = 12'000;  // 6000 frames per step
  state->patterns[0].length = 4;
  state->patterns[0].steps[0][1].on = true;
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  RenderCapture capture{72'000, kChannels};
  const std::array<std::size_t, 1> blocks{512};
  capture.render_in_blocks(eng, blocks);

  // Three wraps in 72000 frames (24000 each). Each one starts a voice, so the
  // frame just after each boundary is where the fade-in begins.
  const std::span<const float> samples = capture.samples();
  for (const std::size_t at : {std::size_t{0}, std::size_t{24'000}, std::size_t{48'000}}) {
    INFO("wrap at frame " << at);
    // Non-silent within a few hundred frames of the boundary, past the declick.
    REQUIRE(std::any_of(samples.begin() + static_cast<std::ptrdiff_t>(at) + 64,
                        samples.begin() + static_cast<std::ptrdiff_t>(at) + 512,
                        [](float value) { return value != 0.0F; }));
  }
}

TEST_CASE("a seek moves the sequencer with the transport", "[unit]") {
  // Steps are derived from the position, so seeking to step 4 must play step 4
  // -- not restart the pattern and not carry on from where it was.
  engine::Engine eng{test_config()};
  load_pads(eng);
  REQUIRE(eng.publish_sequencer(make_pattern({5})));  // frame 30000 at 120 bpm

  // Start just before step 5 and render a short window that contains only it.
  REQUIRE(eng.send_transport(
      rt::TransportCommand{.kind = rt::TransportCommandKind::kSeek, .position_frames = 29'000}));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  RenderCapture capture{4'000, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);

  // Step 5 is at 30000, which is 1000 frames into this window.
  const std::span<const float> samples = capture.samples();
  for (std::size_t frame = 0; frame < 1'000; ++frame) {
    INFO("frame " << frame << " precedes the seeked step");
    REQUIRE(samples[frame] == 0.0F);
  }
  CHECK(std::any_of(samples.begin() + 1'000, samples.end(),
                    [](float value) { return value != 0.0F; }));
}

TEST_CASE("the sequencer plays a chained song", "[unit]") {
  // Chaining reaching the audio, not just the arithmetic. Two 2-step patterns,
  // each firing a different pad, so which pattern is playing is audible in WHICH
  // pad sounds rather than only in the telemetry.
  engine::Engine eng{test_config()};
  load_pads(eng);

  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = 12'000;  // 6000 frames per step
  state->patterns[0].length = 2;
  state->patterns[0].steps[0][1].on = true;  // pad 1 on the first step
  state->patterns[1].length = 2;
  state->patterns[1].steps[0][0].on = true;  // pad 0 on the first step
  state->song.order[0] = 0;
  state->song.order[1] = 1;
  state->song.length = 2;
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  // Four steps = 24000 frames = one pass through the song.
  RenderCapture capture{24'000, kChannels};
  const std::array<std::size_t, 1> blocks{512};
  capture.render_in_blocks(eng, blocks);

  CHECK(std::any_of(capture.samples().begin(), capture.samples().end(),
                    [](float value) { return value != 0.0F; }));

  // THE assertion: pad 0 appears ONLY in pattern 1, so its glow is proof the
  // song reached the second slot. Checking non-silence alone would pass just as
  // happily with the song ignored entirely -- pattern 0 would loop, pad 1 would
  // sound, and nothing would look wrong. Found by the negative control, which
  // failed to fire on the first version of this test.
  const engine::Telemetry telemetry = eng.telemetry();
  CHECK(telemetry.pad_glow[0].triggered);
  CHECK(telemetry.pad_glow[1].triggered);

  // Having rendered a full pass, the transport is back at slot 0.
  CHECK(telemetry.transport_slot == 0);
  CHECK(telemetry.transport_pattern == 0);
}

TEST_CASE("the song advances the slot at a pattern boundary", "[unit]") {
  engine::Engine eng{test_config()};
  load_pads(eng);

  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = 12'000;
  state->patterns[0].length = 2;
  state->patterns[4].length = 2;
  state->song.order[0] = 0;
  state->song.order[1] = 4;
  state->song.length = 2;
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  // One step in: still slot 0, pattern 0.
  RenderCapture first{6'500, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  first.render_in_blocks(eng, blocks);
  CHECK(eng.telemetry().transport_slot == 0);
  CHECK(eng.telemetry().transport_pattern == 0);

  // Two more steps carries it past the end of pattern 0 into slot 1.
  RenderCapture second{6'500, kChannels};
  second.render_in_blocks(eng, blocks);
  CHECK(eng.telemetry().transport_slot == 1);
  CHECK(eng.telemetry().transport_pattern == 4);
}

TEST_CASE("a song of out-of-range patterns plays without reading past the array", "[unit]") {
  // The audio-thread safety case: song.order arrives from the control thread, so
  // a bad index must be clamped rather than dereferenced. Under ASan this is the
  // test that would report the overrun.
  engine::Engine eng{test_config()};
  load_pads(eng);

  auto state = std::make_shared<rt::SequencerState>();
  state->song.order[0] = 250;
  state->song.order[1] = 99;
  state->song.length = 200;  // past kMaxSongSlots as well
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  RenderCapture capture{48'000, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);

  // No steps are on, so it is silent -- the point is that it got here at all.
  for (const float value : capture.samples()) {
    REQUIRE(value == 0.0F);
  }
  CHECK(eng.telemetry().transport_pattern < rt::kMaxPatterns);
}

TEST_CASE("a chained song is invariant to block size", "[unit]") {
  // Chaining is derived from the absolute step, which is derived from the
  // position, so it inherits invariance -- but a song that resolved its slot
  // once per BLOCK rather than per step would break exactly at the boundaries,
  // and only at some block sizes.
  const auto hash_at = [](std::size_t block) {
    engine::Engine eng{test_config()};
    load_pads(eng);
    auto state = std::make_shared<rt::SequencerState>();
    state->bpm_x100 = 13'700;  // non-integral frames per step
    state->patterns[0].length = 3;
    state->patterns[0].steps[0][1].on = true;
    state->patterns[0].steps[2][1].on = true;
    state->patterns[1].length = 5;
    state->patterns[1].swing = 40;
    state->patterns[1].steps[1][0].on = true;
    state->patterns[1].steps[3][1].on = true;
    state->song.order[0] = 0;
    state->song.order[1] = 1;
    state->song.order[2] = 0;
    state->song.length = 3;
    REQUIRE(eng.publish_sequencer(state));
    REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

    RenderCapture capture{96'000, kChannels};
    const std::array<std::size_t, 1> blocks{block};
    capture.render_in_blocks(eng, blocks);
    return capture.hash();
  };

  const std::uint64_t reference = hash_at(2'048);
  CHECK(hash_at(64) == reference);
  CHECK(hash_at(333) == reference);
  CHECK(hash_at(1'024) == reference);
}

TEST_CASE("the metronome is off unless it is asked for", "[unit]") {
  // The only defensible default. A metronome that started on would put a click
  // in the first render of every session -- including an offline bounce, where
  // it would be baked into the file.
  engine::Engine eng{test_config()};
  REQUIRE(eng.publish_sequencer(std::make_shared<rt::SequencerState>()));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  RenderCapture capture{48'000, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);

  for (const float value : capture.samples()) {
    REQUIRE(value == 0.0F);
  }
}

TEST_CASE("the metronome clicks on the beat", "[unit]") {
  engine::Engine eng{test_config()};  // no pads loaded: the click is all there is

  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = 12'000;  // a beat is 24000 frames
  state->metronome = true;
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  RenderCapture capture{96'000, kChannels};  // four beats
  const std::array<std::size_t, 1> blocks{512};
  capture.render_in_blocks(eng, blocks);

  const std::span<const float> samples = capture.samples();
  const auto loudest_in = [samples](std::size_t from, std::size_t to) {
    float peak = 0.0F;
    for (std::size_t frame = from; frame < to; ++frame) {
      peak = std::max(peak, std::abs(samples[frame]));
    }
    return peak;
  };

  // A click at each beat...
  for (const std::size_t beat :
       {std::size_t{0}, std::size_t{24'000}, std::size_t{48'000}, std::size_t{72'000}}) {
    INFO("beat at frame " << beat);
    REQUIRE(loudest_in(beat, beat + rt::kClickFrames) > 0.1F);
  }

  // ...and silence between them. The click is 1200 frames of a 24000-frame beat,
  // so the gap is real rather than a matter of degree.
  CHECK(loudest_in(2'000, 23'000) == 0.0F);
  CHECK(loudest_in(26'000, 47'000) == 0.0F);
}

TEST_CASE("the metronome accents the downbeat", "[unit]") {
  // The accent is the only thing telling you where the bar is, so it has to be
  // distinguishable rather than merely present.
  engine::Engine eng{test_config()};

  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = 12'000;
  state->metronome = true;
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  RenderCapture capture{48'000, kChannels};
  const std::array<std::size_t, 1> blocks{512};
  capture.render_in_blocks(eng, blocks);

  const std::span<const float> samples = capture.samples();
  const auto loudest_in = [samples](std::size_t from, std::size_t to) {
    float peak = 0.0F;
    for (std::size_t frame = from; frame < to; ++frame) {
      peak = std::max(peak, std::abs(samples[frame]));
    }
    return peak;
  };

  const float downbeat = loudest_in(0, rt::kClickFrames);
  const float offbeat = loudest_in(24'000, 24'000 + rt::kClickFrames);
  INFO("downbeat " << downbeat << " vs beat 2 " << offbeat);
  CHECK(downbeat > offbeat);
}

TEST_CASE("the metronome is invariant to block size", "[unit]") {
  // A click can straddle a block boundary, so this is the case a stateful
  // implementation gets wrong: restarting the table at each block would make the
  // click sound different at 64 frames than at 2048. The table index comes from
  // the absolute frame, so there is no state to get wrong.
  //
  // A tempo whose beat is NOT a multiple of any block size here, so a click
  // straddles a boundary at every one of them.
  const auto hash_at = [](std::size_t block) {
    engine::Engine eng{test_config()};
    auto state = std::make_shared<rt::SequencerState>();
    state->bpm_x100 = 13'700;
    state->metronome = true;
    REQUIRE(eng.publish_sequencer(state));
    REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
    RenderCapture capture{96'000, kChannels};
    const std::array<std::size_t, 1> blocks{block};
    capture.render_in_blocks(eng, blocks);
    return capture.hash();
  };

  const std::uint64_t reference = hash_at(2'048);
  CHECK(hash_at(64) == reference);
  CHECK(hash_at(333) == reference);
  CHECK(hash_at(1'000) == reference);
}

TEST_CASE("the metronome is silent while the transport is stopped", "[unit]") {
  engine::Engine eng{test_config()};
  auto state = std::make_shared<rt::SequencerState>();
  state->metronome = true;
  REQUIRE(eng.publish_sequencer(state));

  RenderCapture capture{48'000, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);

  for (const float value : capture.samples()) {
    REQUIRE(value == 0.0F);
  }
}

TEST_CASE("Engine plays events from the MIDI ring", "[unit]") {
  // MIDI has its own ring because rt::SpscRing is single-PRODUCER and RtMidi
  // delivers on a thread of its own. This is the test that the second ring is
  // actually drained rather than merely declared.
  engine::Engine eng{test_config()};
  load_pads(eng);

  // Pad 0, whose sample outlasts the render. Pad 1's is 1500 frames and would
  // have finished inside this window, so the voice count would read zero for a
  // reason that has nothing to do with the ring.
  REQUIRE(eng.submit_midi_event(rt::PadEvent{.pad = 0, .velocity = 1.0F}));

  RenderCapture capture{2'048, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  capture.render_in_blocks(eng, blocks);

  CHECK(std::any_of(capture.samples().begin(), capture.samples().end(),
                    [](float value) { return value != 0.0F; }));
  CHECK(eng.active_voices() == 1);
  CHECK(eng.dropped_midi_events() == 0);
}

TEST_CASE("the keyboard and MIDI rings are independent", "[unit]") {
  // Filling one must not affect the other. If they shared storage -- or if MIDI
  // were quietly pushed onto the keyboard's ring -- a burst of key repeats would
  // start dropping a controller's notes, which is the kind of interaction nobody
  // would think to look for.
  engine::Engine eng{test_config()};
  load_pads(eng);

  // Nothing renders, so nothing drains: fill the keyboard ring completely.
  std::size_t accepted = 0;
  for (std::size_t i = 0; i < engine::Engine::kEventRingCapacity + 8; ++i) {
    if (eng.trigger_pad(rt::PadEvent{.pad = 1, .velocity = 1.0F})) {
      ++accepted;
    }
  }
  CHECK(accepted == engine::Engine::kEventRingCapacity);
  CHECK(eng.dropped_events() == 8);

  // The MIDI ring is untouched by that.
  CHECK(eng.submit_midi_event(rt::PadEvent{.pad = 2, .velocity = 1.0F}));
  CHECK(eng.dropped_midi_events() == 0);
}

TEST_CASE("Engine counts dropped MIDI events rather than blocking", "[unit]") {
  // A full ring means the audio thread stopped draining, not that a player was
  // too fast -- 256 events between two blocks is not something hands do. Saying
  // so is better than silently succeeding.
  engine::Engine eng{test_config()};
  load_pads(eng);

  std::size_t accepted = 0;
  for (std::size_t i = 0; i < engine::Engine::kMidiRingCapacity + 5; ++i) {
    if (eng.submit_midi_event(rt::PadEvent{.pad = 1, .velocity = 1.0F})) {
      ++accepted;
    }
  }
  CHECK(accepted == engine::Engine::kMidiRingCapacity);
  CHECK(eng.dropped_midi_events() == 5);
  CHECK(eng.dropped_events() == 0);  // and the keyboard's counter is untouched
}

TEST_CASE("a MIDI note-off releases a gate pad", "[unit]") {
  // The note-off path was built engine-side in M3 with the Kitty keyboard as its
  // only producer. MIDI is the second one, and needed no engine change -- this
  // is what shows that claim is true rather than merely intended.
  engine::Engine eng{test_config()};

  auto config = std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = make_test_sample(48'000, 1, 48'000),
                    .pad = 3,
                    .env = rt::AdsrFrames{.attack = 0, .decay = 0, .sustain = 1.0F, .release = 64},
                    .trigger = rt::TriggerMode::kGate});
  REQUIRE(eng.publish_pad_config(config));

  REQUIRE(eng.submit_midi_event(rt::PadEvent{.pad = 3, .velocity = 1.0F}));
  RenderCapture held{2'048, kChannels};
  const std::array<std::size_t, 1> blocks{256};
  held.render_in_blocks(eng, blocks);
  CHECK(eng.active_voices() == 1);  // still sounding: it is a gate pad

  REQUIRE(eng.submit_midi_event(
      rt::PadEvent{.pad = 3, .kind = rt::PadEventKind::kNoteOff, .velocity = 0.0F}));
  RenderCapture released{2'048, kChannels};
  released.render_in_blocks(eng, blocks);
  CHECK(eng.active_voices() == 0);  // the release ran to silence and freed it
}

TEST_CASE("a panic stops a one-shot that a note-off would not", "[unit]") {
  // THE COMPLAINT THIS ANSWERS: a long one-shot could not be stopped once it was
  // playing -- you waited for it. A note-off is not the answer, because ignoring
  // one is the definition of one-shot, so kStop is a separate kind rather than a
  // flag on the existing one.
  engine::Engine eng{test_config()};
  auto config = std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = make_test_sample(48'000, 1, 48'000), .pad = 3});
  REQUIRE(eng.publish_pad_config(config));

  const std::array<std::size_t, 1> blocks{256};

  // A note-off leaves it running, which is what makes the panic necessary.
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 3, .velocity = 1.0F}));
  RenderCapture sounding{2'048, kChannels};
  sounding.render_in_blocks(eng, blocks);
  REQUIRE(eng.active_voices() == 1);

  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 3, .kind = rt::PadEventKind::kNoteOff}));
  RenderCapture ignored{2'048, kChannels};
  ignored.render_in_blocks(eng, blocks);
  CHECK(eng.active_voices() == 1);

  // kStop is not ignored, and the voice is gone within the declick floor rather
  // than cut in a single frame.
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 3, .kind = rt::PadEventKind::kStop}));
  RenderCapture stopped{2'048, kChannels};
  stopped.render_in_blocks(eng, blocks);
  CHECK(eng.active_voices() == 0);
}

TEST_CASE("a panic stops every pad, and does not need a pad number", "[unit]") {
  engine::Engine eng{test_config()};
  for (std::uint8_t pad = 0; pad < 4; ++pad) {
    REQUIRE(eng.publish_pad_config(std::make_shared<const rt::PadConfig>(
        rt::PadConfig{.sample = make_test_sample(48'000, 1, 48'000), .pad = pad})));
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = pad, .velocity = 1.0F}));
  }

  const std::array<std::size_t, 1> blocks{256};
  RenderCapture sounding{2'048, kChannels};
  sounding.render_in_blocks(eng, blocks);
  REQUIRE(eng.active_voices() == 4);

  // `pad` is deliberately a value no pad has: kStopAll must not read it, or a
  // panic would depend on a field nobody sets.
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 200, .kind = rt::PadEventKind::kStopAll}));
  RenderCapture silent{2'048, kChannels};
  silent.render_in_blocks(eng, blocks);
  CHECK(eng.active_voices() == 0);
}

TEST_CASE("a panic does not silence the sequencer on its own", "[unit]") {
  // The reason the `.` key stops the TRANSPORT as well as the voices. Stopping
  // what is sounding is not stopping the machine: the next step arrives a
  // fraction of a beat later and starts it all again, so a panic that only
  // released voices would read as not having worked.
  //
  // This is the engine half of that -- kStopAll alone leaves the transport
  // running, on purpose, because it is src/tui/app.cpp that decides the key
  // means both.
  engine::Engine eng{test_config()};
  REQUIRE(eng.publish_pad_config(std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = make_test_sample(48'000, 1, 48'000), .pad = 0})));

  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = 12'000;  // 6000 frames a step at 48 kHz
  state->patterns[0].length = 16;
  for (std::size_t step = 0; step < 16; ++step) {
    state->patterns[0].steps[step][0].on = true;
    state->patterns[0].steps[step][0].velocity = 127;
  }
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  const std::array<std::size_t, 1> blocks{256};
  RenderCapture running{2'048, kChannels};
  running.render_in_blocks(eng, blocks);
  REQUIRE(eng.active_voices() >= 1);

  REQUIRE(eng.trigger_pad(rt::PadEvent{.kind = rt::PadEventKind::kStopAll}));

  // A whole step later the sequencer has fired again -- silence did not last.
  RenderCapture after{8'192, kChannels};
  after.render_in_blocks(eng, blocks);
  CHECK(eng.active_voices() >= 1);
  CHECK(eng.telemetry().transport_playing);
}

TEST_CASE("a panic plus a transport stop stays silent", "[unit]") {
  // The other half of the case above, and together they are why the `.` key
  // sends both: kStopAll alone is retriggered by the next step, and this is what
  // actually produces silence.
  //
  // WHAT THIS DOES NOT COVER, said plainly: that src/tui/app.cpp's `.` really
  // sends both. That wiring has no test. Under --no-audio nothing renders, so
  // the transport cannot be observed to have stopped, and app.cpp is not
  // reachable from a unit test -- the PTY session can only check that the key
  // reaches the program and reports. Removing the transport half is a change
  // these two cases would not catch; they are here so that the reason it is
  // needed is written down where the code is.
  engine::Engine eng{test_config()};
  REQUIRE(eng.publish_pad_config(std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = make_test_sample(48'000, 1, 48'000), .pad = 0})));

  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = 12'000;
  state->patterns[0].length = 16;
  for (std::size_t step = 0; step < 16; ++step) {
    state->patterns[0].steps[step][0].on = true;
    state->patterns[0].steps[step][0].velocity = 127;
  }
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  const std::array<std::size_t, 1> blocks{256};
  RenderCapture running{2'048, kChannels};
  running.render_in_blocks(eng, blocks);
  REQUIRE(eng.active_voices() >= 1);

  REQUIRE(eng.trigger_pad(rt::PadEvent{.kind = rt::PadEventKind::kStopAll}));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kStop}));

  RenderCapture after{8'192, kChannels};
  after.render_in_blocks(eng, blocks);
  CHECK(eng.active_voices() == 0);
  CHECK_FALSE(eng.telemetry().transport_playing);
  // Silent from the end of the declick floor onward. Not from frame 0: the
  // panic RELEASES rather than cuts, so the first ~0.7 ms is the ramp down --
  // which is the whole point of the floor, and asserting silence there would be
  // asserting the click back in.
  // Per CHANNEL, because samples() is channel-major: frame 0 of the second
  // plane is another ramp, and walking the buffer as one run reports it as a
  // failure eight thousand frames in.
  constexpr std::size_t kCaptured = 8'192;
  const std::span<const float> tail = after.samples();
  for (std::size_t channel = 0; channel < kChannels; ++channel) {
    for (std::size_t frame = rt::kDefaultFadeFrames; frame < kCaptured; ++frame) {
      INFO("channel " << channel << ", frame " << frame);
      REQUIRE(tail[(channel * kCaptured) + frame] == 0.0F);
    }
  }
}

TEST_CASE("adopt_offline drains what nothing is rendering", "[unit]") {
  // With no device open nothing calls render(), so nothing adopts what has been
  // published and the handoff ring fills. It holds 32; `:chop` publishes sixteen
  // configs at once and `:slot assign 1-8 1` publishes eight, so four commands
  // exhausted it and every pad edit after that was refused for the rest of the
  // session. Reported as "slot assign is not working", which it was.
  engine::Engine eng{test_config()};

  const auto publish = [&](std::uint8_t pad) {
    return eng.publish_pad_config(std::make_shared<const rt::PadConfig>(
        rt::PadConfig{.sample = make_test_sample(1'024, 1, 48'000), .pad = pad}));
  };

  // Well past the ring's depth, and in the pattern a session actually produces:
  // rounds of sixteen, with a drain between them.
  for (std::size_t round = 0; round < 8; ++round) {
    for (std::uint8_t pad = 0; pad < rt::kNumPads; ++pad) {
      INFO("round " << round << ", pad " << static_cast<int>(pad));
      REQUIRE(publish(pad));
    }
    eng.adopt_offline();
    static_cast<void>(eng.collect_garbage());
  }
  CHECK(eng.rejected_pad_configs() == 0);

  // And it really adopted rather than merely emptied: the pad plays, which it
  // could not if m_pads were still null.
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));
  const std::array<std::size_t, 1> blocks{256};
  RenderCapture out{512, kChannels};
  out.render_in_blocks(eng, blocks);
  CHECK(eng.active_voices() == 1);
}

TEST_CASE("without a drain the handoff ring fills, which is why adopt_offline exists", "[unit]") {
  // The other half, so the test above cannot pass for the wrong reason. Nothing
  // drains here, and the ring is finite -- publishing without bound must start
  // refusing rather than silently succeeding.
  engine::Engine eng{test_config()};
  std::size_t published = 0;
  for (std::size_t attempt = 0; attempt < 200; ++attempt) {
    if (!eng.publish_pad_config(std::make_shared<const rt::PadConfig>(
            rt::PadConfig{.sample = make_test_sample(1'024, 1, 48'000), .pad = 0}))) {
      break;
    }
    ++published;
  }
  CHECK(published == engine::Engine::kPadHandoffCapacity);
  CHECK(eng.rejected_pad_configs() > 0);
}

// ---------------------------------------------------------------------------
// The mixer graph (M5), measured against the signal path it replaced.
// ---------------------------------------------------------------------------
//
// Before M5 every voice summed into one output buffer in voice-slot order. Now
// each pad's voices sum into their own strip, strips sum into buses and buses
// into the master. The same numbers, added in a different order -- and float
// addition is NOT associative, so that is a change to the output and not merely
// a change to the code.
//
// rt::VoicePool::render_add() is the old path, kept for exactly this: the graph
// needs something to be measured against that is not itself.
namespace {

// Distinct material per pad, so the sums below are of genuinely different
// numbers. Values that repeat would make regrouping exact by accident and every
// assertion here vacuous.
std::shared_ptr<const rt::PadConfig> graph_config(std::uint8_t pad) {
  auto sample = std::make_shared<rt::Sample>(44'100U, kChannels, std::size_t{4'000});
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < data.size(); ++frame) {
      // Irrational-looking but integer-derived: no libm call whose last bit
      // could differ between platforms, and no repeating value.
      const auto mixed = static_cast<float>(((frame * 37) + (static_cast<std::size_t>(pad) * 101) +
                                             (static_cast<std::size_t>(channel) * 7)) %
                                            9'973);
      data[frame] = ((mixed / 4'986.5F) - 1.0F) * 0.31F;
    }
  }
  return std::make_shared<const rt::PadConfig>(rt::PadConfig{
      .sample = std::move(sample), .pad = pad, .gain = 0.4F + (0.03F * static_cast<float>(pad))});
}

// Renders `pads` through the engine's graph, and the same voices through the
// flat pre-M5 sum, and returns both. Triggered in the order given, which is what
// decides voice-slot order and therefore whether the two groupings differ.
struct GraphComparison {
  std::vector<float> graph;
  std::vector<float> flat;
};

GraphComparison compare_paths(std::span<const std::uint8_t> pads, std::size_t frames) {
  constexpr float kVelocity = 0.75F;

  std::vector<std::shared_ptr<const rt::PadConfig>> configs;
  configs.reserve(pads.size());

  engine::Engine eng{test_config()};
  for (const std::uint8_t pad : pads) {
    std::shared_ptr<const rt::PadConfig> config = graph_config(pad);
    REQUIRE(eng.publish_pad_config(config));
    configs.push_back(std::move(config));
  }
  for (const std::uint8_t pad : pads) {
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = pad, .velocity = kVelocity}));
  }

  const std::array<std::size_t, 1> blocks{frames};
  RenderCapture capture{frames, kChannels};
  capture.render_in_blocks(eng, blocks);

  // The oracle: the same configs, the same velocity, the same trigger order,
  // summed flat. A separate pool rather than a flag on the engine -- a test
  // switch inside the thing under test is not a control.
  rt::VoicePool<engine::Engine::kMaxVoices> pool;
  rt::GarbageRing<engine::Engine::kGarbageRingCapacity> garbage;
  for (const std::shared_ptr<const rt::PadConfig>& config : configs) {
    REQUIRE(pool.trigger(config, kVelocity, 48'000U, garbage, 0));
  }

  std::vector<float> flat(frames * kChannels, 0.0F);
  std::array<float*, kChannels> flat_channels{};
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    flat_channels[channel] = flat.data() + (static_cast<std::size_t>(channel) * frames);
  }
  pool.render_add(std::span<float* const>{flat_channels}, frames);

  const std::span<const float> rendered = capture.samples();
  return GraphComparison{.graph = std::vector<float>{rendered.begin(), rendered.end()},
                         .flat = std::move(flat)};
}

}  // namespace

TEST_CASE("the mixer graph sums the same numbers the flat mix did", "[unit]") {
  // Trigger order SCRAMBLED against pad order, which is what makes the two
  // groupings genuinely different: the flat sum walks voice slots (and so
  // trigger order), the graph walks pads. Six voices overlapping from frame 0.
  constexpr std::array<std::uint8_t, 6> kPads{9, 2, 14, 0, 7, 4};
  const GraphComparison result = compare_paths(kPads, 1'024);
  REQUIRE(result.graph.size() == result.flat.size());

  float worst = 0.0F;
  float peak = 0.0F;
  for (std::size_t index = 0; index < result.graph.size(); ++index) {
    worst = std::max(worst, std::abs(result.graph[index] - result.flat[index]));
    peak = std::max(peak, std::abs(result.flat[index]));
  }

  // It rendered something. A hash of silence agrees with a hash of silence.
  CHECK(peak > 0.5F);

  // THE BOUND, and the whole justification for M5 touching the signal path at
  // all: regrouping a sum perturbs it by rounding error and nothing else. 1e-6
  // is -120 dBFS, some 20 dB below the noise floor of 16-bit audio and far below
  // anything a listener or a converter can represent.
  //
  // A larger difference is not "the sum was regrouped", it is a bug in the
  // graph: a strip dropped, a bus counted twice, a voice rendered onto the wrong
  // pad. Those miss by a fraction of full scale, not by a ULP.
  CHECK(worst < 1.0e-6F);
}

TEST_CASE("the graph is bit-exact when trigger order follows pad order", "[unit]") {
  // WHY THE M3 AND M4 GOLDENS DID NOT MOVE, pinned so that it stays true.
  //
  // The plan for this milestone predicted both committed e2e hashes would shift
  // and budgeted a justified re-baseline for it. They did not shift, and this is
  // the reason: adding to a zeroed buffer is exact, so when a pad's voices are
  // contiguous in slot order AND pad order agrees with slot order, the graph
  // performs the identical sequence of additions the flat mix did. Every voice
  // in those two suites is triggered in ascending pad order, so the regrouping
  // is a no-op there.
  //
  // Not a lucky accident to be shrugged at -- a property to keep. This fails if
  // someone reorders the strip walk, or gives a silent bus a gain that is 1.0f
  // to eight decimal places, and it fails HERE, next to the explanation, rather
  // than as an unattributable hash mismatch in an e2e test.
  constexpr std::array<std::uint8_t, 6> kPads{0, 2, 4, 7, 9, 14};
  const GraphComparison result = compare_paths(kPads, 1'024);
  REQUIRE(result.graph.size() == result.flat.size());

  float peak = 0.0F;
  for (const float value : result.flat) {
    peak = std::max(peak, std::abs(value));
  }
  CHECK(peak > 0.5F);

  // Bit-exact, not "close". std::memcmp rather than a loop of ==, because -0.0f
  // and +0.0f compare equal and are different bits -- and a hash is taken over
  // bits.
  CHECK(std::memcmp(result.graph.data(), result.flat.data(), result.graph.size() * sizeof(float)) ==
        0);
}
