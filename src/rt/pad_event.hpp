#ifndef CRATEDIG_RT_PAD_EVENT_HPP
#define CRATEDIG_RT_PAD_EVENT_HPP

#include <cstdint>
#include <type_traits>

namespace rt {

// How many pads the engine addresses. Sixteen because that is the 4x4 grid in
// docs/design and the shape of every hardware sampler this thing is imitating.
inline constexpr std::uint8_t kNumPads = 16;

// A pad hit, sent control -> audio through an SpscRing.
//
// Trivially copyable by construction, because SpscRing overwrites slots by
// assignment and never runs a destructor. That constraint is what keeps the
// message path allocation-free and lock-free, so it is enforced below rather
// than left to reviewer attention.
struct PadEvent {
  // Which pad. Out-of-range values are dropped by the engine rather than
  // trusted: this arrives from a key handler, and later from MIDI.
  std::uint8_t pad = 0;

  // Linear gain in [0, 1]. Normalised here rather than carrying MIDI's 7-bit
  // velocity, so that the src/io/ MIDI layer owns the curve choice in M4 and
  // the engine never has to know where a hit came from.
  float velocity = 1.0F;

  // Offset in frames from the start of the block this event is drained in.
  //
  // M1 always triggers at the block start and therefore always sets 0. The field
  // exists now because M4 makes triggers sample-accurate, and adding it later
  // would mean changing a wire format that MIDI, the sequencer and the offline
  // renderer all already speak.
  std::uint32_t frame_offset = 0;
};

static_assert(std::is_trivially_copyable_v<PadEvent>,
              "PadEvent must be trivially copyable to travel through SpscRing");

}  // namespace rt

#endif  // CRATEDIG_RT_PAD_EVENT_HPP
