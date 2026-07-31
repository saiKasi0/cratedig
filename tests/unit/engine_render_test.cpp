#include "engine/engine.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <array>
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
