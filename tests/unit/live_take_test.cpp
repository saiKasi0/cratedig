// Playing a pattern in rather than typing it: the audio thread reporting live
// hits, and those hits becoming steps.
//
// tests/unit/take_test.cpp proves the quantiser against arithmetic. This is the
// half only a running engine can answer:
//
//   1. A hit is reported at the frame it SOUNDED at, inside the block, not at
//      the block boundary and not at whatever time the control thread noticed.
//   2. The sequencer's own hits are not reported -- otherwise an overdub fills a
//      pattern with copies of itself.
//   3. Nothing is reported while the transport is stopped, because there is no
//      position for it to mean anything against.
//   4. End to end: play a figure by hand over one loop, and the sequencer plays
//      it back.

#include "engine/engine.hpp"
#include "engine/take.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "rt/sequencer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kMaxBlock = 1'024;
constexpr std::uint32_t kRate = 48'000;

// 120 bpm, so a sixteenth is exactly 6000 frames and every expectation below is
// arithmetic rather than approximation.
constexpr std::uint32_t kBpmX100 = 12'000;
constexpr std::uint64_t kStepFrames = 6'000;

engine::Engine::Config test_config() {
  return engine::Engine::Config{
      .sample_rate = kRate, .num_channels = kChannels, .max_block_frames = kMaxBlock};
}

// Output buffers in the planar layout the device layer hands over.
class Sink {
 public:
  explicit Sink(std::size_t block_frames)
      : m_storage(static_cast<std::size_t>(kChannels) * block_frames, 0.0F), m_heads(kChannels) {
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
      m_heads[channel] = m_storage.data() + (channel * block_frames);
    }
  }

  [[nodiscard]] std::span<float* const> channels() { return std::span<float* const>{m_heads}; }

 private:
  std::vector<float> m_storage;
  std::vector<float*> m_heads;
};

void load_pad(engine::Engine& eng, std::uint8_t pad) {
  auto sample = std::make_shared<rt::Sample>(kRate, kChannels, std::size_t{2'000});
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    const std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < data.size(); ++frame) {
      data[frame] = 0.25F;
    }
  }
  REQUIRE(eng.set_pad_sample(pad, std::move(sample)));
}

std::shared_ptr<rt::SequencerState> fresh_state() {
  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = kBpmX100;
  state->patterns[0].length = 16;
  return state;
}

std::vector<rt::PadHit> drain(engine::Engine& eng) {
  std::vector<rt::PadHit> hits;
  rt::PadHit hit{};
  while (eng.next_hit(hit)) {
    hits.push_back(hit);
  }
  return hits;
}

}  // namespace

TEST_CASE("Engine reports a live hit at the frame it sounded", "[unit]") {
  constexpr std::size_t kBlock = 256;
  Sink sink{kBlock};
  engine::Engine eng{test_config()};
  load_pad(eng, 4);

  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
  eng.render(sink.channels(), kBlock);
  REQUIRE(drain(eng).empty());  // nothing played yet

  // A hit placed 100 frames into the second block, which starts at frame 256.
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 4, .velocity = 1.0F, .frame_offset = 100}));
  eng.render(sink.channels(), kBlock);

  const std::vector<rt::PadHit> hits = drain(eng);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].pad == 4);

  // 356, NOT 256 and not 512. The whole reason this crosses a ring at all is
  // that the exact frame is knowable only here -- rounding to the block would
  // put a third of a step of error into every take at this tempo.
  CHECK(hits[0].frame == kBlock + 100);
  CHECK(hits[0].velocity == rt::kMaxVelocity);
}

TEST_CASE("Engine reports nothing while the transport is stopped", "[unit]") {
  constexpr std::size_t kBlock = 128;
  Sink sink{kBlock};
  engine::Engine eng{test_config()};
  load_pad(eng, 0);

  // Playable, and played -- but with no transport there is no position, so there
  // is nothing a step could be recorded against.
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F}));
  eng.render(sink.channels(), kBlock);
  CHECK(drain(eng).empty());
  CHECK(eng.active_voices() > 0);  // it really did sound
}

TEST_CASE("Engine does not report the sequencer's own hits", "[unit]") {
  constexpr std::size_t kBlock = 512;
  Sink sink{kBlock};
  engine::Engine eng{test_config()};
  load_pad(eng, 2);

  auto state = fresh_state();
  for (std::size_t step = 0; step < 16; ++step) {
    state->patterns[0].steps[step][2] = rt::Step{.on = true, .velocity = 100};
  }
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  // A whole 16-step bar, every step firing.
  std::size_t sequenced_voices = 0;
  for (std::size_t block = 0; block < (16 * kStepFrames) / kBlock; ++block) {
    eng.render(sink.channels(), kBlock);
    sequenced_voices += eng.active_voices();
  }

  // The pattern really did play -- otherwise this is a case about silence.
  REQUIRE(sequenced_voices > 0);

  // And not one of those hits came back. Recording them would write the pattern
  // into itself, every pass, for ever.
  CHECK(drain(eng).empty());
}

TEST_CASE("Engine reports a hit on a pad with nothing loaded", "[unit]") {
  constexpr std::size_t kBlock = 128;
  Sink sink{kBlock};
  engine::Engine eng{test_config()};

  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = 9, .velocity = 0.5F}));
  eng.render(sink.channels(), kBlock);

  // It made no sound, and it was still played. A take is a record of what
  // somebody did, and a pad they loaded afterwards should have their part on it.
  CHECK(eng.active_voices() == 0);
  const std::vector<rt::PadHit> hits = drain(eng);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].pad == 9);
}

TEST_CASE("a figure played by hand comes back from the sequencer", "[unit]") {
  // THE ACCEPTANCE, end to end and without a terminal: play pads against a
  // rolling transport, record what comes back, and let the sequencer play it.
  constexpr std::size_t kBlock = 250;  // divides no step boundary
  Sink sink{kBlock};
  engine::Engine eng{test_config()};

  auto state = fresh_state();
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  // Four on the floor on pad 0, offbeat hats on pad 1 -- played EARLY or LATE by
  // a few hundred frames each, the way a person does.
  struct Played {
    std::uint8_t pad;
    std::size_t step;
    std::int64_t slop;
  };

  const std::vector<Played> figure{
      {0, 0, 0},    {1, 2, +700},  {0, 4, -500},  {1, 6, +300},
      {0, 8, +200}, {1, 10, -600}, {0, 12, -200}, {1, 14, +800},
  };

  for (const Played& played : figure) {
    load_pad(eng, played.pad);
  }

  const std::size_t total_frames = 16 * kStepFrames;
  std::size_t next = 0;
  for (std::uint64_t at = 0; at < total_frames; at += kBlock) {
    // Anything due inside this block goes in at its exact offset, which is what
    // a keyboard or a MIDI port would produce.
    while (next < figure.size()) {
      const auto target = static_cast<std::uint64_t>(
          static_cast<std::int64_t>(figure[next].step * kStepFrames) + figure[next].slop);
      if (target >= at + kBlock) {
        break;
      }
      REQUIRE(eng.trigger_pad(rt::PadEvent{
          .pad = figure[next].pad,
          .velocity = 1.0F,
          .frame_offset = static_cast<std::uint32_t>(target - at),
      }));
      ++next;
    }
    eng.render(sink.channels(), kBlock);
  }
  REQUIRE(next == figure.size());

  // The take: everything the audio thread reported, quantised into the pattern.
  rt::SequencerState recorded = *state;
  const std::vector<rt::PadHit> hits = drain(eng);
  REQUIRE(hits.size() == figure.size());
  for (const rt::PadHit& hit : hits) {
    REQUIRE(engine::record_hit(recorded, kRate, engine::kQuantiseSixteenth, hit));
  }

  // Every note landed on the step it was aiming at, slop and all.
  for (const Played& played : figure) {
    INFO("pad " << int{played.pad} << " step " << played.step);
    CHECK(recorded.patterns[0].steps[played.step][played.pad].on);
  }

  // And nothing else did. Without this the case would pass on a recorder that
  // set every step of every pad it saw.
  std::size_t on_count = 0;
  for (const auto& step : recorded.patterns[0].steps) {
    for (const rt::Step& cell : step) {
      on_count += cell.on ? 1 : 0;
    }
  }
  CHECK(on_count == figure.size());

  // Now play it back, and it fires. The pattern is not just data that looks
  // right -- the sequencer has to act on it.
  REQUIRE(eng.publish_sequencer(std::make_shared<const rt::SequencerState>(recorded)));
  REQUIRE(eng.send_transport(
      rt::TransportCommand{.kind = rt::TransportCommandKind::kSeek, .position_frames = 0}));

  std::size_t sounded = 0;
  for (std::uint64_t at = 0; at < total_frames; at += kBlock) {
    eng.render(sink.channels(), kBlock);
    sounded += eng.active_voices();
  }
  CHECK(sounded > 0);

  // The playback was the SEQUENCER's, so none of it came back as live hits --
  // which is also what stops a second pass from doubling the pattern.
  CHECK(drain(eng).empty());
}

TEST_CASE("Engine counts live hits it had to drop", "[unit]") {
  constexpr std::size_t kBlock = 64;
  Sink sink{kBlock};
  engine::Engine eng{test_config()};
  load_pad(eng, 0);
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  // Never drained, which is what a stalled UI looks like from the audio thread.
  // The ring fills and the rest are lost -- and counted, because a note missing
  // from a take with no explanation is unreportable.
  std::size_t sent = 0;
  while (sent < engine::Engine::kHitRingCapacity * 2) {
    if (eng.trigger_pad(rt::PadEvent{.pad = 0, .velocity = 1.0F})) {
      ++sent;
    }
    eng.render(sink.channels(), kBlock);
  }

  CHECK(eng.dropped_hits() > 0);
  CHECK(eng.dropped_hits() == sent - engine::Engine::kHitRingCapacity);
}
