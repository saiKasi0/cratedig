// M5.5's acceptance: many records at once, rendered offline, bit for bit.
//
// docs/ROADMAP.md's M5.5 asks for three things, and they are proved in three
// places because they are three different kinds of claim:
//
//   "a pad plays a slice of one file while its neighbour plays a slice of
//    another, published live with no allocation on the audio thread"
//        -- the audio half is THIS FILE; the no-allocation half is
//           tests/integration/rt_safety_test.cpp, where the RT guard is.
//
//   "a slice with no pad can be auditioned from EDIT and from the browser"
//        -- tests/unit/audition_test.cpp for the engine's side of it, and the
//           PTY sessions for whether a person can actually get there.
//
//   "`:` completes both verbs and paths"
//        -- tests/unit/completion_test.cpp and pty_complete_session.py.
//
// WHAT ONLY THIS FILE CAN SEE
// ---------------------------
// Every other crate test works one file at a time or one pad at a time. The
// thing M5.5 changed is that sixteen pads no longer share one Sample, and the
// failure that removal invites is a pad quietly reading the WRONG file's audio
// -- a pointer confusion that produces sound, so it does not crash, and produces
// the same sound every run, so it is not flaky. It is a hash away from being
// invisible.
//
// WHAT MAY BE A COMMITTED HASH HERE, AND WHAT MAY ONLY BE INVARIANCE
// ------------------------------------------------------------------
// docs/TESTING.md: a constant is allowed only when the path provably contains no
// `a*b+c` that one target fuses into an FMA and another does not. The audit is
// the one mixer_e2e_test.cpp already did and this file inherits by using the
// same conditions:
//
//   - the fixtures are at the engine's rate with pitch_ratio 1.0, so the phase
//     fraction is exactly zero and hermite4() collapses to an exact copy;
//   - the pads keep the default flat-sustain envelope, so `Envelope::level()`
//     never evaluates its `m_start + m_step * position` ramp;
//   - the strips are left at unity, so the mixer contributes only `*=` and `+=`,
//     both exactly specified by IEEE-754 with nothing to fuse.
//
// The live-republication case below asserts run-to-run equality instead, and
// says why: adoption happens at a block boundary, so WHEN a republished config
// takes effect is a function of the block size by construction. That is a
// property of the handoff, not a bug, and a block-size-invariance assertion over
// it would be asserting something false.

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
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint32_t kRate = 48'000;
constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kMaxBlock = 2'048;
constexpr std::size_t kRenderFrames = 12'000;
constexpr std::size_t kFileFrames = 24'000;

// Four pads from each of two files. Eight rather than sixteen so the render
// stays quick, and an even split so "every pad came from file A" and "every pad
// came from file B" are both wrong by the same amount.
constexpr std::size_t kPadsPerFile = 4;
constexpr std::size_t kUsedPads = 2 * kPadsPerFile;

[[nodiscard]] engine::Engine::Config crate_config() {
  return engine::Engine::Config{
      .sample_rate = kRate, .num_channels = kChannels, .max_block_frames = kMaxBlock, .seed = 0};
}

// A file whose content is a function of `voice`, so the two are not two copies.
//
// Integers over 32768, a power of two, so every value is exact in a float and
// identical on every machine by construction -- no libm, no <random>, nothing
// whose last bit is a property of the host. The rule the other e2e fixtures
// follow.
//
// `voice` changes the square-wave period AND the amplitude AND the channel
// relationship. One difference would be enough to make the files distinguishable
// in principle; three make it very hard for a mix-up to cancel out into a hash
// that happens to match.
[[nodiscard]] std::shared_ptr<const rt::Sample> make_file(int voice) {
  auto sample = std::make_shared<rt::Sample>(kRate, kChannels, kFileFrames);
  std::span<float> left = sample->mutable_channel(0);
  std::span<float> right = sample->mutable_channel(1);

  for (std::size_t frame = 0; frame < kFileFrames; ++frame) {
    const std::size_t region = frame / (kFileFrames / kPadsPerFile);
    const std::size_t period = (voice == 0 ? 24 : 13) - (2 * std::min(region, std::size_t{3}));
    const bool high = (frame / period) % 2 == 0;

    const int amplitude = voice == 0 ? 24'000 : 18'000;
    const int square = high ? amplitude : -amplitude;

    // The second file's right channel leads rather than mirrors, so a bug that
    // took the left channel of one file and the right of the other would not
    // land on a plausible-looking stereo image.
    const int other = voice == 0 ? (high ? -12'000 : 12'000) : (high ? 6'000 : -6'000);

    left[frame] = static_cast<float>(square) / 32'768.0F;
    right[frame] = static_cast<float>(other) / 32'768.0F;
  }
  return sample;
}

// Captures a whole render, channel-major, so it can be hashed and compared.
// The harness the other e2e tests established.
class Capture {
 public:
  Capture() : m_data(kRenderFrames * kChannels, 0.0F) {}

  void render(engine::Engine& engine, std::span<const std::size_t> block_sizes) {
    std::vector<float> scratch(static_cast<std::size_t>(kMaxBlock) * kChannels, 0.0F);
    std::vector<float*> channels(kChannels);

    std::size_t done = 0;
    std::size_t next = 0;
    while (done < kRenderFrames) {
      const std::size_t block =
          std::min(block_sizes[next % block_sizes.size()], kRenderFrames - done);
      ++next;
      if (block == 0) {
        continue;
      }
      // Dirtied every block: if render() ever fails to write a sample the test
      // must see the garbage rather than a leftover zero that hides it.
      std::fill(scratch.begin(), scratch.end(), -7.5F);
      for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
        channels[channel] = scratch.data() + (static_cast<std::size_t>(channel) * kMaxBlock);
      }

      engine.render(std::span<float* const>{channels}, block);

      for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
        std::copy_n(channels[channel], block,
                    m_data.data() + (static_cast<std::size_t>(channel) * kRenderFrames) + done);
      }
      done += block;
    }
  }

  [[nodiscard]] std::span<const float> samples() const { return m_data; }

  [[nodiscard]] float peak() const {
    float peak = 0.0F;
    for (const float value : m_data) {
      peak = std::max(peak, value < 0.0F ? -value : value);
    }
    return peak;
  }

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

  [[nodiscard]] bool identical(const Capture& other) const {
    return m_data.size() == other.m_data.size() &&
           std::memcmp(m_data.data(), other.m_data.data(), m_data.size() * sizeof(float)) == 0;
  }

 private:
  std::vector<float> m_data;
};

// Pads 1-4 from the first file, pads 5-8 from the second. THE ARRANGEMENT M5.5
// EXISTS TO MAKE POSSIBLE -- before it, sixteen pads shared one Sample and this
// function could not be written.
void load_pads(engine::Engine& engine, const std::shared_ptr<const rt::Sample>& first,
               const std::shared_ptr<const rt::Sample>& second) {
  for (std::size_t pad = 0; pad < kUsedPads; ++pad) {
    const bool from_first = pad < kPadsPerFile;
    const std::size_t slice = pad % kPadsPerFile;

    rt::PadConfig config;
    config.sample = from_first ? first : second;
    config.pad = static_cast<std::uint8_t>(pad);
    config.start_frame = slice * (kFileFrames / kPadsPerFile);
    config.end_frame = (slice + 1) * (kFileFrames / kPadsPerFile);
    REQUIRE(engine.publish_pad_config(std::make_shared<const rt::PadConfig>(config)));
  }
}

// Every pad at frame_offset 0.
//
// NOT staggered, and that is the M5 T9 lesson rather than a simplification:
// `frame_offset` is BLOCK-relative and clamped to `num_frames - 1`, so a trigger
// asked for at offset 700 lands at 700 in a 2048-frame block and at 255 in a
// 256-frame one. Staggering would break block-size invariance by design and the
// test would be reporting the clamp rather than the engine.
void trigger_all(engine::Engine& engine) {
  for (std::size_t pad = 0; pad < kUsedPads; ++pad) {
    REQUIRE(engine.trigger_pad(rt::PadEvent{.pad = static_cast<std::uint8_t>(pad),
                                            .kind = rt::PadEventKind::kNoteOn,
                                            .velocity = 1.0F}));
  }
}

[[nodiscard]] std::uint64_t render_crate(std::span<const std::size_t> block_sizes,
                                         Capture& capture) {
  engine::Engine engine{crate_config()};
  load_pads(engine, make_file(0), make_file(1));
  trigger_all(engine);
  capture.render(engine, block_sizes);
  return capture.hash();
}

}  // namespace

TEST_CASE("e2e: pads from two files render bit-exact", "[e2e]") {
  // THE ACCEPTANCE. Four pads playing slices of one record and four playing
  // slices of another, summed through the mixer at unity.
  //
  // Verified on AppleClang/arm64 and clang-18/x86-64 before being committed, per
  // docs/TESTING.md -- a hash argued to be portable and never run on a second
  // target is a hash nobody has checked.
  constexpr std::uint64_t kExpected = 0xF93E4B76E97ADD51ULL;
  static constexpr std::array<std::size_t, 1> kBlocks{512};

  Capture capture;
  const std::uint64_t hash = render_crate(kBlocks, capture);

  // Sounding at all, before anything about which bits. A hash over silence is
  // stable, portable and says nothing.
  REQUIRE(capture.peak() > 0.1F);

  INFO("crate golden: 0x" << std::hex << hash);
  CHECK(hash == kExpected);
}

TEST_CASE("e2e: a two-file render is invariant to block size", "[e2e]") {
  // The detector for cross-block state bugs, and the one that matters most here:
  // eight voices reading four different regions of two different Samples, all
  // advancing independently. A voice that carried a frame index in the wrong
  // Sample's terms would drift differently under different block sizes.
  static constexpr std::array<std::size_t, 1> kSteady{512};
  static constexpr std::array<std::size_t, 5> kRagged{64, 2'048, 1, 333, 977};

  Capture steady;
  Capture ragged;
  static_cast<void>(render_crate(kSteady, steady));
  static_cast<void>(render_crate(kRagged, ragged));

  CHECK(steady.identical(ragged));
}

TEST_CASE("e2e: a two-file render is the same twice", "[e2e]") {
  static constexpr std::array<std::size_t, 3> kBlocks{256, 1'024, 128};

  Capture first;
  Capture second;
  const std::uint64_t one = render_crate(kBlocks, first);
  const std::uint64_t two = render_crate(kBlocks, second);

  CHECK(one == two);
  CHECK(first.identical(second));
}

TEST_CASE("e2e: two files really are two different sounds", "[e2e]") {
  // THE CONTROL THAT MAKES THE GOLDEN MEAN SOMETHING. If both files were the
  // same audio, every assertion above would hold just as well with the pool
  // handing every pad the same Sample -- which is the exact bug this milestone
  // could introduce and the exact bug a hash cannot describe.
  //
  // So: render with the pads split across two files, then again with all eight
  // reading the FIRST file, and require the two to differ.
  static constexpr std::array<std::size_t, 1> kBlocks{512};

  Capture split;
  static_cast<void>(render_crate(kBlocks, split));

  Capture single;
  {
    engine::Engine engine{crate_config()};
    const std::shared_ptr<const rt::Sample> only = make_file(0);
    load_pads(engine, only, only);
    trigger_all(engine);
    single.render(engine, kBlocks);
  }

  CHECK_FALSE(split.identical(single));
}

TEST_CASE("e2e: a pad repointed at another file mid-render stays deterministic", "[e2e]") {
  // "PUBLISHED LIVE", the half of the acceptance the static cases cannot reach:
  // a pad swapped from one record to another while the stream is running.
  //
  // RUN-TO-RUN EQUALITY RATHER THAN BLOCK-SIZE INVARIANCE, and the distinction
  // is real. A published config is adopted at a block boundary, so WHEN it takes
  // effect is a function of the block size by construction -- asserting
  // invariance over it would be asserting something false about the handoff
  // rather than something true about the engine.
  static constexpr std::array<std::size_t, 1> kBlocks{512};

  const auto run = []() {
    engine::Engine engine{crate_config()};
    const std::shared_ptr<const rt::Sample> first = make_file(0);
    const std::shared_ptr<const rt::Sample> second = make_file(1);
    load_pads(engine, first, second);
    trigger_all(engine);

    std::vector<float> scratch(static_cast<std::size_t>(kMaxBlock) * kChannels, 0.0F);
    std::vector<float*> channels(kChannels);
    std::vector<float> data(kRenderFrames * kChannels, 0.0F);

    std::size_t done = 0;
    bool swapped = false;
    while (done < kRenderFrames) {
      const std::size_t block = std::min(kBlocks[0], kRenderFrames - done);

      // Halfway through, pad 1 is repointed at the second file and retriggered.
      // At a block boundary, which is where the interface publishes from too.
      //
      // `>=` AND A FLAG, not `== kRenderFrames / 2`. Twelve thousand frames in
      // blocks of 512 never lands on 6000, so the first version of this swapped
      // nothing at all -- and every assertion about determinism passed, because
      // two renders that both do nothing agree perfectly. The control below is
      // what found it.
      if (!swapped && done >= kRenderFrames / 2) {
        swapped = true;
        rt::PadConfig moved;
        moved.sample = second;
        moved.pad = 0;
        moved.start_frame = 0;
        moved.end_frame = kFileFrames / kPadsPerFile;
        REQUIRE(engine.publish_pad_config(std::make_shared<const rt::PadConfig>(moved)));
        REQUIRE(engine.trigger_pad(
            rt::PadEvent{.pad = 0, .kind = rt::PadEventKind::kNoteOn, .velocity = 1.0F}));
      }

      std::fill(scratch.begin(), scratch.end(), -7.5F);
      for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
        channels[channel] = scratch.data() + (static_cast<std::size_t>(channel) * kMaxBlock);
      }
      engine.render(std::span<float* const>{channels}, block);
      for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
        std::copy_n(channels[channel], block,
                    data.data() + (static_cast<std::size_t>(channel) * kRenderFrames) + done);
      }
      done += block;
    }
    return data;
  };

  const std::vector<float> first = run();
  const std::vector<float> second = run();

  REQUIRE(first.size() == second.size());
  CHECK(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)) == 0);

  // AND IT ACTUALLY CHANGED THE SOUND. Two identical runs of a republication
  // that never arrived would agree just as well, so the case has to show the
  // republication did something -- against the same render with no swap in it.
  Capture untouched;
  static_cast<void>(render_crate(kBlocks, untouched));
  REQUIRE(untouched.samples().size() == first.size());
  CHECK(std::memcmp(first.data(), untouched.samples().data(), first.size() * sizeof(float)) != 0);
}
