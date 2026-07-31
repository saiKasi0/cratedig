#ifndef CRATEDIG_INGEST_ONSET_HPP
#define CRATEDIG_INGEST_ONSET_HPP

#include "rt/sample.hpp"

#include <cstddef>
#include <vector>

namespace ingest {

// Where the hits are.
//
// WORKER THREAD ONLY. Allocates, runs an FFT per analysis frame, and takes
// milliseconds on a short loop -- everything the audio thread may not do. It
// produces positions; the audio thread never sees this code.
//
// The method is spectral flux with an adaptive median threshold, which is the
// standard approach for percussive material (Dixon, "Onset Detection
// Revisited", DAFx-06) and the one docs/ROADMAP.md names. Flux rather than
// amplitude, because a snare landing over a ringing kick barely changes the
// amplitude envelope while changing the spectrum completely.

struct OnsetParams {
  // Frames between analysis windows. 256 at 48 kHz is 5.3 ms, which bounds how
  // precisely a hit can be located before backtracking refines it.
  std::size_t hop = 256;

  // threshold = delta + lambda * median(flux over a local window).
  //
  // ADAPTIVE, not fixed: a fixed threshold has to be re-tuned for every file,
  // and within one file it either misses the quiet half or invents hits in the
  // loud half. The median is used rather than the mean because it is not
  // dragged upward by the very peaks being detected.
  float threshold_lambda = 1.6F;

  // An absolute floor, in units of the normalised flux (which peaks at 1.0).
  // Without it, a passage of near-silence has a near-zero median and every
  // ripple in it clears the threshold.
  float threshold_delta = 0.04F;

  // Analysis frames either side, for the median. 8 either side at hop 256 is
  // ~90 ms of context: long enough to span a couple of hits, short enough to
  // track a real change in density.
  std::size_t median_radius = 8;

  // Refractory period. Two detections closer than this are one hit: 30 ms is
  // faster than any drummer and slower than the ring of a single transient.
  double min_gap_seconds = 0.030;

  // Move each detection back to where the attack actually starts.
  //
  // Flux peaks once the transient is well inside the analysis window, which is
  // systematically LATE -- and a slice that starts late has its transient
  // clipped off, which is the one artefact that makes chopped drums sound
  // obviously wrong. Off only for measuring what it is worth.
  bool backtrack = true;
};

struct OnsetResult {
  // Positions in source frames, ascending, no duplicates.
  std::vector<std::size_t> frames;

  // The detection function itself, one value per analysis frame, normalised so
  // its maximum is 1. Exposed because it is the thing to look at when a chop
  // comes out wrong, and because the accuracy tests plot it.
  std::vector<float> flux;

  // The threshold that was applied to it, same length as `flux`.
  std::vector<float> threshold;

  std::size_t hop = 0;
};

[[nodiscard]] OnsetResult detect_onsets(const rt::Sample& sample, const OnsetParams& params = {});

}  // namespace ingest

#endif  // CRATEDIG_INGEST_ONSET_HPP
