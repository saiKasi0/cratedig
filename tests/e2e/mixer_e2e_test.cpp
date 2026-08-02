// M5's acceptance: the mixer, rendered offline, bit for bit.
//
// docs/ROADMAP.md's M5 asks for three things. The EQ's +-0.1 dB against the
// analytic response is proved in tests/unit/biquad_test.cpp, where the filter
// is. This file is the other two: that a mixed render is reproducible, and that
// it does not depend on the block size the device happened to negotiate.
//
// WHAT MAY BE A COMMITTED HASH HERE, AND WHAT MAY ONLY BE INVARIANCE
// ------------------------------------------------------------------
// docs/TESTING.md: a constant is allowed only when the path provably contains no
// `a*b+c` that one target fuses into an FMA and another does not, and the test
// must say which ones it avoided. Audited rather than assumed, by reading the
// three places the mixer does arithmetic:
//
//   src/engine/engine.cpp  apply_gain     `samples[frame] *= gain`      multiply
//   src/engine/engine.cpp  apply_balance  `channels[c][f] *= left`      multiply
//   src/engine/engine.cpp  mix_into       `out[frame] += in[frame]`     add
//
// A lone multiply and a lone add are exactly specified by IEEE-754 and identical
// on every target; there is nothing for an FMA to fuse. So the WHOLE LEVEL AND
// ROUTING HALF of the mixer -- strip gain, balance, mute, solo, bus routing, bus
// gain -- is inside the rule, and the golden below exercises all of it rather
// than settling for a transparent chain.
//
// The three processors are outside it, each with a fused expression in its inner
// loop:
//
//   rt/biquad.hpp      `(b0*x) + (b1*x1) + (b2*x2) - ...`
//   rt/compressor.hpp  `(coeff * (env - detector)) + detector`
//   rt/limiter.hpp     `(release_coeff * (gain - target)) + target`
//
// So an engaged EQ, compressor or limiter asserts run-to-run equality and
// block-size invariance instead, which are real properties that need no
// constant to express. Those tests are below the golden and say so.
//
// The voice path itself qualifies for the two reasons the M3 e2e already
// established and this file reuses deliberately: the fixture is at the engine's
// rate with pitch_ratio 1.0, so the phase fraction is exactly zero and
// hermite4() collapses to an exact copy; and the pads keep the default
// flat-sustain envelope, so `Envelope::level()` never evaluates its ramp.

#include "engine/engine.hpp"
#include "rt/compressor.hpp"
#include "rt/eq.hpp"
#include "rt/limiter.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "rt/strip.hpp"

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
constexpr std::size_t kLoopFrames = 24'000;
constexpr std::size_t kUsedPads = 8;

engine::Engine::Config mixer_config() {
  return engine::Engine::Config{
      .sample_rate = kRate, .num_channels = kChannels, .max_block_frames = kMaxBlock, .seed = 0};
}

// Material built from integers divided by 32768.
//
// A power of two, so every value is exact in a float and identical on every
// machine by construction -- no libm, no <random>, nothing whose last bit is a
// property of the host. The same rule make_loop() follows in chop_e2e_test.cpp
// and write_fixture_wav() follows in the PTY sessions.
//
// The two channels differ, which matters here more than anywhere else: balance
// multiplies them by different numbers, and a mixer bug that swapped or
// duplicated a channel would be invisible against identical planes.
[[nodiscard]] std::shared_ptr<const rt::Sample> make_loop() {
  auto sample = std::make_shared<rt::Sample>(kRate, kChannels, kLoopFrames);
  std::span<float> left = sample->mutable_channel(0);
  std::span<float> right = sample->mutable_channel(1);

  std::uint32_t noise = 0x5EED'1234U;
  const auto next_noise = [&noise]() {
    noise = (noise * 1'664'525U) + 1'013'904'223U;
    return noise;
  };

  for (std::size_t frame = 0; frame < kLoopFrames; ++frame) {
    // Eight regions with their own square-wave period, so the eight slices are
    // distinguishable from each other rather than eight copies of one tone.
    const std::size_t region = frame / (kLoopFrames / kUsedPads);
    const std::size_t period = 20 - (2 * std::min(region, std::size_t{7}));
    const bool high = (frame / period) % 2 == 0;
    const int square = high ? 26'000 : -26'000;

    // The right channel is the inverse at half amplitude, written as its own
    // constant rather than as `-square / 2`. That is an integer division feeding
    // a float conversion -- exact here, since 26000 is even, and exactly the
    // kind of thing that stops being exact the day somebody changes the
    // amplitude to an odd number. Two constants say the same thing with nothing
    // left to check.
    const int inverse_half = high ? -13'000 : 13'000;

    // A little noise on top so the two channels are not scaled copies of one
    // another -- a balance bug that applied the left gain to both would survive
    // a test whose channels only differed by a constant.
    const auto grit = static_cast<int>(next_noise() >> 26U) - 32;
    left[frame] = static_cast<float>(square + grit) / 32'768.0F;
    right[frame] = static_cast<float>(inverse_half + grit) / 32'768.0F;
  }
  return sample;
}

// Captures a whole render, channel-major, so it can be hashed and compared.
class Capture {
 public:
  Capture() : m_data(kRenderFrames * kChannels, 0.0F) {}

  void render(engine::Engine& eng, std::span<const std::size_t> block_sizes) {
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

      eng.render(std::span<float* const>{channels}, block);

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

  // FNV-1a over the raw bytes, the harness the other e2e tests established.
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

// Eight pads, each an eighth of the loop, with the default envelope and no
// retuning -- the two conditions that keep the voice path free of a fused
// expression.
void load_pads(engine::Engine& eng, const std::shared_ptr<const rt::Sample>& loop) {
  for (std::size_t pad = 0; pad < kUsedPads; ++pad) {
    rt::PadConfig config;
    config.sample = loop;
    config.pad = static_cast<std::uint8_t>(pad);
    config.start_frame = pad * (kLoopFrames / kUsedPads);
    config.end_frame = (pad + 1) * (kLoopFrames / kUsedPads);
    REQUIRE(eng.publish_pad_config(std::make_shared<const rt::PadConfig>(config)));
  }
}

// The mix: gains, balance, a mute, a solo-free set, and every bus in use.
//
// Deliberately NOT all defaults -- a golden over a transparent chain would hold
// just as well if the fader, the balance and the routing did nothing at all.
void apply_mix(engine::Engine& eng) {
  static constexpr std::array<float, kUsedPads> kGains{1.0F,  0.5F, 0.25F, 0.75F,
                                                       1.25F, 0.6F, 0.9F,  0.4F};
  static constexpr std::array<float, kUsedPads> kBalance{0.0F, -1.0F, 0.5F, -0.25F,
                                                         1.0F, 0.0F,  0.8F, -0.6F};

  for (std::size_t pad = 0; pad < kUsedPads; ++pad) {
    rt::StripConfig strip;
    strip.gain = kGains[pad];
    strip.balance = kBalance[pad];
    strip.bus = static_cast<std::uint8_t>(pad % rt::kNumBuses);
    strip.mute = pad == 6;  // one strip out of the mix, so mute is in the hash
    REQUIRE(eng.set_strip(static_cast<std::uint8_t>(pad), strip));
  }

  // Every bus at its own level, so bus routing is observable in the output and
  // not merely in a meter.
  REQUIRE(eng.set_bus_gain(0, 1.0F));
  REQUIRE(eng.set_bus_gain(1, 0.5F));
  REQUIRE(eng.set_bus_gain(2, 0.8F));
  REQUIRE(eng.set_bus_gain(3, 0.3F));
}

// Eight hits, all at offset 0, so all eight voices sound at once -- summation
// order is part of the result, and total overlap exercises it hardest.
//
// OFFSET 0 IS NOT LAZINESS, it is the only value that means the same thing at
// every block size. `PadEvent::frame_offset` is documented as an offset "from
// the start of the block this event is drained in", and drain_pad_events()
// clamps it to num_frames - 1 -- so an offset of 259 lands at frame 259 in a
// 2048-frame block and at frame 63 in a 64-frame one. A first version of this
// file staggered the hits that way and the invariance test below caught it,
// which is the test doing its job on the test rather than on the engine.
//
// Nothing real produces such an offset: a keyboard hit is 0, a MIDI hit is
// computed within the block that is being drained, and a sequenced hit comes
// from the absolute step frame (rt::step_frame) and is always inside the block
// it fires in. Live placement being block-relative is a property of the
// contract, not a hole in it -- docs/TESTING.md now says so.
void play(engine::Engine& eng) {
  for (std::size_t pad = 0; pad < kUsedPads; ++pad) {
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = static_cast<std::uint8_t>(pad), .velocity = 1.0F}));
  }
}

// Everything engaged: EQ, compressor and limiter. Used only where invariance is
// the claim, never under a committed hash.
void engage_processors(engine::Engine& eng) {
  for (std::size_t pad = 0; pad < kUsedPads; ++pad) {
    rt::StripConfig strip = eng.strip(static_cast<std::uint8_t>(pad));
    strip.eq.bands[0] = rt::make_eq_band(rt::EqBandType::kLowShelf, 180.0F, 6.0F, 0.8F, kRate);
    strip.eq.bands[2] = rt::make_eq_band(rt::EqBandType::kPeaking, 2'400.0F, -8.0F, 3.0F, kRate);
    strip.compressor = rt::make_compressor(-24.0F, 6.0F, 6.0F, 3.0F, 48, 2'400);
    REQUIRE(eng.set_strip(static_cast<std::uint8_t>(pad), strip));
  }
  REQUIRE(eng.set_limiter(rt::make_limiter(-3.0F)));
}

[[nodiscard]] Capture run(std::span<const std::size_t> block_sizes, bool processors) {
  engine::Engine eng{mixer_config()};
  const std::shared_ptr<const rt::Sample> loop = make_loop();
  load_pads(eng, loop);
  apply_mix(eng);
  if (processors) {
    engage_processors(eng);
  }
  play(eng);

  Capture capture;
  capture.render(eng, block_sizes);
  return capture;
}

// THE LARGEST BLOCK THE ENGINE ALLOWS, not the whole render.
//
// This said kRenderFrames at first -- 12000 frames against the 2048-frame
// ceiling Engine::render() asserts. The dev preset is RelWithDebInfo with
// NDEBUG, so that assertion is compiled out and the render simply wrote past
// the end of every graph buffer.
//
// Two things caught it, and the order is worth recording. The INVARIANCE TEST
// below failed first, on dev: the one-block capture disagreed with the
// small-block one even with every processor switched off, which is not something
// a working mixer can do. Running the same case under asan -- which keeps
// assertions -- then aborted on the assertion itself, naming the bound directly.
//
// So a golden hash computed on dev alone would have been a hash of a buffer
// overrun, and it would have been perfectly reproducible.
constexpr std::array<std::size_t, 1> kOneBlock{kMaxBlock};
constexpr std::array<std::size_t, 1> kSmallBlocks{64};
constexpr std::array<std::size_t, 4> kRaggedBlocks{101, 1, 512, 37};

}  // namespace

TEST_CASE("e2e: a mixed render is bit-exact", "[e2e]") {
  const Capture session = run(kOneBlock, false);

  // It made a sound, and the mixer is what shaped it. A hash of silence is
  // bit-exact too, and so is a hash of a mixer that ignored every setting.
  CHECK(session.peak() > 0.05F);

  // The mix is audibly different from the same material at unity: something
  // between the voices and the output is doing work.
  engine::Engine flat{mixer_config()};
  const std::shared_ptr<const rt::Sample> loop = make_loop();
  load_pads(flat, loop);
  play(flat);
  Capture unmixed;
  unmixed.render(flat, kOneBlock);
  CHECK_FALSE(session.identical(unmixed));

  // THE COMMITTED GOLDEN. Strip gain, balance, mute, bus routing and bus gain,
  // over eight overlapping voices.
  //
  // Portable for the reason audited at the top of this file: every operation on
  // this path is a lone multiply or a lone add, both exactly specified by
  // IEEE-754, so there is nothing an FMA could fuse differently. Checked on
  // AppleClang/arm64 and clang-18/x86-64 rather than argued.
  //
  // It changes only when the audible behaviour of the mixer changes, and such a
  // change is explained in the commit that makes it -- never re-baselined to
  // make this line pass (CLAUDE.md).
  CHECK(session.hash() == 0x892A38B7E0A1BF21ULL);
}

TEST_CASE("e2e: a mixed render is invariant to block size", "[e2e]") {
  const Capture single = run(kOneBlock, false);
  CHECK(single.peak() > 0.05F);
  CHECK(single.identical(run(kSmallBlocks, false)));
  CHECK(single.identical(run(kRaggedBlocks, false)));
}

TEST_CASE("e2e: an engaged EQ, compressor and limiter are invariant to block size", "[e2e]") {
  // NO COMMITTED HASH HERE, and that is the rule rather than caution: each of
  // the three has an `a*b+c` in its inner loop, so a constant would be a
  // constant about this compiler and this target. Block-size invariance is the
  // property that matters and it needs no constant.
  //
  // It is also the property these three are most able to break -- all three
  // carry state across blocks, and a filter or an envelope reset at a block
  // boundary would still sound approximately right while being wrong.
  const Capture single = run(kOneBlock, true);
  CHECK(single.peak() > 0.05F);
  CHECK(single.identical(run(kSmallBlocks, true)));
  CHECK(single.identical(run(kRaggedBlocks, true)));

  // And the processors did something: the same session without them differs.
  CHECK_FALSE(single.identical(run(kOneBlock, false)));
}

TEST_CASE("e2e: the mixer renders the same twice", "[e2e]") {
  // Run-to-run equality, with everything engaged. Two runs on one machine
  // agreeing is a real property and is what docs/TESTING.md asks for wherever a
  // committed constant is not allowed.
  CHECK(run(kOneBlock, true).identical(run(kOneBlock, true)));
  CHECK(run(kRaggedBlocks, true).identical(run(kRaggedBlocks, true)));
}

TEST_CASE("e2e: the limiter holds the master below its ceiling", "[e2e]") {
  // The one M5 claim that is about a level rather than about repeatability, and
  // it belongs here because it is a property of the WHOLE graph: everything
  // sums into master before the limiter sees it, so this is the only place the
  // ceiling can be checked against a real mix rather than against a test signal.
  engine::Engine eng{mixer_config()};
  const std::shared_ptr<const rt::Sample> loop = make_loop();
  load_pads(eng, loop);

  // Every strip loud and all on one bus, so master is being pushed hard.
  for (std::size_t pad = 0; pad < kUsedPads; ++pad) {
    rt::StripConfig strip;
    strip.gain = 4.0F;
    REQUIRE(eng.set_strip(static_cast<std::uint8_t>(pad), strip));
  }
  REQUIRE(eng.set_limiter(rt::make_limiter(-6.0F)));
  play(eng);

  Capture capture;
  capture.render(eng, kRaggedBlocks);

  const float ceiling = rt::make_limiter(-6.0F).ceiling_linear;
  CHECK(capture.peak() > 0.1F);

  // One ULP of headroom, which is the rounding in (ceiling/peak)*peak and
  // nothing else -- measured in tests/unit/limiter_test.cpp.
  CHECK(capture.peak() <= ceiling * (1.0F + 1.0e-6F));
}
