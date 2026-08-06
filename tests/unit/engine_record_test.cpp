// Recording, through the engine rather than through rt::Recorder directly.
//
// tests/unit/recorder_test.cpp proves the chunk machinery. What is left, and
// what only the engine can answer, is:
//
//   1. Recording the master captures WHAT WAS RENDERED, sample for sample --
//      after the mixer and the limiter, because that is what a listener heard.
//   2. Recording the input captures the input.
//   3. Arming, punching in and stopping cross the command ring and land on the
//      block they arrived in.
//   4. None of it moves a single sample of the output. Recording is a tap; if it
//      were anything else it would have moved every committed hash in the
//      project the day it landed.

#include "engine/engine.hpp"
#include "rt/limiter.hpp"
#include "rt/pad_event.hpp"
#include "rt/recorder.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kMaxBlock = 1'024;
constexpr std::uint32_t kSampleRate = 48'000;

engine::Engine::Config test_config() {
  return engine::Engine::Config{
      .sample_rate = kSampleRate, .num_channels = kChannels, .max_block_frames = kMaxBlock};
}

// One block's worth of buffers in both directions, in the planar layout the
// device layer hands over.
class Duplex {
 public:
  explicit Duplex(std::size_t block_frames)
      : m_block(block_frames),
        m_out_storage(static_cast<std::size_t>(kChannels) * block_frames, 0.0F),
        m_in_storage(static_cast<std::size_t>(kChannels) * block_frames, 0.0F),
        m_out(kChannels, nullptr),
        m_in(kChannels, nullptr) {
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
      m_out[channel] = m_out_storage.data() + (channel * block_frames);
      m_in[channel] = m_in_storage.data() + (channel * block_frames);
    }
  }

  [[nodiscard]] std::span<float* const> out() { return std::span<float* const>{m_out}; }

  [[nodiscard]] std::span<const float* const> in() const {
    return std::span<const float* const>{m_in};
  }

  // An input span of no channels, which is what an output-only stream provides.
  [[nodiscard]] static std::span<const float* const> no_input() { return {}; }

  void write_input(std::size_t channel, std::size_t frame, float value) {
    m_in_storage[(channel * m_block) + frame] = value;
  }

  [[nodiscard]] float output(std::size_t channel, std::size_t frame) const {
    return m_out_storage[(channel * m_block) + frame];
  }

 private:
  std::size_t m_block;
  std::vector<float> m_out_storage;
  std::vector<float> m_in_storage;
  std::vector<float*> m_out;
  std::vector<const float*> m_in;
};

// Something audible on a pad, so a master recording has content to be wrong
// about. A ramp rather than silence or a constant: every frame is distinct, so
// a take that is offset by one frame is visible rather than plausible.
void load_pad(engine::Engine& eng, std::uint8_t pad, std::size_t frames) {
  auto sample = std::make_shared<rt::Sample>(kSampleRate, kChannels, frames);
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    const std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      data[frame] = 0.5F * std::sin(static_cast<float>(frame + (channel * 37)) * 0.01F);
    }
  }
  REQUIRE(eng.set_pad_sample(pad, std::move(sample)));
}

// Everything the engine wrote to the master, concatenated.
std::vector<std::vector<float>> render_blocks(engine::Engine& eng, Duplex& duplex,
                                              std::size_t block_frames, std::size_t blocks,
                                              bool with_input) {
  std::vector<std::vector<float>> master(kChannels);
  for (std::size_t block = 0; block < blocks; ++block) {
    if (with_input) {
      eng.render(duplex.out(), duplex.in(), block_frames);
    } else {
      eng.render(duplex.out(), block_frames);
    }
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
      for (std::size_t frame = 0; frame < block_frames; ++frame) {
        master[channel].push_back(duplex.output(channel, frame));
      }
    }
    static_cast<void>(eng.collect_take());
  }
  return master;
}

}  // namespace

TEST_CASE("Engine recording does not move a single sample of the output", "[unit]") {
  // THE CLAIM THE WHOLE FEATURE RESTS ON. The tap reads and never writes, so a
  // render with recording engaged must be bit-identical to one without. If it
  // were not, every committed hash in this project would have had to move the
  // day recording landed, and none of them did.
  constexpr std::size_t kBlock = 256;
  constexpr std::size_t kBlocks = 40;

  Duplex quiet{kBlock};
  engine::Engine plain{test_config()};
  load_pad(plain, 0, 8'000);
  REQUIRE(plain.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));
  const std::vector<std::vector<float>> without =
      render_blocks(plain, quiet, kBlock, kBlocks, false);

  Duplex loud{kBlock};
  engine::Engine recording{test_config()};
  load_pad(recording, 0, 8'000);
  REQUIRE(recording.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));
  REQUIRE(recording.start_recording(rt::RecordSource::kMaster));
  const std::vector<std::vector<float>> with =
      render_blocks(recording, loud, kBlock, kBlocks, true);

  REQUIRE(recording.take_frames() > 0);
  for (std::size_t channel = 0; channel < kChannels; ++channel) {
    REQUIRE(with[channel].size() == without[channel].size());
    REQUIRE(std::memcmp(with[channel].data(), without[channel].data(),
                        with[channel].size() * sizeof(float)) == 0);
  }
}

TEST_CASE("Engine records the master exactly as it rendered it", "[unit]") {
  constexpr std::size_t kBlock = 128;
  constexpr std::size_t kBlocks = 60;

  Duplex duplex{kBlock};
  engine::Engine eng{test_config()};
  load_pad(eng, 0, 6'000);
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));

  // THE LIMITER ON, AND WORKING. Without this the case cannot tell a tap after
  // the master section from one before it -- the limiter is off by default, so
  // both points produce the same samples and moving the tap would go unnoticed.
  // A ceiling well under the pad's level is what makes the two differ.
  rt::LimiterConfig limiter = rt::make_limiter(-20.0F);
  limiter.enabled = true;
  REQUIRE(eng.set_limiter(limiter));

  // Before the first render, so the command is drained at the top of block one
  // and the take starts where the audio does.
  REQUIRE(eng.start_recording(rt::RecordSource::kMaster));

  const std::vector<std::vector<float>> master = render_blocks(eng, duplex, kBlock, kBlocks, true);

  REQUIRE(eng.stop_recording());
  // ONE MORE BLOCK. A stop is a message, and a message is applied at the top of
  // a render -- so with no further block the recorder never learns about it and
  // the partial chunk in its hand never comes back. That is not a quirk to work
  // around: it is what it means for the audio thread to own the state.
  eng.render(duplex.out(), duplex.in(), kBlock);
  static_cast<void>(eng.collect_take());

  REQUIRE(eng.take_complete());
  REQUIRE(eng.take_frames() == kBlock * kBlocks);

  // The pad really did sound -- otherwise this whole case compares silence to
  // silence and passes for the wrong reason.
  const auto loudest = *std::max_element(master[0].begin(), master[0].end());
  REQUIRE(loudest > 0.01F);

  // And the limiter really did pull it down, so "after the master section" is a
  // claim with something behind it rather than a description of where a line of
  // code happens to sit.
  REQUIRE(loudest < 0.2F);

  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    const std::span<const float> take = eng.take_channel(channel);
    REQUIRE(take.size() == kBlock * kBlocks);
    for (std::size_t frame = 0; frame < take.size(); ++frame) {
      REQUIRE(take[frame] == master[channel][frame]);
    }
  }
}

TEST_CASE("Engine records the input exactly as it arrived", "[unit]") {
  constexpr std::size_t kBlock = 64;
  constexpr std::size_t kBlocks = 30;

  Duplex duplex{kBlock};
  engine::Engine eng{test_config()};

  // A pad sounding at the same time, so the case can tell the input apart from
  // the master. Recording the input while the sampler plays must capture the
  // input and nothing else.
  load_pad(eng, 0, 6'000);
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));
  REQUIRE(eng.start_recording(rt::RecordSource::kInput));

  std::vector<std::vector<float>> expected(kChannels);
  for (std::size_t block = 0; block < kBlocks; ++block) {
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
      for (std::size_t frame = 0; frame < kBlock; ++frame) {
        const auto value =
            static_cast<float>((block * kBlock) + frame) + (static_cast<float>(channel) * 1'000.0F);
        duplex.write_input(channel, frame, value);
        expected[channel].push_back(value);
      }
    }
    eng.render(duplex.out(), duplex.in(), kBlock);
    static_cast<void>(eng.collect_take());
  }

  REQUIRE(eng.stop_recording());
  eng.render(duplex.out(), duplex.in(), kBlock);
  static_cast<void>(eng.collect_take());

  REQUIRE(eng.take_frames() == kBlock * kBlocks);
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    const std::span<const float> take = eng.take_channel(channel);
    for (std::size_t frame = 0; frame < take.size(); ++frame) {
      REQUIRE(take[frame] == expected[channel][frame]);
    }
  }
}

TEST_CASE("Engine record commands land on the block they arrive in", "[unit]") {
  constexpr std::size_t kBlock = 32;
  Duplex duplex{kBlock};
  engine::Engine eng{test_config()};

  REQUIRE(eng.record_state() == rt::RecordState::kIdle);

  // Queued, not applied: nothing has rendered.
  REQUIRE(eng.arm_recording(rt::RecordSource::kInput, 0.0F, 0));
  REQUIRE(eng.record_state() == rt::RecordState::kIdle);

  eng.render(duplex.out(), duplex.in(), kBlock);
  REQUIRE(eng.record_state() == rt::RecordState::kArmed);
  REQUIRE(eng.take_frames() == 0);

  REQUIRE(eng.start_recording(rt::RecordSource::kInput));
  eng.render(duplex.out(), duplex.in(), kBlock);
  REQUIRE(eng.record_state() == rt::RecordState::kRecording);

  // The block the punch-in arrived in is IN the take, not the one after it.
  static_cast<void>(eng.collect_take());
  REQUIRE(eng.telemetry().recorded_frames == kBlock);

  REQUIRE(eng.stop_recording());
  eng.render(duplex.out(), duplex.in(), kBlock);
  REQUIRE(eng.record_state() == rt::RecordState::kIdle);
  static_cast<void>(eng.collect_take());
  REQUIRE(eng.take_frames() == kBlock);
  REQUIRE(eng.take_complete());
}

TEST_CASE("Engine threshold arm starts the take on the input that crossed", "[unit]") {
  constexpr std::size_t kBlock = 64;
  constexpr float kThreshold = 0.5F;
  constexpr std::size_t kQuietBlocks = 5;

  Duplex duplex{kBlock};
  engine::Engine eng{test_config()};
  REQUIRE(eng.arm_recording(rt::RecordSource::kInput, kThreshold, 0));

  // Quiet: armed, metering, keeping nothing.
  for (std::size_t block = 0; block < kQuietBlocks; ++block) {
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
      for (std::size_t frame = 0; frame < kBlock; ++frame) {
        duplex.write_input(channel, frame, 0.05F);
      }
    }
    eng.render(duplex.out(), duplex.in(), kBlock);
    static_cast<void>(eng.collect_take());
  }
  REQUIRE(eng.record_state() == rt::RecordState::kArmed);
  REQUIRE(eng.take_frames() == 0);
  REQUIRE(eng.telemetry().record_peak == 0.05F);

  // Loud from halfway through the next block.
  constexpr std::size_t kCrossing = kBlock / 2;
  for (std::size_t channel = 0; channel < kChannels; ++channel) {
    for (std::size_t frame = 0; frame < kBlock; ++frame) {
      duplex.write_input(channel, frame, frame < kCrossing ? 0.05F : 0.9F);
    }
  }
  eng.render(duplex.out(), duplex.in(), kBlock);
  REQUIRE(eng.record_state() == rt::RecordState::kRecording);

  // One more, so the case proves capture CONTINUES rather than only that it
  // started.
  eng.render(duplex.out(), duplex.in(), kBlock);

  REQUIRE(eng.stop_recording());
  // This block contributes nothing: the stop is applied at the top of it, which
  // is the same rule that puts a punch-in inside the block it arrived in.
  eng.render(duplex.out(), duplex.in(), kBlock);
  static_cast<void>(eng.collect_take());

  // Half of the crossing block plus all of the next one. NOT 128, which is what
  // a block-granular trigger would have kept by starting at the top of the
  // block the crossing happened to land in.
  REQUIRE(eng.take_frames() == (kBlock - kCrossing) + kBlock);
  REQUIRE(eng.take_channel(0)[0] == 0.9F);
}

TEST_CASE("Engine refuses to record over a take it has not been given back", "[unit]") {
  constexpr std::size_t kBlock = 64;
  Duplex duplex{kBlock};
  engine::Engine eng{test_config()};

  REQUIRE(eng.start_recording(rt::RecordSource::kInput));
  eng.render(duplex.out(), duplex.in(), kBlock);
  REQUIRE(eng.stop_recording());
  eng.render(duplex.out(), duplex.in(), kBlock);
  static_cast<void>(eng.collect_take());
  REQUIRE(eng.take_frames() > 0);

  // A second take now would be spliced onto the first, which is a take that
  // never happened presented as one that did.
  REQUIRE_FALSE(eng.arm_recording(rt::RecordSource::kInput, 0.0F, 0));
  REQUIRE_FALSE(eng.start_recording(rt::RecordSource::kInput));

  eng.discard_take();
  REQUIRE(eng.take_frames() == 0);
  REQUIRE(eng.take_complete());
  REQUIRE(eng.arm_recording(rt::RecordSource::kInput, 0.0F, 0));
}

TEST_CASE("Engine records a take longer than its whole chunk pool", "[unit]") {
  // Sixteen times the pool, which is only possible because collect_take() runs
  // between blocks and hands the chunks back.
  constexpr std::size_t kBlock = 512;
  const std::size_t pool_frames =
      engine::Engine::kRecordPoolChunks * engine::Engine::kRecordChunkFrames;
  const std::size_t blocks = ((pool_frames * 4) / kBlock) + 1;

  Duplex duplex{kBlock};
  engine::Engine eng{test_config()};
  REQUIRE(eng.start_recording(rt::RecordSource::kInput));

  for (std::size_t block = 0; block < blocks; ++block) {
    eng.render(duplex.out(), duplex.in(), kBlock);
    static_cast<void>(eng.collect_take());
  }
  REQUIRE(eng.stop_recording());
  eng.render(duplex.out(), duplex.in(), kBlock);
  static_cast<void>(eng.collect_take());

  REQUIRE(eng.take_frames() == kBlock * blocks);
  REQUIRE(eng.take_frames() > pool_frames * 4);
  REQUIRE(eng.telemetry().record_dropped_frames == 0);
}

TEST_CASE("Engine counts what a stalled collector cost the take", "[unit]") {
  // The same run with the tick never happening, which is what a wedged UI looks
  // like from the audio thread. The pool fills and the rest of the take is gone
  // -- and the count says so, which is the whole point of having one.
  constexpr std::size_t kBlock = 512;
  const std::size_t pool_frames =
      engine::Engine::kRecordPoolChunks * engine::Engine::kRecordChunkFrames;
  const std::size_t blocks = ((pool_frames * 2) / kBlock);

  Duplex duplex{kBlock};
  engine::Engine eng{test_config()};
  REQUIRE(eng.start_recording(rt::RecordSource::kInput));

  for (std::size_t block = 0; block < blocks; ++block) {
    eng.render(duplex.out(), duplex.in(), kBlock);
  }
  REQUIRE(eng.stop_recording());
  eng.render(duplex.out(), duplex.in(), kBlock);

  const engine::Telemetry telemetry = eng.telemetry();
  REQUIRE(telemetry.recorded_frames == pool_frames);
  REQUIRE(telemetry.record_dropped_frames == (kBlock * blocks) - pool_frames);
}

TEST_CASE("Engine meters the recorder's source before anything is kept", "[unit]") {
  constexpr std::size_t kBlock = 64;
  Duplex duplex{kBlock};
  engine::Engine eng{test_config()};

  for (std::size_t channel = 0; channel < kChannels; ++channel) {
    for (std::size_t frame = 0; frame < kBlock; ++frame) {
      duplex.write_input(channel, frame, -0.4F);
    }
  }
  eng.render(duplex.out(), duplex.in(), kBlock);

  // Idle, nothing armed, nothing kept -- and the meter still reads, because
  // that is what it is for: deciding whether to commit to a take at all.
  const engine::Telemetry telemetry = eng.telemetry();
  REQUIRE(telemetry.record_state == rt::RecordState::kIdle);
  REQUIRE(telemetry.record_peak == 0.4F);
  REQUIRE(telemetry.recorded_frames == 0);
}

TEST_CASE("Engine records nothing from an output-only stream and says so", "[unit]") {
  constexpr std::size_t kBlock = 64;
  Duplex duplex{kBlock};
  engine::Engine eng{test_config()};

  REQUIRE(eng.start_recording(rt::RecordSource::kInput));
  for (std::size_t block = 0; block < 4; ++block) {
    // The output-only render: no input span at all, which is what an offline
    // bounce and a device with no capture side both provide.
    eng.render(duplex.out(), kBlock);
    static_cast<void>(eng.collect_take());
  }

  REQUIRE(eng.take_frames() == 0);
  REQUIRE(eng.telemetry().record_dropped_frames == kBlock * 4);
  REQUIRE(eng.telemetry().record_peak == 0.0F);
}

TEST_CASE("Engine adopts record commands with nothing rendering", "[unit]") {
  // --no-audio. Nothing will ever be captured, but the ring must not fill: a
  // producer with no consumer is how the audition path broke twice in M4.5, and
  // the failure is invisible until every later command is silently refused.
  engine::Engine eng{test_config()};

  for (std::size_t round = 0; round < engine::Engine::kRecordRingCapacity * 4; ++round) {
    REQUIRE(eng.start_recording(rt::RecordSource::kInput));
    REQUIRE(eng.stop_recording());
    eng.adopt_offline();
  }
  REQUIRE(eng.record_state() == rt::RecordState::kIdle);
}

TEST_CASE("Engine builds a take into a Sample the crate can hold", "[unit]") {
  // The copy the interface used to write out by hand, where a test can reach
  // it. A loop that fills a Sample one channel at a time is right until
  // somebody edits it, and there was nothing to notice if they did.
  constexpr std::size_t kBlock = 64;
  constexpr std::size_t kBlocks = 20;

  Duplex duplex{kBlock};
  engine::Engine eng{test_config()};
  REQUIRE(eng.start_recording(rt::RecordSource::kInput));

  std::vector<std::vector<float>> expected(kChannels);
  for (std::size_t block = 0; block < kBlocks; ++block) {
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
      for (std::size_t frame = 0; frame < kBlock; ++frame) {
        // Distinct per channel, so a build that filled both from channel 0
        // would be visible rather than plausible.
        const auto value = static_cast<float>((block * kBlock) + frame) * 0.001F +
                           (static_cast<float>(channel) * 100.0F);
        duplex.write_input(channel, frame, value);
        expected[channel].push_back(value);
      }
    }
    eng.render(duplex.out(), duplex.in(), kBlock);
    static_cast<void>(eng.collect_take());
  }
  REQUIRE(eng.stop_recording());
  eng.render(duplex.out(), duplex.in(), kBlock);
  static_cast<void>(eng.collect_take());

  const std::shared_ptr<rt::Sample> sample = eng.build_take();
  REQUIRE(sample != nullptr);
  CHECK(sample->num_frames() == kBlock * kBlocks);
  CHECK(sample->num_channels() == kChannels);
  CHECK(sample->sample_rate() == kSampleRate);

  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    const std::span<const float> audio = sample->channel(channel);
    REQUIRE(audio.size() == expected[channel].size());
    for (std::size_t frame = 0; frame < audio.size(); ++frame) {
      REQUIRE(audio[frame] == expected[channel][frame]);
    }
  }

  // BUILDING DOES NOT CONSUME. The caller may want the spans as well -- to
  // write a file without a second copy of the audio -- so the take survives
  // until it is discarded.
  CHECK(eng.take_frames() == kBlock * kBlocks);
  eng.discard_take();
  CHECK(eng.build_take() == nullptr);
}
