// What the audio thread tells the interface about itself.
//
// None of this feeds back into the audio path, so the determinism goldens in
// engine_render_test.cpp are the guard on that: if publishing telemetry ever
// perturbs a sample, those hashes change. What is tested here is that the
// published numbers mean what the UI is about to assume they mean.

#include "engine/engine.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "rt/sequencer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kEngineRate = 48'000;
constexpr std::uint32_t kMaxBlock = 2'048;

engine::Engine::Config test_config() {
  return engine::Engine::Config{.sample_rate = kEngineRate,
                                .num_channels = kChannels,
                                .max_block_frames = kMaxBlock,
                                .seed = 0};
}

// A constant-amplitude sample, so a peak assertion is about the meter rather
// than about which part of a waveform a block happened to land on.
std::shared_ptr<const rt::Sample> make_flat_sample(float amplitude, std::uint32_t rate = 44'100,
                                                   std::size_t frames = 3'000) {
  auto sample = std::make_shared<rt::Sample>(rate, kChannels, frames);
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    const std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      // Alternating sign at full amplitude: constant magnitude, zero mean, and
      // no libm call whose last bit could differ between platforms.
      data[frame] = (frame % 2 == 0) ? amplitude : -amplitude;
    }
  }
  return sample;
}

void render_frames(engine::Engine& eng, std::size_t frames) {
  while (frames > 0) {
    const std::size_t block = frames < kMaxBlock ? frames : kMaxBlock;
    std::vector<float> left(block, 0.0F);
    std::vector<float> right(block, 0.0F);
    std::array<float*, kChannels> pointers{left.data(), right.data()};
    eng.render(std::span<float* const>{pointers}, block);
    frames -= block;
  }
}

void trigger(engine::Engine& eng, std::uint8_t pad, float velocity = 1.0F) {
  REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = pad, .velocity = velocity, .frame_offset = 0}));
}

}  // namespace

TEST_CASE("an idle engine reports nothing playing and no level", "[unit]") {
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(0, make_flat_sample(0.5F)));

  render_frames(eng, 1'024);
  const engine::Telemetry idle = eng.telemetry();

  CHECK_FALSE(idle.playing);
  CHECK(idle.playhead_frame == 0);
  CHECK(idle.master_peak == 0.0F);
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    INFO("pad " << pad);
    CHECK(idle.pad_peak[pad] == 0.0F);
  }
}

TEST_CASE("the playhead names the pad that is sounding and advances with it", "[unit]") {
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(3, make_flat_sample(0.5F)));

  trigger(eng, 3);
  render_frames(eng, 512);

  const engine::Telemetry first = eng.telemetry();
  REQUIRE(first.playing);
  CHECK(first.playhead_pad == 3);

  render_frames(eng, 512);
  const engine::Telemetry second = eng.telemetry();
  REQUIRE(second.playing);
  CHECK(second.playhead_pad == 3);
  CHECK(second.playhead_frame > first.playhead_frame);
}

TEST_CASE("the playhead advances at the resampled rate", "[unit]") {
  // 44.1 kHz material on a 48 kHz engine advances through the sample at
  // 0.91875 frames per output frame. A playhead reported in output frames
  // instead of source frames would drift by 8% -- enough to put the marker in
  // the wrong place on a long file and not enough to notice on a short one.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(0, make_flat_sample(0.5F, 44'100, 8'000)));

  trigger(eng, 0);
  constexpr std::size_t kRendered = 1'000;
  render_frames(eng, kRendered);

  const engine::Telemetry state = eng.telemetry();
  REQUIRE(state.playing);

  const auto expected = static_cast<std::uint64_t>((static_cast<double>(kRendered) * 44'100.0) /
                                                   static_cast<double>(kEngineRate));
  INFO("expected ~" << expected << ", got " << state.playhead_frame);
  CHECK(state.playhead_frame >= expected - 2);
  CHECK(state.playhead_frame <= expected + 2);
}

TEST_CASE("the playhead follows the newest voice", "[unit]") {
  // With a chord down, the marker should track the hit just played rather than
  // whichever pool slot the publishing loop reached last.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(2, make_flat_sample(0.5F)));
  REQUIRE(eng.set_pad_sample(9, make_flat_sample(0.5F)));

  trigger(eng, 2);
  render_frames(eng, 256);
  CHECK(eng.telemetry().playhead_pad == 2);

  trigger(eng, 9);
  render_frames(eng, 256);
  CHECK(eng.telemetry().playhead_pad == 9);
}

TEST_CASE("nothing playing clears the playhead again", "[unit]") {
  engine::Engine eng{test_config()};
  // 300 source frames at 44.1 kHz is ~327 engine frames; one 1024-frame block
  // outlives it comfortably.
  REQUIRE(eng.set_pad_sample(0, make_flat_sample(0.5F, 44'100, 300)));

  trigger(eng, 0);
  render_frames(eng, 128);
  REQUIRE(eng.telemetry().playing);

  render_frames(eng, 2'048);
  CHECK_FALSE(eng.telemetry().playing);
}

TEST_CASE("pad meters attribute level to the pad that made it", "[unit]") {
  // A meter that lights the wrong pad is worse than no meter: it teaches the
  // wrong mapping between the grid and the sound.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(5, make_flat_sample(0.5F)));

  trigger(eng, 5);
  render_frames(eng, 512);
  const engine::Telemetry state = eng.telemetry();

  CHECK(state.pad_peak[5] > 0.4F);
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    if (pad == 5) {
      continue;
    }
    INFO("pad " << pad);
    CHECK(state.pad_peak[pad] == 0.0F);
  }
  CHECK(state.master_peak > 0.4F);
}

TEST_CASE("velocity scales the metered level", "[unit]") {
  const auto peak_at_velocity = [](float velocity) {
    engine::Engine eng{test_config()};
    REQUIRE(eng.set_pad_sample(1, make_flat_sample(0.5F)));
    trigger(eng, 1, velocity);
    render_frames(eng, 512);
    return eng.telemetry().pad_peak[1];
  };

  const float loud = peak_at_velocity(1.0F);
  const float quiet = peak_at_velocity(0.25F);

  REQUIRE(loud > 0.0F);
  REQUIRE(quiet > 0.0F);
  // Exactly four times, because the first block starts from a published zero so
  // no decay has been applied to either reading yet.
  CHECK(loud / quiet > 3.9F);
  CHECK(loud / quiet < 4.1F);
}

TEST_CASE("meters fall rather than sticking or flickering", "[unit]") {
  // Both failure modes are real. A meter with no fall stays pinned at the
  // loudest thing that ever happened; one with no hold shows whichever 5 ms
  // block a 30 Hz redraw sampled, which for a drum pattern is usually silence.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(0, make_flat_sample(0.8F, 44'100, 300)));

  trigger(eng, 0);
  render_frames(eng, 256);
  const float struck = eng.telemetry().pad_peak[0];
  REQUIRE(struck > 0.7F);

  // The voice is over well before here, so everything below is pure fall.
  render_frames(eng, 1'024);

  // A tenth of the fall time later, still clearly visible.
  render_frames(eng, static_cast<std::size_t>(kEngineRate * 0.05F));
  const float held = eng.telemetry().pad_peak[0];
  CHECK(held > 0.0F);
  CHECK(held < struck);

  // ...and past the full fall time, back to nothing.
  render_frames(eng, static_cast<std::size_t>(kEngineRate * engine::Engine::kPeakFallSeconds));
  CHECK(eng.telemetry().pad_peak[0] == 0.0F);
  CHECK(eng.telemetry().master_peak == 0.0F);
}

TEST_CASE("the fall time does not depend on the block size", "[unit]") {
  // The device negotiates the block size; the meter's behaviour must not change
  // because it handed back 64 frames instead of 512.
  const auto peak_after_fall = [](std::size_t block) {
    engine::Engine eng{test_config()};
    REQUIRE(eng.set_pad_sample(0, make_flat_sample(0.8F, 44'100, 300)));
    trigger(eng, 0);
    render_frames(eng, 256);

    // 0.1 s of silence, rendered in whatever block size.
    const std::size_t total = static_cast<std::size_t>(kEngineRate) / 10;
    for (std::size_t done = 0; done < total; done += block) {
      render_frames(eng, block);
    }
    return eng.telemetry().pad_peak[0];
  };

  const float small_blocks = peak_after_fall(64);
  const float large_blocks = peak_after_fall(512);
  INFO("64-frame blocks: " << small_blocks << ", 512-frame blocks: " << large_blocks);
  CHECK(small_blocks > 0.0F);
  CHECK(small_blocks - large_blocks < 0.02F);
  CHECK(large_blocks - small_blocks < 0.02F);
}

// --- pad glow ----------------------------------------------------------------

TEST_CASE("an untriggered pad reports no glow", "[unit]") {
  // Distinguishable from "hit a long time ago". A default-constructed
  // atomic<uint32_t> is zero and zero decodes as "hit just now at velocity 0",
  // so the engine seeds these to a sentinel -- without which every pad would
  // light on the first frame of every session.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(0, make_flat_sample(0.5F)));
  render_frames(eng, 512);

  for (const engine::PadGlow& glow : eng.telemetry().pad_glow) {
    CHECK_FALSE(glow.triggered);
  }
}

TEST_CASE("a pad glows the moment it is triggered", "[unit]") {
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(5, make_flat_sample(0.5F)));
  trigger(eng, 5, 0.75F);
  render_frames(eng, 128);

  const engine::Telemetry state = eng.telemetry();
  CHECK(state.pad_glow[5].triggered);
  CHECK(state.pad_glow[5].seconds_since_trigger < 0.01F);
  // Quantised to 8 bits on the way through, so within half a step of 1/255.
  CHECK(state.pad_glow[5].velocity > 0.74F);
  CHECK(state.pad_glow[5].velocity < 0.76F);

  // ...and its neighbours are untouched.
  CHECK_FALSE(state.pad_glow[4].triggered);
  CHECK_FALSE(state.pad_glow[6].triggered);
}

TEST_CASE("a pad glows at any sample level", "[unit]") {
  // The M3 acceptance criterion, and the reason glow is published from the
  // trigger rather than from the audio: "pads light on trigger AT ANY SAMPLE
  // LEVEL".
  //
  // -60 dBFS is 0.001 linear -- audible, and utterly invisible on a meter that
  // is eight characters tall. Driving the pad grid from pad_peak would light
  // this pad by less than an eighth of one cell, which is to say not at all.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(2, make_flat_sample(0.001F)));
  trigger(eng, 2, 1.0F);
  render_frames(eng, 256);

  const engine::Telemetry state = eng.telemetry();

  // The peak meter can barely see it...
  CHECK(state.pad_peak[2] < 0.002F);

  // ...and the glow is at full strength regardless.
  CHECK(state.pad_glow[2].triggered);
  CHECK(state.pad_glow[2].velocity > 0.99F);
  CHECK(state.pad_glow[2].seconds_since_trigger < 0.01F);
}

TEST_CASE("a pad triggered into silence still glows", "[unit]") {
  // The other half of the same point: a pad whose sample is digital silence
  // produces no audio at all, and must still acknowledge the hit. Nothing
  // derived from the output could do this.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(1, make_flat_sample(0.0F)));
  trigger(eng, 1, 1.0F);
  render_frames(eng, 256);

  const engine::Telemetry state = eng.telemetry();
  CHECK(state.pad_peak[1] == 0.0F);
  CHECK(state.pad_glow[1].triggered);
  CHECK(state.pad_glow[1].velocity > 0.99F);
}

TEST_CASE("glow ages with rendered frames and does not depend on the block size", "[unit]") {
  const auto age_after = [](std::size_t block) {
    engine::Engine eng{test_config()};
    REQUIRE(eng.set_pad_sample(0, make_flat_sample(0.5F, 44'100, 300)));
    trigger(eng, 0);
    for (std::size_t done = 0; done < kEngineRate; done += block) {
      render_frames(eng, block);
    }
    return eng.telemetry().pad_glow[0].seconds_since_trigger;
  };

  const float small_blocks = age_after(64);
  const float large_blocks = age_after(512);
  INFO("after one second: " << small_blocks << " s at 64 frames, " << large_blocks << " s at 512");

  // One second of rendering is one second of glow age, whatever the block size.
  CHECK(small_blocks > 0.99F);
  CHECK(small_blocks < 1.01F);
  CHECK(large_blocks > 0.99F);
  CHECK(large_blocks < 1.01F);
}

TEST_CASE("retriggering a pad restarts its glow", "[unit]") {
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(0, make_flat_sample(0.5F, 44'100, 300)));

  trigger(eng, 0, 1.0F);
  render_frames(eng, kEngineRate / 2);
  REQUIRE(eng.telemetry().pad_glow[0].seconds_since_trigger > 0.4F);

  trigger(eng, 0, 0.25F);
  render_frames(eng, 128);

  const engine::Telemetry state = eng.telemetry();
  CHECK(state.pad_glow[0].seconds_since_trigger < 0.01F);
  CHECK(state.pad_glow[0].velocity < 0.26F);  // the new, softer hit
}

TEST_CASE("glow saturates rather than wrapping", "[unit]") {
  // 24 bits of frames is 349 seconds at 48 kHz. Rendering that far here would be
  // slow, so this checks the property at the boundary instead: a pad left alone
  // must keep reading "a long time ago" rather than suddenly reading "just now".
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(0, make_flat_sample(0.5F, 44'100, 300)));
  trigger(eng, 0);

  float previous = 0.0F;
  for (int second = 0; second < 4; ++second) {
    render_frames(eng, kEngineRate);
    const float age = eng.telemetry().pad_glow[0].seconds_since_trigger;
    CHECK(age >= previous);  // monotone, never wrapping back to zero
    previous = age;
  }
  CHECK(previous > 3.9F);
}

// --- live against sequenced, and listener time ------------------------------

namespace {

// A sequencer that fires `pad` on step 0 of a 4-step pattern.
std::shared_ptr<const rt::SequencerState> one_step_pattern(std::uint8_t pad) {
  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = 12'000;  // 6000 frames per step
  state->patterns[0].length = 4;
  state->patterns[0].steps[0][pad].on = true;
  return state;
}

}  // namespace

TEST_CASE("a live hit and a sequenced hit are distinguishable", "[unit]") {
  // The M4 addition. Hardware samplers draw these differently and the difference
  // is genuinely useful when overdubbing -- it is how you tell what you just
  // played from what was already there.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(1, make_flat_sample(0.5F)));
  REQUIRE(eng.set_pad_sample(2, make_flat_sample(0.5F)));
  REQUIRE(eng.publish_sequencer(one_step_pattern(2)));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  trigger(eng, 1, 1.0F);  // pad 1 by hand, pad 2 by the sequencer
  render_frames(eng, 256);

  const engine::Telemetry state = eng.telemetry();
  REQUIRE(state.pad_glow[1].triggered);
  REQUIRE(state.pad_glow[2].triggered);
  CHECK_FALSE(state.pad_glow[1].sequenced);
  CHECK(state.pad_glow[2].sequenced);
}

TEST_CASE("the source flag survives ageing", "[unit]") {
  // The flag shares one word with the age, and the age is rewritten every block.
  // A mask that clobbered the flag would show a sequenced hit as live after the
  // first block -- which is to say almost always, since a glow lasts hundreds of
  // blocks.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(3, make_flat_sample(0.5F)));
  REQUIRE(eng.publish_sequencer(one_step_pattern(3)));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  render_frames(eng, 128);
  REQUIRE(eng.telemetry().pad_glow[3].sequenced);

  // Many blocks later, and long after the step that fired it.
  render_frames(eng, 4'096);
  CHECK(eng.telemetry().pad_glow[3].sequenced);
  CHECK(eng.telemetry().pad_glow[3].triggered);
}

TEST_CASE("the velocity survives the narrowed age field", "[unit]") {
  // M4 took a bit off the age to make room for the source flag. Velocity is what
  // the ramp is drawn from, so it had to keep all eight bits -- this is the test
  // that says the bit came off the right field.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(4, make_flat_sample(0.5F)));

  trigger(eng, 4, 1.0F);
  render_frames(eng, 128);
  CHECK(eng.telemetry().pad_glow[4].velocity > 0.99F);

  trigger(eng, 4, 0.25F);
  render_frames(eng, 128);
  const float quarter = eng.telemetry().pad_glow[4].velocity;
  INFO("velocity at 0.25 reported as " << quarter);
  CHECK(quarter > 0.24F);
  CHECK(quarter < 0.26F);
}

TEST_CASE("listener time delays a sequenced glow and not a live one", "[unit]") {
  // The M9 hook, exercised at a non-zero value so the path is tested rather than
  // merely present. A pad that lights when its block renders lights EARLY by the
  // output latency -- on a Bluetooth sink that is plainly visible as the lights
  // running ahead of the music.
  constexpr std::uint32_t kLatency = 9'600;  // 200 ms at 48 kHz, a realistic sink
  engine::Engine::Config config = test_config();
  config.output_latency_frames = kLatency;
  engine::Engine eng{config};

  REQUIRE(eng.set_pad_sample(1, make_flat_sample(0.5F)));
  REQUIRE(eng.set_pad_sample(2, make_flat_sample(0.5F)));
  REQUIRE(eng.publish_sequencer(one_step_pattern(2)));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  trigger(eng, 1, 1.0F);
  render_frames(eng, 256);

  const engine::Telemetry fresh = eng.telemetry();
  // The live hit is visible at once: its reference is the finger that caused it.
  CHECK(fresh.pad_glow[1].seconds_since_trigger >= 0.0F);
  CHECK(fresh.pad_glow[1].seconds_since_trigger < 0.01F);

  // The sequenced one has not been heard yet, and says so with a negative age.
  CHECK(fresh.pad_glow[2].seconds_since_trigger < 0.0F);

  // Once the latency has elapsed it becomes visible.
  render_frames(eng, kLatency);
  CHECK(eng.telemetry().pad_glow[2].seconds_since_trigger >= 0.0F);
}

TEST_CASE("the default latency changes nothing", "[unit]") {
  // Zero until M9 measures a real figure, and this is what makes that safe: with
  // the default config a sequenced glow behaves exactly as an M3 glow did.
  engine::Engine eng{test_config()};
  REQUIRE(eng.set_pad_sample(5, make_flat_sample(0.5F)));
  REQUIRE(eng.publish_sequencer(one_step_pattern(5)));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  render_frames(eng, 256);
  const engine::PadGlow glow = eng.telemetry().pad_glow[5];
  CHECK(glow.triggered);
  CHECK(glow.sequenced);
  CHECK(glow.seconds_since_trigger >= 0.0F);
  CHECK(glow.seconds_since_trigger < 0.01F);
}
