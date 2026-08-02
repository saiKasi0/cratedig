#include "rt/strip.hpp"

#include "engine/engine.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kBlock = 512;

engine::Engine::Config strip_config() {
  return engine::Engine::Config{
      .sample_rate = kRate, .num_channels = kChannels, .max_block_frames = 2'048, .seed = 0};
}

// Asymmetric between channels on purpose: a balance bug that treats both sides
// alike would be invisible against identical channels, and so would a channel
// mapping that swapped them.
std::shared_ptr<const rt::Sample> strip_sample(std::uint8_t pad) {
  auto sample = std::make_shared<rt::Sample>(kRate, kChannels, std::size_t{8'000});
  for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
    std::span<float> data = sample->mutable_channel(channel);
    for (std::size_t frame = 0; frame < data.size(); ++frame) {
      const auto mixed = static_cast<float>(((frame * 53) + (static_cast<std::size_t>(pad) * 311) +
                                             (static_cast<std::size_t>(channel) * 1'009)) %
                                            8'191);
      data[frame] = ((mixed / 4'095.5F) - 1.0F) * (channel == 0 ? 0.62F : 0.44F);
    }
  }
  return sample;
}

// A whole render pass, channel-major, so two of them can be compared bit for
// bit. Deliberately not shared with engine_render_test.cpp's RenderCapture: this
// file needs to change a strip BETWEEN blocks, which that one cannot express.
class StripRender {
 public:
  explicit StripRender(std::size_t blocks) : m_data(blocks * kBlock * kChannels, 0.0F) {}

  // Renders one block and appends it. `before` runs on the control thread first,
  // which is what makes "move the fader while it is sounding" expressible.
  template <typename Before>
  void block(engine::Engine& eng, std::size_t index, Before&& before) {
    before();

    // Dirtied every time: if render() ever fails to write a sample the test must
    // see the garbage rather than a leftover zero that hides it.
    std::array<float, kBlock * kChannels> scratch{};
    scratch.fill(-3.25F);
    std::array<float*, kChannels> channels{};
    for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
      channels[channel] = scratch.data() + (static_cast<std::size_t>(channel) * kBlock);
    }

    eng.render(std::span<float* const>{channels}, kBlock);

    for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
      float* destination = m_data.data() + (static_cast<std::size_t>(channel) * kBlock) +
                           (index * kBlock * kChannels);
      std::copy_n(channels[channel], kBlock, destination);
    }
  }

  // One block's worth of one channel.
  [[nodiscard]] std::span<const float> channel(std::size_t index, std::uint16_t which) const {
    return std::span<const float>{
        m_data.data() + (index * kBlock * kChannels) + (static_cast<std::size_t>(which) * kBlock),
        kBlock};
  }

  [[nodiscard]] std::span<const float> samples() const { return m_data; }

 private:
  std::vector<float> m_data;
};

[[nodiscard]] float peak_of(std::span<const float> values) {
  float peak = 0.0F;
  for (const float value : values) {
    peak = std::max(peak, std::abs(value));
  }
  return peak;
}

[[nodiscard]] bool identical(std::span<const float> left, std::span<const float> right) {
  return left.size() == right.size() &&
         std::memcmp(left.data(), right.data(), left.size() * sizeof(float)) == 0;
}

void load(engine::Engine& eng, std::span<const std::uint8_t> pads) {
  for (const std::uint8_t pad : pads) {
    REQUIRE(eng.set_pad_sample(pad, strip_sample(pad)));
  }
}

void hit(engine::Engine& eng, std::span<const std::uint8_t> pads) {
  for (const std::uint8_t pad : pads) {
    REQUIRE(eng.trigger_pad(rt::PadEvent{.pad = pad, .velocity = 1.0F}));
  }
}

}  // namespace

TEST_CASE("the balance law is exactly unity at centre", "[unit]") {
  // The trap this exists to keep shut. An equal-power law would put 0.7071 on
  // both sides here, which is a stereo programme 3 dB quieter than it was the
  // day the mixer landed -- and quiet enough to be blamed on anything.
  CHECK(rt::balance_left(0.0F) == 1.0F);
  CHECK(rt::balance_right(0.0F) == 1.0F);

  // Hard over silences the side panned AWAY FROM and leaves the near side
  // untouched. Positive is right, so +1 is what kills the left channel. Not
  // "nearly untouched" on the near side: a pan control that costs 0.2 dB
  // somewhere in its travel is one nobody trusts.
  CHECK(rt::balance_left(1.0F) == 0.0F);
  CHECK(rt::balance_right(1.0F) == 1.0F);
  CHECK(rt::balance_left(-1.0F) == 1.0F);
  CHECK(rt::balance_right(-1.0F) == 0.0F);

  // Linear across the travel, both sides.
  CHECK(rt::balance_right(0.25F) == 1.0F);
  CHECK(rt::balance_left(0.25F) == 0.75F);
}

TEST_CASE("strip values that crossed a thread boundary are clamped, not trusted", "[unit]") {
  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInf = std::numeric_limits<float>::infinity();

  // Unity and silence both survive exactly -- the two values a fader must be
  // able to express.
  CHECK(rt::clamp_strip_gain(1.0F) == 1.0F);
  CHECK(rt::clamp_strip_gain(0.0F) == 0.0F);
  CHECK(rt::clamp_strip_gain(rt::kMaxStripGain) == rt::kMaxStripGain);

  // Out of range falls back to unity rather than to the nearest legal value.
  // std::clamp would map NaN to whichever bound it compared against first, and
  // an infinity to the ceiling -- turning "this number is meaningless" into
  // "play this as loud as the mixer allows".
  CHECK(rt::clamp_strip_gain(kNan) == 1.0F);
  CHECK(rt::clamp_strip_gain(kInf) == 1.0F);
  CHECK(rt::clamp_strip_gain(-1.0F) == 1.0F);
  CHECK(rt::clamp_strip_gain(1.0e9F) == 1.0F);

  CHECK(rt::clamp_strip_balance(kNan) == 0.0F);
  CHECK(rt::clamp_strip_balance(-kInf) == 0.0F);
  CHECK(rt::clamp_strip_balance(2.0F) == 0.0F);
  CHECK(rt::clamp_strip_balance(-0.5F) == -0.5F);

  CHECK(rt::clamp_strip_bus(0) == 0);
  CHECK(rt::clamp_strip_bus(rt::kNumBuses - 1) == rt::kNumBuses - 1);
  CHECK(rt::clamp_strip_bus(rt::kNumBuses) == rt::kDefaultBus);
  CHECK(rt::clamp_strip_bus(255) == rt::kDefaultBus);
}

TEST_CASE("solo is a property of the set, and beats mute", "[unit]") {
  const rt::StripConfig plain{};
  const rt::StripConfig muted{.mute = true};
  const rt::StripConfig soloed{.solo = true};
  const rt::StripConfig both{.mute = true, .solo = true};

  // Nothing soloed: mute decides.
  CHECK(rt::strip_audible(plain, false));
  CHECK_FALSE(rt::strip_audible(muted, false));

  // Something soloed: solo decides, and every strip without it goes quiet --
  // including ones nobody muted.
  CHECK_FALSE(rt::strip_audible(plain, true));
  CHECK(rt::strip_audible(soloed, true));

  // Solo beats mute. Solo says "let me hear this now"; mute is a decision about
  // the mix, and the temporary instruction wins.
  CHECK(rt::strip_audible(both, true));
}

TEST_CASE("the fader moves a voice that is already sounding", "[unit]") {
  // THE POINT OF THE WHOLE TASK, and the trap rt::PadConfig::gain sets: that one
  // is captured into the voice at trigger time, so a mixer built on it would
  // have a fader that only affected the NEXT hit. That is broken in a way that
  // is very hard to describe and very easy to ship.
  //
  // Two engines fed identically, differing only in that the second has its
  // fader moved to 0.5 after the voice is already running.
  constexpr std::array<std::uint8_t, 1> kPads{5};

  engine::Engine reference{strip_config()};
  engine::Engine faded{strip_config()};
  load(reference, kPads);
  load(faded, kPads);

  StripRender flat{2};
  StripRender moved{2};

  flat.block(reference, 0, [&] { hit(reference, kPads); });
  moved.block(faded, 0, [&] { hit(faded, kPads); });

  // Same input, same first block: nothing has been changed yet.
  REQUIRE(identical(flat.channel(0, 0), moved.channel(0, 0)));
  REQUIRE(peak_of(flat.channel(0, 0)) > 0.1F);

  flat.block(reference, 1, [] {});
  moved.block(faded, 1, [&] { REQUIRE(faded.set_strip(5, rt::StripConfig{.gain = 0.5F})); });

  // EXACTLY half, not approximately: 0.5 is a power of two, so x * 0.5f is
  // exact for every value in the block. An approximate comparison here would
  // also pass for a fader applied to the wrong thing, one block late, or with a
  // ramp nobody asked for.
  const std::span<const float> plain = flat.channel(1, 0);
  const std::span<const float> half = moved.channel(1, 0);
  REQUIRE(peak_of(plain) > 0.1F);
  bool exact = true;
  for (std::size_t frame = 0; frame < plain.size(); ++frame) {
    exact = exact && (half[frame] == plain[frame] * 0.5F);
  }
  CHECK(exact);

  // And the voice really was already running -- it was triggered in block 0 and
  // never retriggered, so this is the fader reaching a sounding voice rather
  // than a new one starting quieter.
  CHECK(faded.active_voices() == 1);
}

TEST_CASE("a strip fader is not the pad's own gain", "[unit]") {
  // The other half, so the test above cannot pass for the wrong reason.
  // Publishing a new PadConfig::gain mid-voice must leave what is sounding
  // alone: it is a property of the material, captured at trigger.
  constexpr std::array<std::uint8_t, 1> kPads{5};

  engine::Engine reference{strip_config()};
  engine::Engine retuned{strip_config()};
  load(reference, kPads);
  load(retuned, kPads);

  StripRender flat{2};
  StripRender changed{2};
  flat.block(reference, 0, [&] { hit(reference, kPads); });
  changed.block(retuned, 0, [&] { hit(retuned, kPads); });

  flat.block(reference, 1, [] {});
  changed.block(retuned, 1, [&] {
    const std::shared_ptr<const rt::PadConfig> current = retuned.pad_config(5);
    REQUIRE(current != nullptr);
    rt::PadConfig next = *current;
    next.gain = 0.5F;
    // Built before the REQUIRE rather than moved inside it: clang-tidy reads the
    // macro's expansion and cannot tell that the move is the last use.
    const auto published = std::make_shared<const rt::PadConfig>(next);
    REQUIRE(retuned.publish_pad_config(published));
  });

  CHECK(identical(flat.channel(1, 0), changed.channel(1, 0)));
}

TEST_CASE("mute and solo silence a strip exactly, and restore it exactly", "[unit]") {
  constexpr std::array<std::uint8_t, 2> kBoth{2, 9};
  constexpr std::array<std::uint8_t, 1> kOnly{2};

  // What pad 2 alone sounds like, as the reference both cases must reproduce.
  engine::Engine solo_only{strip_config()};
  load(solo_only, kBoth);
  StripRender alone{1};
  alone.block(solo_only, 0, [&] { hit(solo_only, kOnly); });

  engine::Engine muted{strip_config()};
  load(muted, kBoth);
  StripRender with_mute{1};
  with_mute.block(muted, 0, [&] {
    hit(muted, kBoth);
    REQUIRE(muted.set_strip(9, rt::StripConfig{.mute = true}));
  });

  engine::Engine soloed{strip_config()};
  load(soloed, kBoth);
  StripRender with_solo{1};
  with_solo.block(soloed, 0, [&] {
    hit(soloed, kBoth);
    REQUIRE(soloed.set_strip(2, rt::StripConfig{.solo = true}));
  });

  // Both routes to "only pad 2 is audible" produce the same samples, bit for
  // bit -- muting the other strip and soloing this one are the same statement
  // about what reaches the bus.
  CHECK(identical(alone.samples(), with_mute.samples()));
  CHECK(identical(alone.samples(), with_solo.samples()));

  // It was not silence agreeing with silence.
  CHECK(peak_of(alone.channel(0, 0)) > 0.1F);

  // Un-soloing restores the full mix EXACTLY. Solo is derived per block from the
  // set rather than stored, so there is no state to leak -- and this is what
  // would catch it if there were.
  engine::Engine restored{strip_config()};
  load(restored, kBoth);
  StripRender both_pads{2};
  StripRender toggled{2};

  engine::Engine plain{strip_config()};
  load(plain, kBoth);
  both_pads.block(plain, 0, [&] { hit(plain, kBoth); });
  both_pads.block(plain, 1, [] {});

  toggled.block(restored, 0, [&] {
    hit(restored, kBoth);
    REQUIRE(restored.set_strip(2, rt::StripConfig{.solo = true}));
  });
  toggled.block(restored, 1, [&] { REQUIRE(restored.set_strip(2, rt::StripConfig{})); });

  CHECK(identical(both_pads.channel(1, 0), toggled.channel(1, 0)));
  CHECK_FALSE(identical(both_pads.channel(0, 0), toggled.channel(0, 0)));
}

TEST_CASE("balance attenuates one side and leaves the other untouched", "[unit]") {
  constexpr std::array<std::uint8_t, 1> kPads{4};

  engine::Engine centred{strip_config()};
  engine::Engine panned{strip_config()};
  load(centred, kPads);
  load(panned, kPads);

  StripRender middle{1};
  StripRender right{1};
  middle.block(centred, 0, [&] { hit(centred, kPads); });
  right.block(panned, 0, [&] {
    hit(panned, kPads);
    REQUIRE(panned.set_strip(4, rt::StripConfig{.balance = 1.0F}));
  });

  // Hard right: the left channel is silent and the right is BIT-IDENTICAL to
  // the centred render. Balance attenuates the side panned away from and does
  // nothing at all to the other, which is exactly what a pan law would get
  // wrong.
  CHECK(peak_of(right.channel(0, 0)) == 0.0F);
  CHECK(peak_of(right.channel(0, 1)) > 0.1F);
  CHECK(identical(middle.channel(0, 1), right.channel(0, 1)));

  // And the centred render itself changed nothing on either side.
  CHECK(peak_of(middle.channel(0, 0)) > 0.1F);
}

TEST_CASE("a strip goes to the bus it was routed to, and master hears no difference", "[unit]") {
  // TWO CLAIMS, and the second one alone is worthless. Buses sum into master at
  // unity, so "the output is unchanged" is exactly what you also get from a
  // graph that ignores routing completely -- deleting the routing lookup passed
  // an earlier version of this test, which is how the omission was found. The
  // bus meters are what make the routing observable at all.
  constexpr std::array<std::uint8_t, 1> kPads{7};

  engine::Engine baseline{strip_config()};
  load(baseline, kPads);
  StripRender bus_a{1};
  bus_a.block(baseline, 0, [&] { hit(baseline, kPads); });
  CHECK(peak_of(bus_a.channel(0, 0)) > 0.1F);
  CHECK(baseline.telemetry().bus_peak[rt::kDefaultBus] > 0.0F);

  for (std::uint8_t bus = 1; bus < rt::kNumBuses; ++bus) {
    engine::Engine routed{strip_config()};
    load(routed, kPads);
    StripRender elsewhere{1};
    elsewhere.block(routed, 0, [&] {
      hit(routed, kPads);
      REQUIRE(routed.set_strip(7, rt::StripConfig{.bus = bus}));
    });

    // It moved: the chosen bus has the level and every other bus is silent.
    const engine::Telemetry telemetry = routed.telemetry();
    CHECK(telemetry.bus_peak[bus] > 0.0F);
    for (std::size_t other = 0; other < rt::kNumBuses; ++other) {
      if (other != bus) {
        CHECK(telemetry.bus_peak[other] == 0.0F);
      }
    }

    // And master cannot tell, which is what makes routing a mix decision rather
    // than a level decision -- and a check that no bus has a gain of its own.
    CHECK(identical(bus_a.samples(), elsewhere.samples()));
  }
}

TEST_CASE("the strip meter reports what reached the mix, the pad meter what played", "[unit]") {
  constexpr std::array<std::uint8_t, 2> kPads{1, 3};

  engine::Engine eng{strip_config()};
  load(eng, kPads);
  StripRender render{1};
  render.block(eng, 0, [&] {
    hit(eng, kPads);
    REQUIRE(eng.set_strip(3, rt::StripConfig{.mute = true}));
  });

  const engine::Telemetry telemetry = eng.telemetry();

  // Pad 3 played -- it was hit, and its voice ran. The pad grid should
  // acknowledge that however the mixer is set.
  CHECK(telemetry.pad_peak[3] > 0.0F);

  // But it contributed nothing, and its strip meter says so. Two questions, two
  // numbers; a single meter could only answer one of them.
  CHECK(telemetry.strip_peak[3] == 0.0F);

  // Pad 1 did both.
  CHECK(telemetry.pad_peak[1] > 0.0F);
  CHECK(telemetry.strip_peak[1] > 0.0F);

  // Every strip feeds bus A by default, so that is where the level is and the
  // others are silent.
  CHECK(telemetry.bus_peak[rt::kDefaultBus] > 0.0F);
  for (std::size_t bus = 0; bus < rt::kNumBuses; ++bus) {
    if (bus != rt::kDefaultBus) {
      CHECK(telemetry.bus_peak[bus] == 0.0F);
    }
  }
}

TEST_CASE("a default strip is bit-transparent", "[unit]") {
  // Load-bearing rather than tidy. The M3 and M4 e2e goldens are hashes of the
  // pre-mixer signal path, and every one of them still holds -- which is only
  // possible because a strip at its defaults multiplies by exactly 1.0f on both
  // sides and routes to a bus that sums at unity.
  //
  // Asserted here as well as implied there, because an e2e hash mismatch says
  // "something changed" and this says which thing.
  constexpr std::array<std::uint8_t, 3> kPads{0, 6, 11};

  engine::Engine untouched{strip_config()};
  engine::Engine defaulted{strip_config()};
  load(untouched, kPads);
  load(defaulted, kPads);

  StripRender bare{1};
  StripRender explicitly{1};
  bare.block(untouched, 0, [&] { hit(untouched, kPads); });
  explicitly.block(defaulted, 0, [&] {
    hit(defaulted, kPads);
    // Publishing a default StripConfig must be indistinguishable from never
    // having touched the strip at all.
    for (const std::uint8_t pad : kPads) {
      REQUIRE(defaulted.set_strip(pad, rt::StripConfig{}));
    }
  });

  CHECK(identical(bare.samples(), explicitly.samples()));
  CHECK(peak_of(bare.channel(0, 0)) > 0.1F);
}
