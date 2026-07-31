// The M4 acceptance, offline and end to end: a recorded pattern, played by the
// sequencer, rendered bit-exact -- and MIDI bytes reaching the audio.
//
// docs/ROADMAP.md states it in one line -- "recorded pattern renders bit-exact
// offline; MIDI integration test green" -- and it is tested in the two layers
// docs/TESTING.md already draws. This file is the offline half: the real engine,
// the real sequencer, no terminal and no device, checking the audio that comes
// out. tests/e2e/pty_sequencer_session.py is the other half, and writes the
// pattern by typing at the real binary.
//
// WHY THE SEQUENCER IS ON THE AUDIO THREAD, WHICH THIS FILE IS THE PROOF OF
// -------------------------------------------------------------------------
// Every test below calls Engine::render() in a plain loop. There is no control
// thread in existence, nothing posts an event, and the pattern still plays --
// because fire_sequencer_steps() runs inside render(). A sequencer that
// generated events from the control thread would be simpler to write and could
// not be tested here at all, which is the whole argument recorded in
// src/rt/sequencer.hpp and docs/ARCHITECTURE.md.
//
// WHY THE GOLDEN HASH IS PORTABLE, WHICH IS NOT AN ACCIDENT
// ---------------------------------------------------------
// Same rule as tests/e2e/chop_e2e_test.cpp, which states it at length: a
// committed hash of real audio is only portable when the signal path contains no
// a*b+c that one target fuses into an FMA and another does not. The path taken
// here has none that matters, for three reasons, all of which are properties of
// THIS configuration rather than of the engine:
//
//   - The material is 48 kHz, the engine is 48 kHz and pitch_ratio is 1.0, so the
//     phase fraction is exactly 0 at every frame and hermite4(a, b, c, d, 0.0f)
//     collapses to exactly b.
//   - The pads carry the default envelope, a flat sustain, so level() never
//     evaluates its `m_start + m_step * position` ramp.
//   - EVERY STEP IS AT VELOCITY 127, which is 127/127.0f == 1.0f exactly. That is
//     the one thing this file has to be careful about that the M3 test did not:
//     a step at velocity 100 would give a gain of 0.7874..., the product
//     amplitude*value would round, and `channels[c][f] += scaled` with a second
//     voice already in the accumulator would then depend on whether the multiply
//     and the add were fused. At gain 1.0 the product is exact, and fma(a, b, c)
//     and (a*b)+c agree to the bit when a*b is exact.
//
// The declick fade is in the hashed region and is fine for the same reason: its
// gain is `into * inv_fade_in` with inv_fade_in == 1/32, and the material is
// integers over 32768, so every product is exact too.
//
// A future test that gives a step a real velocity, retunes a pad, or turns the
// metronome on belongs in the other case and must assert INVARIANCE rather than a
// committed constant -- the metronome case below does exactly that, and says so.

#include "engine/engine.hpp"
#include "ingest/decoder.hpp"
#include "io/midi_message.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "rt/sequencer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint32_t kRate = 48'000;
constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kMaxBlock = 2'048;

engine::Engine::Config e2e_config() {
  return engine::Engine::Config{
      .sample_rate = kRate, .num_channels = kChannels, .max_block_frames = kMaxBlock, .seed = 0};
}

// -- the material -------------------------------------------------------------

// Four one-shots in one sample, 0.25 s apart. Each is a short percussive body,
// and the pads take one each -- so "pad 2 fired" is a statement about audio
// rather than about a counter.
constexpr std::size_t kHitSpacing = 12'000;
constexpr std::size_t kBodyFrames = 3'000;
constexpr std::size_t kSliceFrames = 4'000;  // body plus a tail of silence
constexpr std::size_t kNumSounds = 4;
constexpr std::size_t kMaterialFrames = kHitSpacing * kNumSounds;

[[nodiscard]] constexpr std::size_t sound_start(std::size_t index) noexcept {
  return index * kHitSpacing;
}

// NO libm ANYWHERE, deliberately: std::sin is not required to give the same last
// bit on two platforms, so a golden hash over material generated with it would be
// a golden hash over the host's libm. Everything below is an integer over 32768 --
// a power of two, so every value is exact in a float and identical on every
// machine by construction. Same rule as make_loop() in chop_e2e_test.cpp.
[[nodiscard]] std::shared_ptr<const rt::Sample> make_kit() {
  auto sample = std::make_shared<rt::Sample>(kRate, kChannels, kMaterialFrames);

  constexpr int kPeakAmplitude = 30'000;  // of 32768

  std::span<float> left = sample->mutable_channel(0);
  std::span<float> right = sample->mutable_channel(1);

  for (std::size_t sound = 0; sound < kNumSounds; ++sound) {
    const std::size_t start = sound_start(sound);

    // 24, 18, 12 and 6 frames per period: 1 kHz up to 4 kHz. Four sounds nobody
    // could confuse, which is what lets a test say WHICH pad it is hearing.
    const std::size_t period = 24 - (sound * 6);

    for (std::size_t offset = 0; offset < kBodyFrames; ++offset) {
      const auto decay = static_cast<int>(
          (static_cast<std::size_t>(kPeakAmplitude) * (kBodyFrames - offset)) / kBodyFrames);
      const int value = ((offset / period) % 2 == 0) ? decay : -decay;

      // 1/32768 exactly, so the float holds the integer with no rounding.
      const float scaled = static_cast<float>(value) / 32'768.0F;
      left[start + offset] = scaled;
      // Half amplitude on the right: still exact, and different enough that a bug
      // collapsing the two channels into one shows in the hash rather than
      // hiding behind identical planes.
      right[start + offset] = scaled * 0.5F;
    }
  }

  return sample;
}

// One sound per pad, published through the documented handoff protocol.
[[nodiscard]] std::size_t assign_kit(engine::Engine& eng,
                                     const std::shared_ptr<const rt::Sample>& sample) {
  std::size_t published = 0;
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    rt::PadConfig config{};
    config.pad = static_cast<std::uint8_t>(pad);
    if (pad < kNumSounds) {
      config.sample = sample;
      config.start_frame = sound_start(pad);
      config.end_frame = sound_start(pad) + kSliceFrames;
    }
    if (!eng.publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(config)))) {
      return published;
    }
    ++published;
  }
  return published;
}

// -- the pattern --------------------------------------------------------------

// 120.00 bpm at 48 kHz is exactly 6000 frames per sixteenth, so every step
// boundary in this file is a round number and a failure reads as a frame count
// rather than as arithmetic to redo by hand.
constexpr std::uint32_t kBpmX100 = 12'000;
constexpr std::size_t kFramesPerStep = 6'000;
constexpr std::size_t kPatternSteps = 16;

// FULL VELOCITY EVERYWHERE. Not a detail -- see the note at the top of the file
// about why a committed hash depends on it.
constexpr std::uint8_t kFullVelocity = 127;

void put(rt::Pattern& pattern, std::size_t pad, std::initializer_list<std::size_t> steps) {
  for (const std::size_t step : steps) {
    pattern.steps[step][pad].on = true;
    pattern.steps[step][pad].velocity = kFullVelocity;
  }
}

// A beat with a shape: four on the floor, backbeat, offbeat hats, and one late
// hit on pad 4 so the end of the bar is not empty. Every pad appears on steps no
// other pad shares as well as on steps it does, which is what lets the per-pad
// timing test below tell them apart.
[[nodiscard]] rt::Pattern make_beat() {
  rt::Pattern pattern;
  pattern.length = kPatternSteps;
  put(pattern, 0, {0, 4, 8, 12});
  put(pattern, 1, {4, 12});
  put(pattern, 2, {2, 6, 10, 14});
  put(pattern, 3, {15});
  return pattern;
}

[[nodiscard]] std::shared_ptr<const rt::SequencerState> make_state(const rt::Pattern& pattern) {
  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = kBpmX100;
  state->patterns[0] = pattern;
  return state;
}

// Long enough to cover the whole bar and run into the next one, so the WRAP is
// in the hashed region: a sequencer that played sixteen steps and then stopped
// would pass every assertion that ended at the bar line.
constexpr std::size_t kRenderFrames = 100'000;

// -- the harness --------------------------------------------------------------

// Renders the transport and keeps every sample, channel-major.
//
// The block sizes cycle, and NOTHING is pushed from outside during the render:
// the sequencer is the only source of triggers, which is what makes block-size
// invariance a statement about the sequencer rather than about when this test
// happened to post an event.
class Render {
 public:
  Render(std::size_t total_frames, std::uint16_t channels)
      : m_channels(channels), m_total_frames(total_frames), m_data(total_frames * channels, 0.0F) {}

  void run(engine::Engine& eng, std::span<const std::size_t> block_sizes) {
    std::vector<float> scratch(static_cast<std::size_t>(kMaxBlock) * m_channels, 0.0F);
    std::vector<float*> channel_pointers(m_channels);

    std::size_t done = 0;
    std::size_t next_size = 0;
    while (done < m_total_frames) {
      const std::size_t block =
          std::min(block_sizes[next_size % block_sizes.size()], m_total_frames - done);
      ++next_size;
      if (block == 0) {
        continue;
      }

      // Dirtied before every call: a render() that fails to write a sample must
      // show as garbage rather than as a leftover zero nobody notices.
      std::fill(scratch.begin(), scratch.end(), -7.5F);
      for (std::uint16_t channel = 0; channel < m_channels; ++channel) {
        channel_pointers[channel] =
            scratch.data() + (static_cast<std::size_t>(channel) * kMaxBlock);
      }

      eng.render(std::span<float* const>{channel_pointers}, block);

      for (std::uint16_t channel = 0; channel < m_channels; ++channel) {
        std::memcpy(m_data.data() + (static_cast<std::size_t>(channel) * m_total_frames) + done,
                    channel_pointers[channel], block * sizeof(float));
      }
      done += block;
    }
  }

  [[nodiscard]] std::span<const float> channel(std::uint16_t index) const {
    return std::span<const float>{m_data}.subspan(static_cast<std::size_t>(index) * m_total_frames,
                                                  m_total_frames);
  }

  [[nodiscard]] float peak() const {
    float loudest = 0.0F;
    for (const float value : m_data) {
      loudest = std::max(loudest, value < 0.0F ? -value : value);
    }
    return loudest;
  }

  // FNV-1a over the raw bytes, the harness tests/unit/engine_render_test.cpp
  // established: a changed hash is a changed behaviour, to be explained in the
  // commit rather than re-baselined (docs/TESTING.md).
  [[nodiscard]] std::uint64_t hash() const {
    constexpr std::uint64_t kOffsetBasis = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(m_data.data());
    const std::size_t byte_count = m_data.size() * sizeof(float);

    std::uint64_t hash = kOffsetBasis;
    for (std::size_t index = 0; index < byte_count; ++index) {
      hash ^= bytes[index];
      hash *= kPrime;
    }
    return hash;
  }

 private:
  std::uint16_t m_channels;
  std::size_t m_total_frames;
  std::vector<float> m_data;
};

struct Session {
  std::uint64_t hash = 0;
  float peak = 0.0F;
  std::uint64_t dropped_triggers = 0;
  std::uint64_t rejected_states = 0;
  std::uint64_t garbage_overflows = 0;
  std::size_t published = 0;
  engine::Telemetry telemetry;
};

// Publishes a sequencer, starts the transport, renders, and reports.
//
// The transport command and the state both go through the rings the real program
// uses, so this exercises adopt_sequencer() and drain_transport() rather than
// reaching past them.
[[nodiscard]] Session run_session(std::span<const std::size_t> block_sizes,
                                  const std::shared_ptr<const rt::SequencerState>& state,
                                  const std::shared_ptr<const rt::Sample>& material,
                                  std::size_t frames = kRenderFrames) {
  Session session;
  engine::Engine eng{e2e_config()};
  session.published = assign_kit(eng, material);
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  Render render{frames, kChannels};
  render.run(eng, block_sizes);

  session.hash = render.hash();
  session.peak = render.peak();
  session.dropped_triggers = eng.dropped_triggers();
  session.rejected_states = eng.rejected_sequencer_states();
  session.garbage_overflows = eng.garbage_overflows();
  session.telemetry = eng.telemetry();
  return session;
}

// Where the signal starts after each silence, in frames.
//
// READ OFF THE AUDIO rather than taken from telemetry, which is the point: a
// counter saying a step fired proves the counter was incremented. The declick
// fade opens at exactly zero, so a voice's first frame is silent and its onset is
// one frame after it starts -- consistent, and stated here rather than hidden in
// a tolerance.
[[nodiscard]] std::vector<std::size_t> onsets(std::span<const float> samples) {
  std::vector<std::size_t> found;
  bool sounding = false;
  for (std::size_t frame = 0; frame < samples.size(); ++frame) {
    const bool now = samples[frame] != 0.0F;
    if (now && !sounding) {
      found.push_back(frame);
    }
    sounding = now;
  }
  return found;
}

// The frames one pad's steps should land on, given a pattern and a swing.
[[nodiscard]] std::vector<std::size_t> expected_onsets(const rt::Pattern& pattern, std::size_t pad,
                                                       std::size_t frames) {
  std::vector<std::size_t> want;
  for (std::uint64_t step = 0;; ++step) {
    const std::uint64_t at = rt::step_frame(step, kRate, kBpmX100, pattern.swing);
    if (at >= frames) {
      break;
    }
    if (pattern.steps[step % rt::pattern_length(pattern)][pad].on) {
      // +1: the declick fade's first frame is exactly zero, so the audio starts
      // the frame after the voice does.
      want.push_back(static_cast<std::size_t>(at) + 1);
    }
  }
  return want;
}

// A pattern with only one pad's steps in it, so its onsets can be measured
// without another pad's audio in the way.
[[nodiscard]] rt::Pattern only_pad(const rt::Pattern& source, std::size_t pad) {
  rt::Pattern out;
  out.length = source.length;
  out.swing = source.swing;
  for (std::size_t step = 0; step < rt::kMaxSteps; ++step) {
    out.steps[step][pad] = source.steps[step][pad];
  }
  return out;
}

}  // namespace

TEST_CASE("e2e: the sequencer renders a recorded pattern bit-exact", "[e2e]") {
  const std::array<std::size_t, 1> blocks{512};
  const rt::Pattern beat = make_beat();
  const Session session = run_session(blocks, make_state(beat), make_kit());

  // Sixteen pads published, four with sounds. Nothing refused anywhere.
  CHECK(session.published == rt::kNumPads);
  CHECK(session.rejected_states == 0);
  CHECK(session.dropped_triggers == 0);
  CHECK(session.garbage_overflows == 0);

  // It made a sound. Trivial to state, and the thing every other assertion here
  // would happily hold for if it did not: a bit-exact hash of silence is still
  // bit-exact.
  CHECK(session.peak > 0.5F);

  // And the transport moved. A sequencer that never advanced its position would
  // fire step 0 forever, which is audible but would still hash consistently.
  CHECK(session.telemetry.transport_playing);
  CHECK(session.telemetry.transport_frames == kRenderFrames);

  // THE COMMITTED GOLDEN. This is docs/ROADMAP.md's "recorded pattern renders
  // bit-exact offline" for M4. It changes only when the audible behaviour of the
  // sequencer changes, and such a change is explained in the commit that makes
  // it -- never re-baselined to make this line pass (CLAUDE.md, "Never fix a
  // golden file").
  CHECK(session.hash == 0xAB0E2D88E2A4609DULL);
}

TEST_CASE("e2e: the sequenced pattern is invariant to block size", "[e2e]") {
  // The property the whole determinism contract rests on, and the one the
  // sequencer is most able to break: step positions are computed from the
  // absolute frame rather than accumulated per block precisely so that this
  // holds. An engine that played the same pattern differently at 64 frames than
  // at 2048 is not deterministic however repeatable each of those runs is.
  const std::shared_ptr<const rt::Sample> material = make_kit();
  const std::shared_ptr<const rt::SequencerState> state = make_state(make_beat());

  const std::array<std::size_t, 1> one_block{kMaxBlock};
  const std::array<std::size_t, 1> small{64};
  const std::array<std::size_t, 3> mixed{128, 512, 333};

  // Fixed seed, so the ragged sizes are the same on every run and on every
  // machine -- a varying input cannot prove determinism.
  std::mt19937 rng{4'242};
  std::uniform_int_distribution<std::size_t> sizes{1, kMaxBlock};
  std::array<std::size_t, 64> ragged{};
  for (std::size_t& size : ragged) {
    size = sizes(rng);
  }

  const std::uint64_t reference = run_session(one_block, state, material).hash;
  CHECK(run_session(small, state, material).hash == reference);
  CHECK(run_session(mixed, state, material).hash == reference);
  CHECK(run_session(ragged, state, material).hash == reference);
}

TEST_CASE("e2e: a swung pattern is invariant to block size too", "[e2e]") {
  // Swing is where block-size invariance is easiest to lose: an accumulator
  // advanced once per block rounds the shift away entirely at large blocks, and
  // a step pushed late can land in a block whose scan started after its unswung
  // base. Asserted as invariance rather than against a constant, because it is
  // the property that matters and it needs no committed value to be true.
  rt::Pattern swung = make_beat();
  swung.swing = 58;

  const std::shared_ptr<const rt::Sample> material = make_kit();
  const std::shared_ptr<const rt::SequencerState> state = make_state(swung);

  const std::array<std::size_t, 1> one_block{kMaxBlock};
  const std::array<std::size_t, 1> small{64};
  const std::array<std::size_t, 3> mixed{128, 512, 333};

  const std::uint64_t reference = run_session(one_block, state, material).hash;
  CHECK(run_session(small, state, material).hash == reference);
  CHECK(run_session(mixed, state, material).hash == reference);

  // And it is a different rendering from the straight one, which is what stops
  // the three lines above passing on a build where swing does nothing at all.
  CHECK(reference != run_session(one_block, make_state(make_beat()), material).hash);
}

TEST_CASE("e2e: the same pattern renders the same bytes every run", "[e2e]") {
  const std::shared_ptr<const rt::Sample> material = make_kit();
  const std::shared_ptr<const rt::SequencerState> state = make_state(make_beat());
  const std::array<std::size_t, 3> blocks{256, 128, 700};

  CHECK(run_session(blocks, state, material).hash == run_session(blocks, state, material).hash);
}

TEST_CASE("e2e: every step lands on its own frame", "[e2e]") {
  // The assertion that makes this a SEQUENCER test rather than a playback test.
  // A hash cannot tell you the hits were in the right places, only that they were
  // in the same places as last time -- and every timing test above would pass
  // just as happily on an engine that fired every step at its block boundary,
  // which is exactly the bug sample-accurate offsets exist to prevent.
  const std::shared_ptr<const rt::Sample> material = make_kit();
  const rt::Pattern beat = make_beat();

  // Ragged blocks on purpose: 333 frames is not a multiple of anything here, so
  // a step quantised to a block edge would be visibly late.
  const std::array<std::size_t, 3> blocks{333, 128, 1'000};

  for (std::size_t pad = 0; pad < kNumSounds; ++pad) {
    const rt::Pattern solo = only_pad(beat, pad);

    engine::Engine eng{e2e_config()};
    REQUIRE(assign_kit(eng, material) == rt::kNumPads);
    REQUIRE(eng.publish_sequencer(make_state(solo)));
    REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

    Render render{kRenderFrames, kChannels};
    render.run(eng, blocks);

    const std::vector<std::size_t> got = onsets(render.channel(0));
    const std::vector<std::size_t> want = expected_onsets(beat, pad, kRenderFrames);

    INFO("pad " << pad + 1 << ": " << got.size() << " onsets, expected " << want.size());
    REQUIRE_FALSE(want.empty());
    REQUIRE(got.size() == want.size());
    for (std::size_t hit = 0; hit < want.size(); ++hit) {
      INFO("pad " << pad + 1 << ", hit " << hit);
      CHECK(got[hit] == want[hit]);
    }
  }
}

TEST_CASE("e2e: swing pushes the odd steps late and leaves the even ones", "[e2e]") {
  // What swing IS, measured in frames on the output rather than asserted about
  // the arithmetic that produced it. A swing that moved every step, or that moved
  // the odd ones by the wrong amount, would still be block-size invariant and
  // still hash consistently.
  const std::shared_ptr<const rt::Sample> material = make_kit();

  // Every step of one pad, so the shift of each is visible on its own.
  rt::Pattern straight;
  straight.length = kPatternSteps;
  for (std::size_t step = 0; step < kPatternSteps; ++step) {
    straight.steps[step][2].on = true;
    straight.steps[step][2].velocity = kFullVelocity;
  }
  rt::Pattern swung = straight;
  swung.swing = 50;

  const std::array<std::size_t, 1> blocks{256};
  constexpr std::size_t kBar = kFramesPerStep * kPatternSteps;

  const auto measure = [&](const rt::Pattern& pattern) {
    engine::Engine eng{e2e_config()};
    REQUIRE(assign_kit(eng, material) == rt::kNumPads);
    REQUIRE(eng.publish_sequencer(make_state(pattern)));
    REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
    Render render{kBar, kChannels};
    render.run(eng, blocks);
    return onsets(render.channel(0));
  };

  const std::vector<std::size_t> even_feel = measure(straight);
  const std::vector<std::size_t> swung_feel = measure(swung);

  REQUIRE(even_feel.size() == kPatternSteps);
  REQUIRE(swung_feel.size() == kPatternSteps);

  // 50% of one step, and a step is exactly 6000 frames at this tempo.
  constexpr std::size_t kShift = kFramesPerStep / 2;
  for (std::size_t step = 0; step < kPatternSteps; ++step) {
    INFO("step " << step + 1);
    if (step % 2 == 0) {
      CHECK(swung_feel[step] == even_feel[step]);
    } else {
      CHECK(swung_feel[step] == even_feel[step] + kShift);
    }
  }
}

TEST_CASE("e2e: a chained song plays its patterns in order", "[e2e]") {
  // Two patterns, each with a pad the other never uses, so which one is playing
  // is a question the audio answers. A song that ignored its order and repeated
  // the selected pattern would be perfectly deterministic and perfectly wrong.
  auto state = std::make_shared<rt::SequencerState>();
  state->bpm_x100 = kBpmX100;

  state->patterns[0].length = kPatternSteps;
  put(state->patterns[0], 0, {0, 4, 8, 12});

  state->patterns[1].length = kPatternSteps;
  put(state->patterns[1], 3, {0, 4, 8, 12});

  state->song.order[0] = 0;
  state->song.order[1] = 1;
  state->song.length = 2;

  // The selected pattern is 1, which is NOT the first slot. A song that used the
  // selection rather than the order would start on the wrong pattern, and
  // starting them the same way would hide that.
  state->selected_pattern = 1;

  constexpr std::size_t kBar = kFramesPerStep * kPatternSteps;

  engine::Engine eng{e2e_config()};
  REQUIRE(assign_kit(eng, make_kit()) == rt::kNumPads);
  REQUIRE(eng.publish_sequencer(state));
  REQUIRE(eng.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));

  // Rendered a bar at a time so the telemetry can be read at each boundary,
  // which is the same thing the pattern lane reads.
  const std::array<std::size_t, 1> blocks{512};
  Render first{kBar, kChannels};
  first.run(eng, blocks);
  const engine::Telemetry after_first = eng.telemetry();

  Render second{kBar, kChannels};
  second.run(eng, blocks);
  const engine::Telemetry after_second = eng.telemetry();

  // Slot 0 played pattern 0 and slot 1 played pattern 1. Read at the END of each
  // bar, where the transport has already moved on to the next slot -- so after
  // bar one it names slot 1, and after bar two it has wrapped back to slot 0.
  CHECK(after_first.transport_slot == 1);
  CHECK(after_first.transport_pattern == 1);
  CHECK(after_second.transport_slot == 0);
  CHECK(after_second.transport_pattern == 0);

  // And the audio agrees: pad 1's sound in the first bar, pad 4's in the second.
  // Pad 1 is a 24-frame square and pad 4 a 6-frame one, so the two bars cannot be
  // confused for each other by amplitude alone.
  const std::vector<std::size_t> bar_one = onsets(first.channel(0));
  const std::vector<std::size_t> bar_two = onsets(second.channel(0));
  CHECK(bar_one.size() == 4);
  CHECK(bar_two.size() == 4);
  CHECK(first.hash() != second.hash());
}

TEST_CASE("e2e: the metronome is off by default and audible when it is not", "[e2e]") {
  // NO COMMITTED HASH here, and the reason is the one at the top of this file:
  // the click's gains are 0.50 and 0.32, and 0.32 is not a binary fraction, so
  // `amplitude * value` rounds and the accumulate becomes an a*b+c an FMA could
  // fuse differently. What is portable, and what matters, is that it is silent
  // until asked for and that turning it on changes the output.
  const std::shared_ptr<const rt::Sample> material = make_kit();
  const std::array<std::size_t, 1> blocks{512};

  auto quiet = std::make_shared<rt::SequencerState>();
  quiet->bpm_x100 = kBpmX100;
  quiet->patterns[0].length = kPatternSteps;

  // An EMPTY pattern, so whatever is heard is the click and nothing else.
  const Session without = run_session(blocks, quiet, material);
  CHECK(without.peak == 0.0F);

  auto ticking = std::make_shared<rt::SequencerState>(*quiet);
  ticking->metronome = true;
  const Session with = run_session(blocks, ticking, material);
  CHECK(with.peak > 0.0F);

  // Block-size invariant even though its bytes are not portable, which is the
  // half of determinism that can be asserted on this machine.
  const std::array<std::size_t, 3> mixed{128, 512, 333};
  CHECK(run_session(mixed, ticking, material).hash == with.hash);
}

TEST_CASE("e2e: a stopped transport plays nothing", "[e2e]") {
  // The other half of the transport, and not a formality: the sequencer runs
  // inside render(), so "stopped" has to mean the step scan is skipped rather
  // than that nobody is calling it. Nothing calls render() differently here.
  const std::array<std::size_t, 1> blocks{512};

  engine::Engine eng{e2e_config()};
  REQUIRE(assign_kit(eng, make_kit()) == rt::kNumPads);
  REQUIRE(eng.publish_sequencer(make_state(make_beat())));

  Render render{kRenderFrames, kChannels};
  render.run(eng, blocks);
  CHECK(render.peak() == 0.0F);

  const engine::Telemetry telemetry = eng.telemetry();
  CHECK_FALSE(telemetry.transport_playing);
  CHECK(telemetry.transport_frames == 0);
}

TEST_CASE("e2e: MIDI bytes reach the audio", "[e2e]") {
  // docs/ROADMAP.md's "MIDI integration test green", and it CANNOT SKIP: the
  // bytes are literals, decode_midi() is a pure function, and the ring is the
  // engine's own. No controller, no CoreMIDI, no ALSA. The half that does need
  // hardware is tests/unit/midi_device_test.cpp's [device] case, which skips
  // loudly; between them the path from a cable to a sound is covered on every
  // machine.
  //
  // submit_midi_event() is documented MIDI-THREAD-ONLY, and is called from this
  // thread deliberately: there is only one thread here, so there is no second
  // producer and no race to have. Concurrency on that ring is
  // tests/integration/engine_threading_test.cpp's subject; this is about what a
  // note MEANS by the time it is audible.
  const std::shared_ptr<const rt::Sample> material = make_kit();
  const std::array<std::size_t, 1> blocks{256};
  constexpr std::size_t kProbeFrames = 4'096;

  const auto play_note = [&](std::initializer_list<std::uint8_t> bytes) {
    const std::vector<std::uint8_t> message{bytes};
    const std::optional<rt::PadEvent> event = io::decode_midi(message, io::MidiMap{});
    REQUIRE(event.has_value());

    engine::Engine eng{e2e_config()};
    REQUIRE(assign_kit(eng, material) == rt::kNumPads);
    REQUIRE(eng.submit_midi_event(*event));

    Render render{kProbeFrames, kChannels};
    render.run(eng, blocks);
    REQUIRE(eng.dropped_midi_events() == 0);
    return render;
  };

  // Note 36 is the General MIDI bass drum and io::MidiMap's default base, so it
  // is pad 1 -- the note a plugged-in controller's first pad actually sends.
  const Render pad_one = play_note({0x90, 36, 127});
  CHECK(pad_one.peak() > 0.5F);

  // Note 39 is pad 4, whose sound is a 6-frame square where pad 1's is 24. Two
  // notes that both made A sound would pass; these two made DIFFERENT sounds.
  const Render pad_four = play_note({0x90, 39, 127});
  CHECK(pad_four.peak() > 0.5F);
  CHECK(pad_one.hash() != pad_four.hash());

  // Velocity arrives as level, which is the whole of "note-on/off with velocity".
  const Render soft = play_note({0x90, 36, 32});
  CHECK(soft.peak() > 0.0F);
  CHECK(soft.peak() < pad_one.peak() / 2.0F);

  // A note-on at velocity 0 is a NOTE-OFF, which is what a large share of
  // controllers send instead of 0x80. Read as a note-on it would start a voice;
  // here it must start nothing, and on a one-shot pad a note-off is ignored.
  const std::vector<std::uint8_t> release{0x90, 36, 0};
  const std::optional<rt::PadEvent> off = io::decode_midi(release, io::MidiMap{});
  REQUIRE(off.has_value());
  CHECK(off->kind == rt::PadEventKind::kNoteOff);

  engine::Engine eng{e2e_config()};
  REQUIRE(assign_kit(eng, material) == rt::kNumPads);
  REQUIRE(eng.submit_midi_event(*off));
  Render silence{kProbeFrames, kChannels};
  silence.run(eng, blocks);
  CHECK(silence.peak() == 0.0F);
}

TEST_CASE("e2e: a MIDI note and a sequenced step reach the same pad the same way", "[e2e]") {
  // One path for every producer, which is what start_voice() exists to be. A
  // note played at full velocity and a step written at velocity 127 have to
  // produce the SAME AUDIO -- if they did not, "record what you played" would
  // not be a thing M6 could build.
  const std::shared_ptr<const rt::Sample> material = make_kit();
  const std::array<std::size_t, 1> blocks{256};
  constexpr std::size_t kProbeFrames = 8'192;

  // A one-step pattern on pad 1, at step 0, so it fires at frame 0 -- the same
  // frame the MIDI note lands on, since an event with no offset is placed at the
  // top of the next block and nothing has rendered yet.
  rt::Pattern single;
  single.length = kPatternSteps;
  put(single, 0, {0});

  engine::Engine sequenced{e2e_config()};
  REQUIRE(assign_kit(sequenced, material) == rt::kNumPads);
  REQUIRE(sequenced.publish_sequencer(make_state(single)));
  REQUIRE(sequenced.send_transport(rt::TransportCommand{.kind = rt::TransportCommandKind::kPlay}));
  Render from_pattern{kProbeFrames, kChannels};
  from_pattern.run(sequenced, blocks);

  const std::vector<std::uint8_t> note{0x90, 36, 127};
  const std::optional<rt::PadEvent> event = io::decode_midi(note, io::MidiMap{});
  REQUIRE(event.has_value());

  engine::Engine live{e2e_config()};
  REQUIRE(assign_kit(live, material) == rt::kNumPads);
  REQUIRE(live.submit_midi_event(*event));
  Render from_midi{kProbeFrames, kChannels};
  from_midi.run(live, blocks);

  CHECK(from_pattern.peak() > 0.5F);
  CHECK(from_pattern.hash() == from_midi.hash());

  // They differ in exactly one thing, and it is a light rather than a sample:
  // the sequenced hit is marked as such so the pad grid can draw it differently.
  CHECK(sequenced.telemetry().pad_glow[0].sequenced);
  CHECK_FALSE(live.telemetry().pad_glow[0].sequenced);
}

TEST_CASE("e2e: the real loop sequences and plays", "[e2e][fixture]") {
  // The same path on real decoded material, at a rate that is not the engine's,
  // so the resampler and the interpolator are both in it.
  //
  // NO COMMITTED HASH, on purpose and for the reason stated at the top: this goes
  // through FFmpeg and libsamplerate, whose output is a property of the versions
  // installed rather than of this repository, and the phase step is not one frame
  // so the Hermite kernel is genuinely interpolating. What is still true, and
  // worth asserting, is that the session is repeatable and block-size invariant
  // on whatever machine it runs.
  const std::filesystem::path path =
      std::filesystem::path{CRATEDIG_STARTER_PACK_DIR} / "loop_industrial.flac";
  std::error_code ignored;
  if (!std::filesystem::exists(path, ignored)) {
    SKIP("starter pack not fetched -- run scripts/fetch_starter_pack.sh");
  }

  const ingest::SampleLoad load = ingest::load_sample(path, kRate);
  INFO("decode error: " << ingest::describe(load.error) << " (" << load.detail << ")");
  REQUIRE(load.ok());

  rt::Pattern beat = make_beat();
  beat.swing = 33;  // a shift that is not a whole number of frames per step
  const std::shared_ptr<const rt::SequencerState> state = make_state(beat);

  const std::array<std::size_t, 1> one_block{kMaxBlock};
  const std::array<std::size_t, 3> mixed{128, 512, 333};

  const Session first = run_session(one_block, state, load.sample);
  const Session second = run_session(one_block, state, load.sample);
  const Session chunked = run_session(mixed, state, load.sample);

  CHECK(first.published == rt::kNumPads);
  CHECK(first.peak > 0.0F);
  CHECK(first.hash == second.hash);
  CHECK(first.hash == chunked.hash);
}
