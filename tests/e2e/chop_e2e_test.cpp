// The M3 acceptance, offline and end to end: import -> chop transient -> play the chops.
//
// docs/ROADMAP.md states it as one sentence -- "import, `:chop transient`, play
// chops end-to-end e2e script passes bit-exact" -- and it is tested in two
// layers, matching the split docs/TESTING.md already draws. This file is the
// offline half: it runs the whole pipeline through the real ingest and engine
// code with no terminal, no device and no file, and checks the audio that comes
// out. tests/e2e/pty_chop_session.py is the other half, and types the command
// into the real binary.
//
// WHY THE MATERIAL IS SYNTHESISED HERE
// ------------------------------------
// The acceptance test for a milestone must not be able to skip. A fresh clone
// has no starter pack until scripts/fetch_starter_pack.sh runs, so anything
// built on the fixtures reports success by not running -- which is the one
// outcome an acceptance test may never have. The loop below is built from
// integer arithmetic in this file, so it exists on every machine and is the same
// bytes on all of them. The real material is covered by the [fixture] case at
// the bottom, which is allowed to skip because it is an addition rather than the
// acceptance itself.
//
// WHY THE GOLDEN HASH IS PORTABLE, WHICH IS NOT AN ACCIDENT
// ---------------------------------------------------------
// The committed hash below was checked on AppleClang/arm64 and on clang-18/x86-64
// in the Docker CI image and is the same value on both. It is worth saying why,
// because a hash of real audio usually is NOT portable -- an a*b+c that one
// target fuses into an FMA and another does not differs in the last bit, and a
// golden hash notices. The signal path taken here has no such expression in it,
// which is a property of THIS configuration rather than of the engine:
//
//   - The sample is 48 kHz and the engine is 48 kHz and pitch_ratio is 1.0, so
//     step_for() returns exactly kPhaseOne, the phase fraction is exactly 0 at
//     every frame, and hermite4(a, b, c, d, 0.0f) collapses to exactly b -- each
//     multiply by zero is exact, so it stays exact whether or not the multiplies
//     and adds are fused.
//   - The pads carry the default envelope, which is a flat sustain: level()
//     returns m_spec.sustain directly and never evaluates its
//     `m_start + m_step * position` ramp, which is the one expression in the
//     voice path an FMA would change the low bit of.
//
// Both are what `:chop transient` actually produces (see apply_slices() in
// src/tui/app.cpp -- it sets a slice range and nothing else), so this is the real
// path rather than a contrived one. A future test that retunes a pad or gives it
// an attack is in the other case and should assert invariance rather than a
// committed constant; docs/TESTING.md records the rule.

#include "engine/engine.hpp"
#include "ingest/decoder.hpp"
#include "ingest/slices.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
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

// The constructed loop: eight hits, 0.25 s apart, after 0.1 s of silence.
constexpr std::size_t kLeadFrames = 4'800;
constexpr std::size_t kHitSpacing = 12'000;
constexpr std::size_t kNumHits = 8;
constexpr std::size_t kLoopFrames = kLeadFrames + (kHitSpacing * kNumHits);

// Where the hits actually are, by construction. The onset detector has to find
// these; nothing here is derived from what it reports, which is what stops the
// assertion being circular.
[[nodiscard]] std::size_t hit_frame(std::size_t hit) noexcept {
  return kLeadFrames + (hit * kHitSpacing);
}

// A percussive loop, built rather than decoded.
//
// NO libm ANYWHERE IN HERE, deliberately. std::sin is not required to give the
// same last bit on two platforms, so a golden hash over material generated with
// it would be a golden hash over the host's libm. Everything below is integer
// arithmetic divided by 32768 -- a power of two, so every value is exact in a
// float and identical on every machine by construction. Same reasoning as
// write_fixture_wav() in tests/tui/pty_session.py.
//
// Each hit is a noise attack followed by a square-wave body at its own period,
// under a linear decay. The noise is what makes the spectral flux detector fire;
// the per-hit period is what makes the eight slices distinguishable from each
// other, which is what "pad 3 plays slice 3" needs in order to mean anything.
[[nodiscard]] std::shared_ptr<const rt::Sample> make_loop() {
  auto sample = std::make_shared<rt::Sample>(kRate, kChannels, kLoopFrames);

  constexpr std::size_t kAttackFrames = 96;   // 2 ms of noise
  constexpr std::size_t kBodyFrames = 8'000;  // ~167 ms, well short of the spacing
  constexpr int kPeakAmplitude = 30'000;      // of 32768

  std::span<float> left = sample->mutable_channel(0);
  std::span<float> right = sample->mutable_channel(1);

  // A plain LCG rather than <random>: the values must not depend on a standard
  // library's choice of engine or distribution implementation.
  std::uint32_t noise = 0x5EED'1234U;
  const auto next_noise = [&noise]() {
    noise = (noise * 1'664'525U) + 1'013'904'223U;
    return noise;
  };

  for (std::size_t hit = 0; hit < kNumHits; ++hit) {
    const std::size_t start = hit_frame(hit);
    // Periods from 20 down to 6 frames: 2.4 kHz up to 8 kHz, so consecutive
    // slices differ audibly and the flux at each boundary is a real spectral
    // change rather than only a level change.
    const std::size_t period = 20 - (hit * 2);

    for (std::size_t offset = 0; offset < kBodyFrames; ++offset) {
      const std::size_t frame = start + offset;
      if (frame >= kLoopFrames) {
        break;
      }

      // Linear decay to silence over the body, in integer units.
      const auto decay = static_cast<int>(
          (static_cast<std::size_t>(kPeakAmplitude) * (kBodyFrames - offset)) / kBodyFrames);

      int value = 0;
      if (offset < kAttackFrames) {
        // Full-scale noise, sign only: the spectrum is what matters, and a
        // one-bit source of it is exactly representable.
        value = (next_noise() & 0x8000'0000U) != 0 ? decay : -decay;
      } else {
        value = ((offset / period) % 2 == 0) ? decay : -decay;
      }

      // 1/32768 exactly, so the float holds the integer with no rounding.
      const float scaled = static_cast<float>(value) / 32'768.0F;
      left[frame] = scaled;
      // The right channel is the same signal at half amplitude -- still exact,
      // and different enough that a bug collapsing the two channels into one
      // shows up in the hash rather than hiding behind identical planes.
      right[frame] = scaled * 0.5F;
    }
  }

  return sample;
}

// One scheduled hit in the played pattern.
struct Trigger {
  std::size_t frame = 0;
  std::uint8_t pad = 0;
  float velocity = 1.0F;
};

// The pattern the chopped pads play. Deliberately not one pad after another:
// two pads overlap at 36000 and again at 60000, so the mix path and the voice
// pool are both carrying more than one voice when the hash is taken.
//
// Velocities are exact binary fractions so the gain multiply introduces no
// rounding of its own -- the point of the hash is the engine's arithmetic, and a
// velocity of 0.7f would only add the host's decimal-to-float conversion, which
// is at least standard but is not the thing under test.
[[nodiscard]] std::vector<Trigger> pattern() {
  return {
      Trigger{.frame = 0, .pad = 0, .velocity = 1.0F},
      Trigger{.frame = 6'000, .pad = 2, .velocity = 0.5F},
      Trigger{.frame = 12'000, .pad = 4, .velocity = 0.75F},
      Trigger{.frame = 18'000, .pad = 0, .velocity = 0.25F},
      Trigger{.frame = 24'000, .pad = 7, .velocity = 1.0F},
      Trigger{.frame = 30'000, .pad = 1, .velocity = 0.5F},
      Trigger{.frame = 36'000, .pad = 3, .velocity = 1.0F},
      Trigger{.frame = 36'000, .pad = 5, .velocity = 0.5F},
      Trigger{.frame = 48'000, .pad = 6, .velocity = 0.75F},
      Trigger{.frame = 60'000, .pad = 0, .velocity = 1.0F},
      Trigger{.frame = 60'000, .pad = 4, .velocity = 0.25F},
  };
}

constexpr std::size_t kPatternFrames = 72'000;  // 1.5 s, past the end of every hit

// Assigns one slice per pad, exactly as `:chop transient` does.
//
// This mirrors apply_slices() in src/tui/app.cpp rather than calling it: that
// function is in an anonymous namespace inside the binary's own translation
// unit, and exporting it so a test could reach it would be changing the program
// to suit the test. What it does is the documented publish_pad_config()
// protocol, which is what is exercised here; the PTY session covers app.cpp's
// copy by driving the real command.
[[nodiscard]] std::size_t assign_slices(engine::Engine& eng,
                                        const std::shared_ptr<const rt::Sample>& sample,
                                        const ingest::SliceSet& set) {
  std::size_t published = 0;
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    const bool has_slice = pad < set.size();

    rt::PadConfig config{};
    config.pad = static_cast<std::uint8_t>(pad);
    if (has_slice) {
      config.sample = sample;
      config.start_frame = set.slices[pad].start_frame;
      config.end_frame = set.slices[pad].end_frame;
    }

    if (!eng.publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(config)))) {
      return published;
    }
    ++published;
  }
  return published;
}

// Renders a pattern and keeps every sample, channel-major.
//
// The block sizes cycle WITHIN each gap between triggers, and every trigger is
// pushed at exactly its scheduled frame. That is what makes block-size
// invariance a real assertion here: the engine drains its event ring at the top
// of a block, so a trigger pushed at an arbitrary moment would land at a
// different frame under a different block size and the outputs would differ for
// a reason that has nothing to do with the engine.
class SessionRender {
 public:
  SessionRender(std::size_t total_frames, std::uint16_t channels)
      : m_channels(channels), m_total_frames(total_frames), m_data(total_frames * channels, 0.0F) {}

  void play(engine::Engine& eng, std::span<const Trigger> triggers,
            std::span<const std::size_t> block_sizes) {
    std::size_t done = 0;
    std::size_t next_size = 0;

    for (const Trigger& trigger : triggers) {
      render_until(eng, std::min(trigger.frame, m_total_frames), done, next_size, block_sizes);
      REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = trigger.pad,
                                           .kind = rt::PadEventKind::kNoteOn,
                                           .velocity = trigger.velocity,
                                           .frame_offset = 0}));
    }
    render_until(eng, m_total_frames, done, next_size, block_sizes);
  }

  [[nodiscard]] std::span<const float> samples() const { return m_data; }

  [[nodiscard]] float peak() const {
    float loudest = 0.0F;
    for (const float value : m_data) {
      loudest = std::max(loudest, value < 0.0F ? -value : value);
    }
    return loudest;
  }

  // FNV-1a over the raw bytes, the same harness tests/unit/engine_render_test.cpp
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
  void render_until(engine::Engine& eng, std::size_t target, std::size_t& done,
                    std::size_t& next_size, std::span<const std::size_t> block_sizes) {
    std::vector<float> scratch(static_cast<std::size_t>(kMaxBlock) * m_channels, 0.0F);
    std::vector<float*> channel_pointers(m_channels);

    while (done < target) {
      const std::size_t block =
          std::min(block_sizes[next_size % block_sizes.size()], target - done);
      next_size++;
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

  std::uint16_t m_channels;
  std::size_t m_total_frames;
  std::vector<float> m_data;
};

// The whole acceptance path in one call, so each test below states only what it
// is asserting about the result.
struct Session {
  std::shared_ptr<const rt::Sample> sample;
  ingest::SliceSet slices;
  std::uint64_t hash = 0;
  float peak = 0.0F;
  std::uint64_t dropped_events = 0;
  std::uint64_t dropped_triggers = 0;
  std::uint64_t rejected_configs = 0;
  std::uint64_t garbage_overflows = 0;
  std::size_t published = 0;
};

[[nodiscard]] Session run_session(std::span<const std::size_t> block_sizes,
                                  const std::shared_ptr<const rt::Sample>& material) {
  Session session;
  session.sample = material;
  session.slices = ingest::chop_transient(*session.sample);

  engine::Engine eng{e2e_config()};
  session.published = assign_slices(eng, session.sample, session.slices);

  const std::vector<Trigger> triggers = pattern();
  SessionRender render{kPatternFrames, kChannels};
  render.play(eng, triggers, block_sizes);

  session.hash = render.hash();
  session.peak = render.peak();
  session.dropped_events = eng.dropped_events();
  session.dropped_triggers = eng.dropped_triggers();
  session.rejected_configs = eng.rejected_pad_configs();
  session.garbage_overflows = eng.garbage_overflows();
  return session;
}

}  // namespace

TEST_CASE("e2e: import, chop transient, and play the chops", "[e2e]") {
  const std::array<std::size_t, 1> blocks{512};
  const Session session = run_session(blocks, make_loop());

  // The chop found the hits that were built into the material. Onset accuracy
  // itself is tests/unit/onset_accuracy_test.cpp's subject; what this asserts is
  // that the pipeline is connected -- a chop that returned one slice covering
  // everything (detect_onsets' documented empty-result fallback) would produce
  // perfectly plausible audio and pass every other assertion in this file.
  REQUIRE(session.slices.size() == kNumHits);
  for (std::size_t hit = 0; hit < kNumHits; ++hit) {
    const std::size_t start = session.slices.slices[hit].start_frame;
    const std::size_t expected = hit_frame(hit);

    // 10 ms either side. The detector locates an onset to a hop and then
    // backtracks to the preceding energy minimum, so it lands near the attack
    // rather than exactly on it -- and the zero-crossing snap then moves it by
    // up to another 64 frames.
    const std::size_t tolerance = kRate / 100;
    INFO("slice " << hit << " starts at " << start << ", hit is at " << expected);
    CHECK(start + tolerance > expected);
    CHECK(start < expected + tolerance);
  }

  // Sixteen pads published: eight with slices, eight cleared. Nothing refused.
  CHECK(session.published == rt::kNumPads);
  CHECK(session.rejected_configs == 0);

  // Every trigger arrived and every one of them started a voice.
  CHECK(session.dropped_events == 0);
  CHECK(session.dropped_triggers == 0);
  CHECK(session.garbage_overflows == 0);

  // It made a sound. Trivial to state and the thing every other assertion here
  // would happily hold for if it did not: a bit-exact hash of silence is still
  // bit-exact.
  CHECK(session.peak > 0.5F);

  // THE COMMITTED GOLDEN. This is docs/ROADMAP.md's "passes bit-exact" for M3.
  // It changes only when the audible behaviour of chopping and triggering
  // changes, and such a change is explained in the commit that makes it -- never
  // re-baselined to make this line pass (CLAUDE.md, "Never fix a golden file").
  CHECK(session.hash == 0x3B0E7A491A4BF085ULL);
}

TEST_CASE("e2e: the chop session is invariant to block size", "[e2e]") {
  // The property the whole determinism contract rests on: state must not leak
  // across block boundaries. An engine that plays the same pattern differently
  // at 64 frames than at 2048 is not deterministic however repeatable each of
  // those runs is on its own.
  const std::shared_ptr<const rt::Sample> material = make_loop();

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

  const std::uint64_t reference = run_session(one_block, material).hash;
  CHECK(run_session(small, material).hash == reference);
  CHECK(run_session(mixed, material).hash == reference);
  CHECK(run_session(ragged, material).hash == reference);
}

TEST_CASE("e2e: the same session renders the same bytes every run", "[e2e]") {
  const std::shared_ptr<const rt::Sample> material = make_loop();
  const std::array<std::size_t, 3> blocks{256, 128, 700};

  CHECK(run_session(blocks, material).hash == run_session(blocks, material).hash);
}

TEST_CASE("e2e: each pad plays its own chop", "[e2e]") {
  // The assertion that makes this a CHOP test rather than a playback test. Every
  // other check here would pass just as well if all sixteen pads played the top
  // of the file, which is precisely the bug the slice range exists to prevent
  // and precisely what a hash cannot tell you it has.
  const std::shared_ptr<const rt::Sample> material = make_loop();
  const ingest::SliceSet set = ingest::chop_transient(*material);
  REQUIRE(set.size() == kNumHits);

  for (std::size_t pad = 0; pad < kNumHits; ++pad) {
    engine::Engine eng{e2e_config()};
    REQUIRE(assign_slices(eng, material, set) == rt::kNumPads);

    const std::array<Trigger, 1> single{
        Trigger{.frame = 0, .pad = static_cast<std::uint8_t>(pad), .velocity = 1.0F}};
    const std::array<std::size_t, 1> blocks{256};

    constexpr std::size_t kProbeFrames = 2'048;
    SessionRender render{kProbeFrames, kChannels};
    render.play(eng, single, blocks);

    const std::span<const float> out = render.samples();
    const std::span<const float> source = material->channel(0);
    const std::size_t start = set.slices[pad].start_frame;

    // Past the declick fade-in, where the output is the source unaltered:
    // velocity is 1.0, gain is 1.0, the envelope is a flat sustain and the phase
    // step is exactly one frame, so these are equal to the BIT rather than
    // approximately.
    std::size_t compared = 0;
    for (std::size_t frame = rt::kDefaultFadeFrames; frame < kProbeFrames; ++frame) {
      if (start + frame >= material->num_frames()) {
        break;
      }
      INFO("pad " << pad << ", frame " << frame << " of slice starting " << start);
      REQUIRE(out[frame] == source[start + frame]);
      ++compared;
    }
    CHECK(compared > 1'000);
  }
}

TEST_CASE("e2e: the real loop chops and plays", "[e2e][fixture]") {
  // The same path on real decoded material: a CC0 loop at a rate that is not the
  // engine's, so the resampler and the interpolator are both in it.
  //
  // NO COMMITTED HASH, on purpose. This one goes through FFmpeg and
  // libsamplerate, whose output is a property of the versions installed rather
  // than of this repository, and the phase step is not one frame so the Hermite
  // kernel is genuinely interpolating -- the fused-multiply-add note at the top
  // of this file applies. What is still true, and worth asserting, is that the
  // session is repeatable and block-size invariant on whatever machine it runs.
  const std::filesystem::path path =
      std::filesystem::path{CRATEDIG_STARTER_PACK_DIR} / "loop_industrial.flac";
  std::error_code ignored;
  if (!std::filesystem::exists(path, ignored)) {
    SKIP("starter pack not fetched -- run scripts/fetch_starter_pack.sh");
  }

  const ingest::SampleLoad load = ingest::load_sample(path, kRate);
  INFO("decode error: " << ingest::describe(load.error) << " (" << load.detail << ")");
  REQUIRE(load.ok());

  const std::array<std::size_t, 1> one_block{kMaxBlock};
  const std::array<std::size_t, 3> mixed{128, 512, 333};

  const Session first = run_session(one_block, load.sample);
  const Session second = run_session(one_block, load.sample);
  const Session chunked = run_session(mixed, load.sample);

  // A real drum loop has hits in it, and chop_transient falls back to one slice
  // covering everything when it finds none -- so this is the check that the
  // detector did something on real material rather than giving up.
  CHECK(first.slices.size() > 1);
  CHECK(first.published == rt::kNumPads);
  CHECK(first.peak > 0.0F);

  CHECK(first.hash == second.hash);
  CHECK(first.hash == chunked.hash);
}
