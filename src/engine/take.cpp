#include "engine/take.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace engine {
namespace {

// How many snap points either side of the first guess to consider.
//
// One would do with no swing: the base-grid estimate floors, so the nearest snap
// is the guess or the one after it. Swing is what makes two necessary -- it
// moves a step as much as three quarters of a step later than its base, so at
// the finest resolution the guess can be a whole step out. Five candidates is
// five multiplications on the control thread, once per note played.
constexpr std::uint64_t kSearchRadius = 2;

// The swing that applies at an absolute step, which in a song depends on which
// pattern is playing there.
[[nodiscard]] std::uint8_t swing_at(const rt::SequencerState& state, std::uint64_t step) noexcept {
  const rt::SongPosition position = rt::song_position_at(state, step);
  return state.patterns[position.pattern].swing;
}

// Where an absolute step actually sounds.
//
// THE SAME CALL Engine::fire_sequencer_steps makes, deliberately: a take has to
// land where the sequencer will replay it, and the only way to be sure of that
// is to ask the same function. Two implementations of "where is step n" would
// agree until somebody changed one of them.
[[nodiscard]] std::uint64_t sounding_frame(const rt::SequencerState& state,
                                           std::uint32_t sample_rate, std::uint64_t step) noexcept {
  return rt::step_frame(step, sample_rate, state.bpm_x100, swing_at(state, step));
}

}  // namespace

HitPlacement place_hit(const rt::SequencerState& state, std::uint32_t sample_rate,
                       std::uint8_t quantise_steps, std::uint64_t frame) noexcept {
  const std::uint64_t snap = quantise_steps == 0 ? 1 : quantise_steps;

  // A first guess off the straight grid, then a search around it. The guess
  // alone is not the answer for two separate reasons: it floors where the
  // nearest snap point may be the one above, and it ignores swing entirely.
  const std::uint64_t centre = rt::step_index_at(frame, sample_rate, state.bpm_x100) / snap;
  const std::uint64_t first = centre > kSearchRadius ? centre - kSearchRadius : 0;

  std::uint64_t best_step = 0;
  std::uint64_t best_distance = std::numeric_limits<std::uint64_t>::max();
  for (std::uint64_t index = first; index <= centre + kSearchRadius; ++index) {
    const std::uint64_t step = index * snap;
    const std::uint64_t at = sounding_frame(state, sample_rate, step);
    const std::uint64_t distance = at > frame ? at - frame : frame - at;

    // Strictly less than, so a hit exactly between two snap points takes the
    // EARLIER one. Arbitrary, and fixed rather than arbitrary at run time: a tie
    // that broke differently depending on iteration order would make a take
    // non-reproducible, and reproducibility is a promise this project makes.
    if (distance < best_distance) {
      best_distance = distance;
      best_step = step;
    }
  }

  const rt::SongPosition position = rt::song_position_at(state, best_step);
  return HitPlacement{
      .pattern = position.pattern,
      .step = position.step,
      .absolute_step = best_step,
  };
}

bool record_hit(rt::SequencerState& state, std::uint32_t sample_rate, std::uint8_t quantise_steps,
                const rt::PadHit& hit) noexcept {
  if (hit.pad >= rt::kNumPads) {
    return false;
  }

  const HitPlacement where = place_hit(state, sample_rate, quantise_steps, hit.frame);
  rt::Step& cell = state.patterns[where.pattern].steps[where.step][hit.pad];
  cell.on = true;

  // At least 1. A step that is on at velocity zero is a note that exists in the
  // pattern lane and makes no sound, which is a worse answer to "why can I not
  // hear it" than anything the extra count costs.
  cell.velocity = std::max<std::uint8_t>(hit.velocity, 1);
  return true;
}

std::size_t record_hits(rt::SequencerState& state, std::uint32_t sample_rate,
                        std::uint8_t quantise_steps, std::span<const rt::PadHit> hits) noexcept {
  std::size_t written = 0;
  for (const rt::PadHit& hit : hits) {
    if (record_hit(state, sample_rate, quantise_steps, hit)) {
      ++written;
    }
  }
  return written;
}

}  // namespace engine
