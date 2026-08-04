#ifndef CRATEDIG_ENGINE_TAKE_HPP
#define CRATEDIG_ENGINE_TAKE_HPP

#include "rt/pad_event.hpp"
#include "rt/sequencer.hpp"

#include <cstddef>
#include <cstdint>

namespace engine {

// Turning what somebody played into what the pattern holds.
//
// CONTROL THREAD, and pure. It takes an rt::PadHit reported by the audio thread
// (rt/pad_event.hpp) and works out which step of which pattern it belongs to.
// Nothing here touches the engine, the transport or any state of its own, which
// is what makes quantisation testable against arithmetic rather than against a
// running sequencer.
//
// WHY THIS IS NOT ON THE AUDIO THREAD. It could be -- the audio thread already
// computes step positions, and it knows the transport exactly. But quantise
// resolution is a SETTING, and a setting the audio thread owns is one that needs
// a message to change and a test with a render loop to check. The frame in a
// PadHit is exact; everything below is arithmetic on it, and it can be redone
// with a different answer for free.

// Snap resolutions, in steps of the native grid.
//
// THERE IS NO "OFF", and that is a property of the pattern rather than a missing
// feature: rt::Step is {on, velocity} with no micro-timing, so the finest a take
// can be recorded at is the finest the pattern can hold. Offering an "off" that
// silently snapped to sixteenths anyway would be a setting that did nothing.
//
// UNQUANTISED PLAYBACK IS DEFERRED BY DECISION, not overlooked. Giving a step a
// timing offset changes the struct M6 serialises into the project file and the
// scan in Engine::fire_sequencer_steps, which is a milestone rather than a knob
// -- see docs/ROADMAP.md. Until then, "how finely" is the only question a take
// asks, and these are the answers.
inline constexpr std::uint8_t kQuantiseSixteenth = 1;
inline constexpr std::uint8_t kQuantiseEighth = 2;
inline constexpr std::uint8_t kQuantiseQuarter = 4;  // on the beat
inline constexpr std::uint8_t kQuantiseHalf = 8;

// Where a hit belongs.
struct HitPlacement {
  // Which pattern was playing there. Resolved through the song, so a take that
  // runs across a pattern change writes into both -- which is what recording
  // over a chained song has to mean.
  std::uint8_t pattern = 0;

  // The step within that pattern.
  std::size_t step = 0;

  // The step counted from the transport origin, before the pattern wrap. Here
  // because it is what the arithmetic actually produced, and a caller checking
  // the quantiser wants to see it rather than a value already folded modulo
  // something.
  std::uint64_t absolute_step = 0;
};

// Which step a frame belongs to, at the given snap resolution.
//
// SWING IS RESPECTED, and it has to be. A swung pattern sounds its odd steps
// late by up to three quarters of a step, and a player following what they hear
// plays late with it. Rounding those hits against the straight grid would push
// every one of them onto the following step -- so a swung pattern would record
// wrong in exactly the way that is hardest to hear as a quantisation bug and
// easiest to hear as "the machine cannot play in time".
//
// `quantise_steps` of zero is treated as one rather than as a division by zero.
[[nodiscard]] HitPlacement place_hit(const rt::SequencerState& state, std::uint32_t sample_rate,
                                     std::uint8_t quantise_steps, std::uint64_t frame) noexcept;

// Writes one hit into `state`. False if the pad is out of range, which is the
// only way this can fail.
//
// LAST HIT WINS on a step that already has one, and that is the grid rather than
// a choice: a pattern holds one cell per pad per step, so two hits that quantise
// together are one note. It is also what a player expects from correcting
// themselves mid-loop.
bool record_hit(rt::SequencerState& state, std::uint32_t sample_rate, std::uint8_t quantise_steps,
                const rt::PadHit& hit) noexcept;

}  // namespace engine

#endif  // CRATEDIG_ENGINE_TAKE_HPP
