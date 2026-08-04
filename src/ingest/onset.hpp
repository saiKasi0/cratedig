#ifndef CRATEDIG_INGEST_ONSET_HPP
#define CRATEDIG_INGEST_ONSET_HPP

#include "rt/sample.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ingest {

// Where the hits are.
//
// WORKER THREAD ONLY. Allocates, runs an FFT per analysis frame, and takes
// milliseconds on a short loop -- everything the audio thread may not do. It
// produces positions; the audio thread never sees this code.
//
// The method is LOG-magnitude spectral flux with an adaptive median threshold,
// which is the standard approach for percussive material (Dixon, "Onset
// Detection Revisited", DAFx-06) and the one docs/ROADMAP.md names. Flux rather
// than amplitude, because a snare landing over a ringing kick barely changes the
// amplitude envelope while changing the spectrum completely; log rather than
// linear because a real kick's pitch-falling decay does change the spectrum, and
// on linear magnitudes it changes it enough to look like a second hit. See the
// note at the flux loop in onset.cpp for the measurement.

// The most of the spectrum a profile may discard. Beyond this there is too
// little left to detect anything, and the detector returns nothing rather than
// something worse -- see OnsetParams::hf_emphasis.
inline constexpr float kMaxHfEmphasis = 0.85F;

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

  // The fraction of the spectrum, from the bottom, that does not count toward
  // the flux at all. 0 uses every bin; 0.4 ignores the lowest 40%.
  //
  // ZERO IS EXACTLY TODAY'S BEHAVIOUR, which is why it is the default: no
  // existing detection moves and no committed chop changes.
  //
  // What it is for: a note attack on a sustained instrument puts its energy in
  // the upper partials while the fundamental carries on unchanged, so a sum over
  // every bin is dominated by low bins that did not move. Ignoring the bottom is
  // what separates "a new note started" from "the same note is still ringing".
  // Drums do not need it -- a drum is broadband and the flat sum already sees it.
  //
  // A CUT RATHER THAN A TILT, and that is a measurement. This began as a weight
  // ramp, `1 + emphasis * (bin / last)`. Measured on sustained material it did
  // NOTHING: at 0, 0.4, 0.8, 2.0 and even 6.0 it found the same 7 of 7 true
  // onsets and varied the total by one. A smooth monotone reweighting does not
  // move where the flux peaks, and peak position is all that survives
  // normalisation and the adaptive threshold. Cutting the band does move it: on
  // the same material 0.4 keeps 7 of 7 while dropping the spurious detections
  // from three to one. A knob that does nothing is worse than no knob.
  //
  // NO SHIPPED DENSITY SETS IT, and that is also a measurement. On a real record
  // -- distorted guitar, low E riff -- a cut of 0.2 pushed the first detection
  // from 0.05 s to 1.45 s and a cut of 0.4 to 2.64 s: the opening was simply
  // gone, because the band being discarded is the band the riff is in. It stays
  // as a parameter because the sustained-material measurement is real and the
  // live re-chop preview should be able to reach it. It is not a default
  // anywhere.
  //
  // Clamped on use to [0, kMaxHfEmphasis]. Above 1 there are no bins left and
  // the detector finds nothing at all -- which is precisely the "one slice
  // covering the whole file" that docs/ROADMAP.md describes as the symptom of a
  // chop gone wrong, and it must not be reachable by typing a number.
  float hf_emphasis = 0.0F;

  // Move each detection back to where the attack actually starts.
  //
  // Flux peaks once the transient is well inside the analysis window, which is
  // systematically LATE -- and a slice that starts late has its transient
  // clipped off, which is the one artefact that makes chopped drums sound
  // obviously wrong. Off only for measuring what it is worth.
  bool backtrack = true;
};

// How fine a chop to make.
//
// DENSITY, NOT MATERIAL, and that is a correction. This began as
// drums/melodic/mixed -- profiles named after what you were chopping -- because
// that is what docs/ROADMAP.md asked for. Measured against a real record
// (distorted guitar, ~172 BPM) the material names did not survive: the "melodic"
// settings MISSED THE ENTIRE OPENING RIFF, because their band cut discards the
// low bins a distorted E-based riff lives in, and the drums default cut almost
// exactly one slice per strum. Nobody looking at that wanted a different
// algorithm; they wanted the same one to cut less often.
//
// So the axis is musical: a slice per strum, per beat, or per bar. The first is
// a property of the sound, the other two are properties of the tempo, which is
// why params_for() needs one.
enum class ChopDensity : std::uint8_t {
  // Every attack the detector can find. The settings this detector has always
  // had, so `:chop transient` with nothing after it is unchanged and no
  // committed chop moves.
  kStrum,

  // No two slices closer than about a beat.
  kBeat,

  // Phrase-length pieces -- riffs whole, rather than rebuilt from parts.
  kBar,
};

// Beats to a bar. Four, because the sequencer has no time signature yet and
// pretending otherwise would be inventing a feature to justify a constant.
inline constexpr int kBeatsPerBar = 4;

// The parameters a density stands for at a given tempo.
//
// `bpm_x100` is the session's tempo, as `rt::SequencerState` carries it. kStrum
// ignores it; the other two are defined in beats and cannot be expressed without
// it. When M5.7's tempo detection lands it feeds this rather than replacing it.
[[nodiscard]] OnsetParams params_for(ChopDensity density, std::uint32_t bpm_x100) noexcept;

// The density a word names, or nothing. For the `:` grammar; here rather than in
// the parser so the names and the parameters cannot drift apart.
[[nodiscard]] bool density_from_name(std::string_view name, ChopDensity& out) noexcept;

[[nodiscard]] std::string_view name_of(ChopDensity density) noexcept;

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

// The expensive half, kept so it can be reused.
//
// WHY THIS IS SPLIT OUT. Detection is one FFT per hop -- the whole cost -- and
// then a couple of linear passes to threshold and pick. The live re-chop preview
// changes sensitivity and gap on every keystroke, which only the second half
// depends on; re-running the first would put a full analysis between a key and
// the screen. ARCHITECTURE.md records 161 ms to chop a five-and-a-half-minute
// file, so that is 161 ms a keypress on material people actually chop.
//
// Only `hop` and `hf_emphasis` reach this. Everything else in OnsetParams is
// picking, and can be changed without touching it.
struct OnsetAnalysis {
  // The detection function, normalised so its maximum is 1.
  std::vector<float> flux;

  // The downmixed source, kept because sample-resolution attack refinement
  // needs it and re-downmixing per keystroke would put back a cost this split
  // exists to remove.
  std::vector<float> mono;

  std::size_t hop = 0;
  std::uint32_t sample_rate = 0;
};

[[nodiscard]] OnsetAnalysis analyse_onsets(const rt::Sample& sample,
                                           const OnsetParams& params = {});

// The cheap half: threshold, pick, backtrack. Safe to run on every keystroke.
[[nodiscard]] OnsetResult pick_onsets(const OnsetAnalysis& analysis,
                                      const OnsetParams& params = {});

// Both at once, which is what every non-interactive caller wants.
[[nodiscard]] OnsetResult detect_onsets(const rt::Sample& sample, const OnsetParams& params = {});

}  // namespace ingest

#endif  // CRATEDIG_INGEST_ONSET_HPP
