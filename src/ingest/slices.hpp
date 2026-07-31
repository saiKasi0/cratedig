#ifndef CRATEDIG_INGEST_SLICES_HPP
#define CRATEDIG_INGEST_SLICES_HPP

#include "ingest/onset.hpp"
#include "rt/sample.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ingest {

// One chop: a half-open range of source frames.
//
// Deliberately NOT rt::PadConfig. A slice is an ingest-side description of where
// the material is; a PadConfig is what the audio thread reads. Keeping them
// separate is what lets one slice be assigned to several pads with different
// envelopes, and keeps analysis types out of src/rt/.
struct Slice {
  std::size_t start_frame = 0;
  std::size_t end_frame = 0;  // exclusive

  // How far the zero-crossing snap moved each boundary, signed, in frames.
  //
  // Kept so the EDIT screen can say "start snapped -3 smp" as the mockups do,
  // and so a boundary the snap could not improve is distinguishable from one it
  // did not need to.
  std::ptrdiff_t start_snap = 0;
  std::ptrdiff_t end_snap = 0;

  [[nodiscard]] std::size_t length() const noexcept {
    return end_frame > start_frame ? end_frame - start_frame : 0;
  }
};

enum class ChopAlgorithm : std::uint8_t {
  kTransient = 0,
  kGrid,
};

struct SliceSet {
  std::vector<Slice> slices;
  ChopAlgorithm algorithm = ChopAlgorithm::kTransient;

  [[nodiscard]] bool empty() const noexcept { return slices.empty(); }

  [[nodiscard]] std::size_t size() const noexcept { return slices.size(); }
};

struct SnapParams {
  // How far a boundary may move to reach a zero crossing. 64 frames is 1.3 ms
  // at 48 kHz -- inaudible as a timing change, and enough to reach a crossing in
  // anything above about 400 Hz.
  //
  // It is a GIVE-UP BOUND, not a risk budget: the search runs outward from the
  // boundary, so it always finds the nearest crossing and a larger radius never
  // makes it pick a worse one. What the small value protects is the case where
  // there is no nearby crossing at all -- a sustained low tone, where the next
  // crossing can be a whole half-period away. At 40 Hz that is 12 ms, which is
  // no longer an inaudible correction but a moved hit. Better to leave the
  // boundary where the detector put it and let PadConfig's declick fade handle
  // the step.
  std::size_t radius = 64;

  bool enabled = true;
};

// The nearest frame to `at` where the mono sum changes sign, within `radius`.
//
// Returns `at` unchanged when there is no crossing in range, which is the
// honest answer for a boundary inside a sustained tone -- there is nothing to
// snap to, and PadConfig's declick fade is the backstop for exactly that case.
//
// Works on the MONO SUM rather than per channel. A stereo file's channels cross
// at different frames, so no single boundary is a zero crossing in both; the sum
// is what a listener hears stepping, and it is the one worth minimising.
[[nodiscard]] std::size_t snap_to_zero_crossing(const rt::Sample& sample, std::size_t at,
                                                std::size_t radius);

// Chop at detected transients.
//
// Slices run from one onset to the next, so material BEFORE the first onset is
// not in any slice. That is what a hardware sampler does and what a player
// expects: leading silence or a count-in is not a chop.
[[nodiscard]] SliceSet chop_transient(const rt::Sample& sample, const OnsetParams& onset = {},
                                      const SnapParams& snap = {});

// Chop into `parts` equal pieces. What `:chop grid 16` does, and the right
// answer for a loop that is already in time.
[[nodiscard]] SliceSet chop_grid(const rt::Sample& sample, std::size_t parts,
                                 const SnapParams& snap = {});

}  // namespace ingest

#endif  // CRATEDIG_INGEST_SLICES_HPP
