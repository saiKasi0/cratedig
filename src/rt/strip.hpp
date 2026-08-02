#ifndef CRATEDIG_RT_STRIP_HPP
#define CRATEDIG_RT_STRIP_HPP

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

}  // namespace rt

#endif  // CRATEDIG_RT_STRIP_HPP
