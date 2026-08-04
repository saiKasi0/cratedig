#ifndef CRATEDIG_INGEST_TEMPO_HPP
#define CRATEDIG_INGEST_TEMPO_HPP

#include "ingest/onset.hpp"

#include <cstdint>

namespace ingest {

// How fast a sample is, read off the material rather than typed.
//
// WORKER/CONTROL THREAD, like everything else in `ingest`. It reads the onset
// detection function, so a caller that already has an OnsetAnalysis pays nothing
// for the expensive half twice -- which is the whole reason analyse_onsets() was
// split out for the live re-chop preview.
//
// THE METHOD is autocorrelation of the detection function. A periodic pulse
// train correlates with itself at the beat period and at its multiples, so the
// lag with the strongest correlation is the beat -- and unlike counting the gaps
// between onsets, it survives missing hits and extra ones, because every onset
// contributes to every lag rather than one interval each.

// The tempo range this searches. Below 60 the autocorrelation window is longer
// than most loops; above 200 the lag is short enough that a snare and its own
// ring correlate.
inline constexpr std::uint32_t kMinBpmX100 = 6'000;
inline constexpr std::uint32_t kMaxBpmX100 = 20'000;

struct TempoEstimate {
  // 0 when nothing was found -- too short, too sparse, or no periodicity.
  // Callers must treat that as ordinary rather than as an error: a one-shot has
  // no tempo, and saying so is the right answer.
  //
  // THE METRICAL LEVEL IS NOT DETERMINED BY THE SIGNAL, and that is a property
  // of music rather than a weakness here. A pulse train at 172 BPM repeats
  // exactly at 86, at 57.3 and at 344 -- every one of those is a true period,
  // and no amount of analysis can pick among them because they are all correct.
  // Measured on an isochronous train: raw correlations of 0.0224 at 86 BPM,
  // 0.0209 at 173 and 0.0203 at 114.8, within 10% of each other.
  //
  // A comb over the multiples helps when the material has real metrical
  // structure -- a fundamental whose multiples also correlate -- and cannot help
  // when it does not. What is left is a PREFERENCE for where most music sits,
  // which is what a listener uses too. So this reports a genuine period near
  // 120 BPM, and the person doubles or halves it if they wanted a different
  // level. It is a starting point, not an oracle.
  std::uint32_t bpm_x100 = 0;

  // How much the winning lag stood out, as a ratio over the mean correlation.
  // Not a probability and not presented as one; it is here so the interface can
  // say "92 bpm" confidently or "92 bpm?" and mean it.
  float confidence = 0.0F;

  [[nodiscard]] bool found() const noexcept { return bpm_x100 != 0; }
};

[[nodiscard]] TempoEstimate detect_tempo(const OnsetAnalysis& analysis);

}  // namespace ingest

#endif  // CRATEDIG_INGEST_TEMPO_HPP
