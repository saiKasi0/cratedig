#ifndef CRATEDIG_RT_STRIP_HPP
#define CRATEDIG_RT_STRIP_HPP

#include "rt/compressor.hpp"
#include "rt/eq.hpp"
#include "rt/pad_event.hpp"

#include <cstddef>
#include <cstdint>

namespace rt {

// The shape of the mixer graph. Sixteen strips (one per pad), four buses, one
// master -- see docs/MIXER.md, "Signal flow".
//
// FIXED, and that is the design rather than a simplification. Nothing here is
// created or destroyed while the stream is running, so there is no topology to
// allocate on the audio thread, no null node to check for, and no ordering to
// recompute. The engine preallocates one buffer per node at construction and
// walks them in the same order every block, which is also what makes the graph
// deterministic without any effort.
//
// Four buses because that is the smallest number that lets the obvious grouping
// happen -- drums, bass, music, vocals -- and because a bus costs a buffer and a
// summation pass whether anything is routed to it or not.
inline constexpr std::size_t kNumBuses = 4;

// Where a strip goes when nobody has said otherwise.
//
// Bus A, not "spread the pads across the four buses". A mixer sends everything
// to the main mix until you decide otherwise; grouping pads is a musical
// judgement about a particular track, and a default that guesses at one would be
// wrong for every track that disagrees with it. The other three buses are
// summed anyway -- adding a silent bus is adding exactly 0.0f, which is exact --
// so a fixed shape costs nothing in accuracy.
inline constexpr std::uint8_t kDefaultBus = 0;

static_assert(kDefaultBus < kNumBuses, "the default bus has to exist");
static_assert(kNumPads > 0, "the graph needs at least one strip");

// The most a strip fader may add, linear. +12 dB.
//
// A limit rather than an opinion: this value crosses a thread boundary, and the
// audio thread has to do something defined with whatever arrives. Twelve dB is
// well past any mix decision and well short of the range where one bad number
// makes the output painful.
inline constexpr float kMaxStripGain = 4.0F;

// The mixer half of a pad, and the reason it lives inside rt::PadConfig rather
// than in a table of its own: docs/ARCHITECTURE.md's one-pointer rule. One
// object, one publish, one swap -- a parallel strip table would be a second
// thing to keep in step with the first, and the audio thread would eventually
// read a fader from one edit with a slice from another.
//
// THIS IS NOT PadConfig::gain, and the difference is the whole point.
// PadConfig::gain is multiplied into the voice AT TRIGGER TIME along with
// velocity: it says how loud this chop is, and changing it deliberately leaves
// what is already sounding alone. StripConfig::gain is the fader, applied per
// block on the strip, and it moves what is sounding -- because a fader that only
// affected the next hit is broken in a way that is very hard to describe and
// very easy to ship.
//
// Declared in SIGNAL ORDER -- gain, EQ, compressor, balance -- so the struct
// reads as the chain docs/MIXER.md specifies rather than as a bag of settings.
struct StripConfig {
  // Linear, both channels. 1.0 is unity and is exact: x * 1.0f == x for every
  // finite x, which is what lets a default strip be bit-transparent rather than
  // merely quiet-transparent (docs/MIXER.md).
  float gain = 1.0F;

  // Four bands, all bypassed until somebody enables one. The FILTER STATE is not
  // here and must not be: a config is immutable and shared between the pad and
  // every voice holding it, while state belongs to one strip on the audio
  // thread. The engine owns the rt::Biquad instances.
  EqConfig eq{};

  // One per strip, after the EQ. Off until somebody enables it, and its default
  // ratio of 1 is unity at every level anyway -- two independent reasons a fresh
  // strip cannot change the signal.
  CompressorConfig compressor{};

  // [-1, +1], 0 at centre. BALANCE, not a constant-power pan law -- see
  // balance_left/right below.
  float balance = 0.0F;

  // Which bus this strip feeds.
  std::uint8_t bus = kDefaultBus;

  // Silent, unless something else is soloed. See strip_audible().
  bool mute = false;

  // Audible, and everything not soloed is not. A property of the SET rather than
  // of this strip, which is why nothing here counts how many are set.
  bool solo = false;
};

// Clamps, applied on the audio thread where the value is used.
//
// The same discipline as VoicePool::step_for: these arrive from the control
// thread and are not trusted, and the audio thread does not get to abort on bad
// input. Written as "in range ? value : default" rather than std::clamp on
// purpose -- NaN fails every comparison, so it falls through to the default
// instead of propagating into the mix and silencing the output for good.
[[nodiscard]] constexpr float clamp_strip_gain(float gain) noexcept {
  return gain >= 0.0F && gain <= kMaxStripGain ? gain : 1.0F;
}

[[nodiscard]] constexpr float clamp_strip_balance(float balance) noexcept {
  return balance >= -1.0F && balance <= 1.0F ? balance : 0.0F;
}

[[nodiscard]] constexpr std::uint8_t clamp_strip_bus(std::uint8_t bus) noexcept {
  return bus < kNumBuses ? bus : kDefaultBus;
}

// The balance law. Attenuates only the side being panned away from.
//
// BOTH ARE EXACTLY 1.0 AT CENTRE, and that is the requirement rather than a
// property. An equal-power law is cos/sin of the pan angle and gives 0.7071 on
// both sides at centre -- 3 dB down -- which for a stereo signal is simply
// wrong, and would have made every existing recording quieter the day the mixer
// landed. Placing a mono source in a stereo field is a different job, and if it
// is ever wanted it is a different control.
[[nodiscard]] constexpr float balance_left(float balance) noexcept {
  return balance <= 0.0F ? 1.0F : 1.0F - balance;
}

[[nodiscard]] constexpr float balance_right(float balance) noexcept {
  return balance >= 0.0F ? 1.0F : 1.0F + balance;
}

// Whether this strip reaches its bus, given whether ANY strip is soloed.
//
// Solo is derived from the whole set every block rather than stored, so there is
// no solo count to leak when a strip is reconfigured, muted or reloaded.
//
// Solo beats mute. A soloed strip that is also muted is audible: solo is a
// statement about what you want to hear right now, mute is a statement about the
// mix, and the temporary one wins.
[[nodiscard]] constexpr bool strip_audible(const StripConfig& strip, bool any_soloed) noexcept {
  return any_soloed ? strip.solo : !strip.mute;
}

static_assert(balance_left(0.0F) == 1.0F && balance_right(0.0F) == 1.0F,
              "a centred balance must be exactly unity on both sides");
static_assert(clamp_strip_gain(1.0F) == 1.0F, "unity gain must survive the clamp exactly");

}  // namespace rt

#endif  // CRATEDIG_RT_STRIP_HPP
