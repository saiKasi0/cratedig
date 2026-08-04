// rt::Recorder — capture on the audio thread into storage somebody else owns.
//
// The claims worth testing here are not "audio arrives". They are:
//
//   1. What comes out is what went in, frame for frame, across chunk boundaries
//      the writer never sees.
//   2. A take longer than the whole pool works, because the pool circulates.
//   3. When it does not circulate, frames are DROPPED AND COUNTED rather than
//      silently missing.
//   4. The threshold fires on the frame that crosses it, not on the block.
//   5. Pre-roll gives back what the threshold necessarily throws away.
//
// The threading case at the bottom is the authority for the concurrency, and
// only under TSan (docs/TESTING.md) -- to a single-threaded test, a chunk
// written while somebody else is reading it looks exactly like a chunk written
// before they started.

#include "rt/recorder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint16_t kChannels = 2;
constexpr std::size_t kChunkFrames = 64;
constexpr std::size_t kPoolChunks = 8;
constexpr std::size_t kPreRollFrames = 128;

// The pool the control thread owns, in the shape engine::Engine will build it:
// one flat allocation, chunks as views into it.
class Pool {
 public:
  Pool(std::size_t chunks, std::size_t chunk_frames, std::uint16_t channels,
       std::size_t preroll_frames)
      : m_storage(chunks * chunk_frames * channels, 0.0F),
        m_preroll(preroll_frames * channels, 0.0F),
        m_chunks(chunks) {
    for (std::size_t index = 0; index < chunks; ++index) {
      m_chunks[index] = rt::RecordChunk{
          .data = m_storage.data() + (index * chunk_frames * channels),
          .capacity = chunk_frames,
          .channels = channels,
          .frames = 0,
      };
    }
  }

  void give_to(rt::Recorder& recorder, std::size_t preroll_frames, std::uint16_t channels) {
    recorder.reset(std::span<rt::RecordChunk>{m_chunks}, std::span<float>{m_preroll},
                   preroll_frames, channels);
  }

 private:
  std::vector<float> m_storage;
  std::vector<float> m_preroll;
  std::vector<rt::RecordChunk> m_chunks;
};

// What the control thread does with the chunks that come back: append and
// recycle. One vector per channel, which is the shape rt::Sample wants.
class Take {
 public:
  explicit Take(std::uint16_t channels) : m_channels(channels) {}

  void collect(rt::Recorder& recorder) {
    std::uint16_t index = 0;
    while (recorder.try_collect(index)) {
      const rt::RecordChunk& chunk = recorder.chunk(index);
      for (std::uint16_t channel = 0; channel < m_channels; ++channel) {
        const std::span<const float> frames = chunk.channel(channel);
        m_frames.at(channel).insert(m_frames.at(channel).end(), frames.begin(), frames.end());
      }
      recorder.recycle(index);
    }
  }

  [[nodiscard]] const std::vector<float>& channel(std::uint16_t index) const {
    return m_frames.at(index);
  }

  [[nodiscard]] std::size_t frames() const { return m_frames.at(0).size(); }

 private:
  std::uint16_t m_channels;
  std::array<std::vector<float>, rt::kMaxRecordChannels> m_frames{};
};

// A block whose every frame names itself, so a reassembled take can be checked
// frame by frame rather than by length.
class Ramp {
 public:
  Ramp(std::uint16_t channels, std::size_t block_frames)
      : m_storage(channels, std::vector<float>(block_frames, 0.0F)), m_heads(channels, nullptr) {
    for (std::size_t channel = 0; channel < m_storage.size(); ++channel) {
      m_heads[channel] = m_storage[channel].data();
    }
  }

  std::span<const float* const> block(std::size_t start) {
    for (std::size_t channel = 0; channel < m_storage.size(); ++channel) {
      for (std::size_t frame = 0; frame < m_storage[channel].size(); ++frame) {
        m_storage[channel][frame] = value_at(start + frame, static_cast<std::uint16_t>(channel));
      }
    }
    return std::span<const float* const>{m_heads.data(), m_heads.size()};
  }

  // Exactly representable in a float well past any length these tests use.
  static float value_at(std::size_t frame, std::uint16_t channel) {
    return static_cast<float>(frame) + (static_cast<float>(channel) * kChannelOffset);
  }

  static constexpr float kChannelOffset = 1'000'000.0F;

 private:
  std::vector<std::vector<float>> m_storage;
  std::vector<const float*> m_heads;
};

// A block whose samples a test supplies directly.
class Block {
 public:
  Block(std::uint16_t channels, std::size_t frames)
      : m_storage(channels, std::vector<float>(frames, 0.0F)), m_heads(channels, nullptr) {
    for (std::size_t channel = 0; channel < m_storage.size(); ++channel) {
      m_heads[channel] = m_storage[channel].data();
    }
  }

  void fill(std::size_t frame, float value) {
    for (auto& channel : m_storage) {
      channel[frame] = value;
    }
  }

  void fill_all(float value) {
    for (auto& channel : m_storage) {
      std::fill(channel.begin(), channel.end(), value);
    }
  }

  // Marks a channel the recorder is not supposed to read. Without this a test
  // that offers a NARROW view of a wide block cannot tell duplication from a
  // read past the end of the span -- the storage beyond it is still there and
  // still holds the same audio. That is not hypothetical: it is what let a
  // deliberately broken source_channel() pass this file's mono case.
  void poison(std::uint16_t channel, float value) {
    std::fill(m_storage[channel].begin(), m_storage[channel].end(), value);
  }

  [[nodiscard]] std::span<const float* const> view() const {
    return std::span<const float* const>{m_heads.data(), m_heads.size()};
  }

  [[nodiscard]] std::span<const float* const> view(std::size_t channels) const {
    return std::span<const float* const>{m_heads.data(), channels};
  }

 private:
  std::vector<std::vector<float>> m_storage;
  std::vector<const float*> m_heads;
};

}  // namespace

TEST_CASE("Recorder round-trips a take across chunk boundaries", "[unit]") {
  Pool pool{kPoolChunks, kChunkFrames, kChannels, kPreRollFrames};
  rt::Recorder recorder;
  pool.give_to(recorder, kPreRollFrames, kChannels);

  // A block size that divides no chunk boundary, so chunks are filled by partial
  // blocks and the split path runs rather than being skipped by a tidy fixture.
  constexpr std::size_t kBlock = 25;
  constexpr std::size_t kBlocks = 20;

  Ramp ramp{kChannels, kBlock};
  Take take{kChannels};

  recorder.start(rt::RecordSource::kInput);
  REQUIRE(recorder.is_recording());

  for (std::size_t block = 0; block < kBlocks; ++block) {
    recorder.capture(ramp.block(block * kBlock), kBlock);
    take.collect(recorder);
  }
  recorder.stop();
  take.collect(recorder);

  REQUIRE(recorder.state() == rt::RecordState::kIdle);
  REQUIRE(recorder.dropped_frames() == 0);
  REQUIRE(recorder.frames_captured() == kBlock * kBlocks);
  REQUIRE(take.frames() == kBlock * kBlocks);

  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    for (std::size_t frame = 0; frame < take.frames(); ++frame) {
      REQUIRE(take.channel(channel)[frame] == Ramp::value_at(frame, channel));
    }
  }
}

TEST_CASE("Recorder captures a take many times longer than its pool", "[unit]") {
  Pool pool{kPoolChunks, kChunkFrames, kChannels, kPreRollFrames};
  rt::Recorder recorder;
  pool.give_to(recorder, kPreRollFrames, kChannels);

  // THE POINT OF THE WHOLE MECHANISM. The pool holds 512 frames; this records
  // sixteen times that, which is only possible because chunks go around.
  constexpr std::size_t kBlock = 32;
  constexpr std::size_t kBlocks = 256;
  static_assert(kBlock * kBlocks > kPoolChunks * kChunkFrames * 4,
                "the take must be several pools long or this test proves nothing");

  Ramp ramp{kChannels, kBlock};
  Take take{kChannels};

  recorder.start(rt::RecordSource::kInput);
  for (std::size_t block = 0; block < kBlocks; ++block) {
    recorder.capture(ramp.block(block * kBlock), kBlock);
    take.collect(recorder);
  }
  recorder.stop();
  take.collect(recorder);

  REQUIRE(recorder.dropped_frames() == 0);
  REQUIRE(take.frames() == kBlock * kBlocks);
  REQUIRE(take.channel(0).back() == Ramp::value_at((kBlock * kBlocks) - 1, 0));
}

TEST_CASE("Recorder drops and counts what an exhausted pool cannot hold", "[unit]") {
  Pool pool{kPoolChunks, kChunkFrames, kChannels, kPreRollFrames};
  rt::Recorder recorder;
  pool.give_to(recorder, kPreRollFrames, kChannels);

  // Never collect: the control thread has stalled, which is the failure this
  // counter exists to make visible.
  constexpr std::size_t kBlock = kChunkFrames;
  constexpr std::size_t kBlocks = kPoolChunks * 3;
  constexpr std::size_t kCapacity = kPoolChunks * kChunkFrames;

  Ramp ramp{kChannels, kBlock};
  recorder.start(rt::RecordSource::kInput);
  for (std::size_t block = 0; block < kBlocks; ++block) {
    recorder.capture(ramp.block(block * kBlock), kBlock);
  }
  recorder.stop();

  REQUIRE(recorder.frames_captured() == kCapacity);
  REQUIRE(recorder.dropped_frames() == (kBlock * kBlocks) - kCapacity);

  // And what it did keep is still the FIRST part of the take, uncorrupted: a
  // dropped frame must not shift the ones around it.
  Take take{kChannels};
  take.collect(recorder);
  REQUIRE(take.frames() == kCapacity);
  for (std::size_t frame = 0; frame < kCapacity; ++frame) {
    REQUIRE(take.channel(0)[frame] == Ramp::value_at(frame, 0));
  }
}

TEST_CASE("Recorder keeps the tail when a take stops mid-chunk", "[unit]") {
  Pool pool{kPoolChunks, kChunkFrames, kChannels, kPreRollFrames};
  rt::Recorder recorder;
  pool.give_to(recorder, kPreRollFrames, kChannels);

  // Deliberately not a multiple of the chunk size: the last chunk is partial,
  // and a flush that only handed over FULL chunks would lose these frames --
  // which is the tail of every take a person ever makes.
  constexpr std::size_t kFrames = kChunkFrames + 7;
  Ramp ramp{kChannels, kFrames};

  recorder.start(rt::RecordSource::kInput);
  recorder.capture(ramp.block(0), kFrames);
  recorder.stop();

  Take take{kChannels};
  take.collect(recorder);
  REQUIRE(take.frames() == kFrames);
  REQUIRE(take.channel(0).back() == Ramp::value_at(kFrames - 1, 0));
}

TEST_CASE("Recorder meters its source whatever the state", "[unit]") {
  Pool pool{kPoolChunks, kChunkFrames, kChannels, kPreRollFrames};
  rt::Recorder recorder;
  pool.give_to(recorder, kPreRollFrames, kChannels);

  Block block{kChannels, 16};
  block.fill_all(-0.5F);

  // Idle: not recording, still metering. This is the meter you watch before you
  // commit to a take, so it has to read when nothing is being kept.
  REQUIRE(recorder.state() == rt::RecordState::kIdle);
  recorder.capture(block.view(), 16);
  REQUIRE(recorder.source_peak() == 0.5F);
  REQUIRE(recorder.frames_captured() == 0);

  block.fill_all(0.25F);
  recorder.capture(block.view(), 16);
  REQUIRE(recorder.source_peak() == 0.25F);
}

TEST_CASE("Recorder starts a threshold take on the frame that crosses", "[unit]") {
  Pool pool{kPoolChunks, kChunkFrames, kChannels, kPreRollFrames};
  rt::Recorder recorder;
  pool.give_to(recorder, kPreRollFrames, kChannels);

  constexpr std::size_t kFrames = 32;
  constexpr std::size_t kCrossing = 19;
  constexpr float kThreshold = 0.5F;

  Block block{kChannels, kFrames};
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    // Below the threshold until the crossing, at it from there on. The value
    // encodes its own frame number so the take can be checked for where it
    // BEGAN rather than merely for how long it is.
    block.fill(frame, frame < kCrossing ? 0.1F : kThreshold + static_cast<float>(frame));
  }

  recorder.arm(rt::RecordSource::kInput, kThreshold, 0);
  REQUIRE(recorder.state() == rt::RecordState::kArmed);
  REQUIRE(recorder.frames_captured() == 0);

  recorder.capture(block.view(), kFrames);
  REQUIRE(recorder.state() == rt::RecordState::kRecording);
  recorder.stop();

  Take take{kChannels};
  take.collect(recorder);

  // SAMPLE ACCURATE, not block accurate: the take is exactly the frames from the
  // crossing onward. A block-granular trigger would have kept all 32.
  REQUIRE(take.frames() == kFrames - kCrossing);
  REQUIRE(take.channel(0).front() == kThreshold + static_cast<float>(kCrossing));
}

TEST_CASE("Recorder pre-roll gives back the attack a threshold throws away", "[unit]") {
  // THE MEASUREMENT recorder.hpp cites, and the reason pre-roll exists at all.
  //
  // A needle drop: 500 frames of quiet lead-in, then a 20 ms linear swell to
  // full scale, triggering at -20 dBFS. A threshold trigger CANNOT begin before
  // the threshold, so the take opens on a step from nothing straight to a tenth
  // of full scale -- a click at the head of every take, plus the 2 ms of swell
  // underneath it.
  constexpr std::size_t kLeadIn = 500;
  constexpr float kLeadLevel = 0.02F;
  constexpr std::size_t kFade = 960;  // 20 ms at 48 kHz
  constexpr float kThreshold = 0.1F;  // -20 dBFS
  constexpr std::size_t kFrames = kLeadIn + kFade + 64;

  const auto signal = [&](std::size_t frame) {
    if (frame < kLeadIn) {
      return kLeadLevel;
    }
    const std::size_t into = std::min(frame - kLeadIn, kFade);
    return static_cast<float>(into) / static_cast<float>(kFade);
  };

  // Where the swell first reaches the threshold, found the same way the recorder
  // must find it.
  std::size_t crossing = kFrames;
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    if (signal(frame) >= kThreshold) {
      crossing = frame;
      break;
    }
  }
  REQUIRE(crossing > kLeadIn);
  REQUIRE(crossing < kLeadIn + kFade);

  // The lead-in is nearly five times the pre-roll buffer, so the circular write
  // wraps several times before the trigger. A fixture short enough to fit would
  // never exercise the wrap, which is where the bug would be.
  static_assert(kLeadIn > 3 * kPreRollFrames, "the pre-roll buffer must wrap before the trigger");

  const auto record = [&](std::size_t preroll) {
    Pool pool{kPoolChunks, kChunkFrames, kChannels, kPreRollFrames};
    rt::Recorder recorder;
    pool.give_to(recorder, kPreRollFrames, kChannels);
    Take take{kChannels};

    constexpr std::size_t kBlock = 100;
    recorder.arm(rt::RecordSource::kInput, kThreshold, preroll);
    for (std::size_t at = 0; at < kFrames; at += kBlock) {
      const std::size_t run = std::min(kBlock, kFrames - at);
      Block window{kChannels, run};
      for (std::size_t frame = 0; frame < run; ++frame) {
        window.fill(frame, signal(at + frame));
      }
      recorder.capture(window.view(), run);
      take.collect(recorder);
    }
    recorder.stop();
    take.collect(recorder);
    return std::vector<float>{take.channel(0)};
  };

  const std::vector<float> bare = record(0);
  const std::vector<float> with = record(kPreRollFrames);

  // WITHOUT pre-roll the take opens AT the threshold: everything quieter is
  // gone, and the first sample is a step up from nothing.
  REQUIRE(bare.size() == kFrames - crossing);
  REQUIRE(bare.front() >= kThreshold);

  // WITH it the take opens from a much quieter place, and is longer by exactly
  // the pre-roll asked for.
  REQUIRE(with.size() == bare.size() + kPreRollFrames);
  REQUIRE(with.front() < bare.front());
  REQUIRE(with.front() == kLeadLevel);

  // And the pre-roll is the real audio that preceded the trigger, in order and
  // joined to the take without a seam -- not a fade, and not a guess.
  for (std::size_t frame = 0; frame < with.size(); ++frame) {
    REQUIRE(with[frame] == signal(crossing - kPreRollFrames + frame));
  }
}

TEST_CASE("Recorder spreads a mono source across a stereo take", "[unit]") {
  Pool pool{kPoolChunks, kChunkFrames, kChannels, kPreRollFrames};
  rt::Recorder recorder;
  pool.give_to(recorder, kPreRollFrames, kChannels);

  constexpr std::size_t kFrames = 40;
  Block block{kChannels, kFrames};
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    block.fill(frame, static_cast<float>(frame) * 0.01F);
  }

  // The channel the recorder is NOT being offered, marked so that reading it
  // anyway is visible rather than indistinguishable from doing the right thing.
  constexpr float kPoison = -7.0F;
  block.poison(1, kPoison);

  // One channel offered to a two-channel take. Silence on the right would be the
  // other plausible behaviour and is the wrong one: a mono mic is not a
  // half-silent stereo source.
  recorder.start(rt::RecordSource::kInput);
  recorder.capture(block.view(1), kFrames);
  recorder.stop();

  Take take{kChannels};
  take.collect(recorder);
  REQUIRE(take.frames() == kFrames);
  for (std::size_t frame = 0; frame < kFrames; ++frame) {
    REQUIRE(take.channel(1)[frame] != kPoison);
    REQUIRE(take.channel(1)[frame] == take.channel(0)[frame]);
    REQUIRE(take.channel(0)[frame] == static_cast<float>(frame) * 0.01F);
  }
}

TEST_CASE("Recorder counts frames it was asked to record with no source", "[unit]") {
  Pool pool{kPoolChunks, kChunkFrames, kChannels, kPreRollFrames};
  rt::Recorder recorder;
  pool.give_to(recorder, kPreRollFrames, kChannels);

  // What recording from the input looks like on a stream that has none. The take
  // is empty and the counter says why, which is the difference between a bug
  // report and a shrug.
  recorder.start(rt::RecordSource::kInput);
  recorder.capture(std::span<const float* const>{}, 128);
  recorder.stop();

  REQUIRE(recorder.frames_captured() == 0);
  REQUIRE(recorder.dropped_frames() == 128);
}

TEST_CASE("Recorder loses nothing between an audio thread and a control thread", "[unit][stress]") {
  // THE AUTHORITY FOR THE CONCURRENCY, and only under TSan. Everything above
  // runs on one thread, where a chunk written while somebody else is reading it
  // looks exactly like a chunk written before.
  //
  // The claim: a control thread draining alongside a live audio thread ends up
  // with every frame the audio thread kept, in order and uncorrupted.
  //
  // NOT the claim, and deliberately: that the last chunk is pushed before kIdle
  // is published. It is -- see stop() -- but the window is a few instructions
  // wide, so a test racing to land inside it fails to catch a reversal
  // essentially always. Measured: reversing the two in stop() left this case
  // green. The answer was to stop depending on it. The drain below finishes on
  // the frame COUNT rather than on the state alone, which is the protocol
  // recorder.hpp documents and which no ordering can break.
  Pool pool{kPoolChunks, kChunkFrames, kChannels, kPreRollFrames};
  rt::Recorder recorder;
  pool.give_to(recorder, kPreRollFrames, kChannels);

  constexpr std::size_t kBlock = 17;
  constexpr std::size_t kBlocks = 3'000;

  // Started here rather than inside the thread: otherwise the drain loop below
  // can observe kIdle before the audio thread has begun and finish with an empty
  // take, which is a flaky test rather than a race in the recorder.
  recorder.start(rt::RecordSource::kInput);

  std::thread audio{[&] {
    Ramp ramp{kChannels, kBlock};
    for (std::size_t block = 0; block < kBlocks; ++block) {
      recorder.capture(ramp.block(block * kBlock), kBlock);
    }
    recorder.stop();
  }};

  // BOUNDED. An unbounded spin is how a concurrency bug presents as a hung suite
  // instead of a failed one, which this project has already paid for once in the
  // PTY harnesses (docs/TESTING.md).
  constexpr std::size_t kSpinLimit = 5'000'000;
  Take take{kChannels};
  std::size_t spins = 0;
  while (spins < kSpinLimit) {
    take.collect(recorder);
    if (recorder.state() == rt::RecordState::kIdle && take.frames() >= recorder.frames_captured()) {
      break;
    }
    ++spins;
    std::this_thread::yield();
  }
  audio.join();
  take.collect(recorder);
  REQUIRE(spins < kSpinLimit);

  // This thread may have fallen behind, and dropping frames when it does is the
  // designed behaviour rather than a failure -- so the take is checked against
  // what was KEPT, and every frame of it must be in order and uncorrupted.
  REQUIRE(take.frames() == recorder.frames_captured());
  for (std::size_t frame = 0; frame < take.frames(); ++frame) {
    REQUIRE(take.channel(1)[frame] == take.channel(0)[frame] + Ramp::kChannelOffset);
  }
}
