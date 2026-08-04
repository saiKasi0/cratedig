#include "ingest/tempo.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ingest {
namespace {

// Where a listener's ear sits, and the reason octave errors are survivable.
//
// A pulse train at 172 BPM correlates just as well at 86 and at 344 -- every
// other beat and every half beat are equally periodic, and autocorrelation
// cannot tell which one a person taps. Nothing in the signal breaks the tie, so
// the tie is broken by preference: a Gaussian on log2(tempo) centred where most
// music is.
//
// 120 BPM and about an octave of spread, which is the shape the tempo-induction
// literature settles on (Parncutt; Klapuri). It is a WEIGHTING, not a clamp --
// a strong 172 still beats a weak 120, which is what makes fast material work.
// How many multiples of a candidate lag the comb looks at. Four covers a beat,
// a bar of two, three and four -- past that the correlation window is short
// enough that the estimate is noise.
constexpr std::size_t kCombPartials = 4;

constexpr double kPreferredBpm = 120.0;
constexpr double kPreferenceOctaves = 0.9;

[[nodiscard]] double preference(double bpm) {
  const double octaves = std::log2(bpm / kPreferredBpm) / kPreferenceOctaves;
  return std::exp(-0.5 * octaves * octaves);
}

}  // namespace

TempoEstimate detect_tempo(const OnsetAnalysis& analysis) {
  TempoEstimate out;
  if (analysis.flux.empty() || analysis.hop == 0 || analysis.sample_rate == 0) {
    return out;
  }

  // Seconds per analysis frame -- what turns a lag into a tempo.
  const double frame_seconds =
      static_cast<double>(analysis.hop) / static_cast<double>(analysis.sample_rate);
  if (frame_seconds <= 0.0) {
    return out;
  }

  const auto lag_for = [frame_seconds](double bpm) {
    return static_cast<std::size_t>(std::lround(60.0 / (bpm * frame_seconds)));
  };

  // Fast tempo -> short lag, so the bounds cross over.
  const std::size_t shortest = lag_for(static_cast<double>(kMaxBpmX100) / 100.0);
  const std::size_t longest = lag_for(static_cast<double>(kMinBpmX100) / 100.0);
  if (shortest < 1 || longest <= shortest) {
    return out;
  }

  // TWO FULL PERIODS, at least. A file shorter than that has nothing to
  // correlate against and would report whichever lag happened to overlap best --
  // which is a number, and a wrong one, where "no tempo" is the truth.
  if (analysis.flux.size() < 2 * longest) {
    return out;
  }

  // The flux with its mean removed.
  //
  // WITHOUT THIS THE CORRELATION IS DOMINATED BY THE DC TERM: a detection
  // function is non-negative, so every lag correlates strongly simply because
  // both copies are positive, and the periodic part is a ripple on top of a
  // large constant. Subtracting the mean is what makes the peak a peak.
  double mean = 0.0;
  for (const float value : analysis.flux) {
    mean += static_cast<double>(value);
  }
  mean /= static_cast<double>(analysis.flux.size());

  std::vector<double> centred(analysis.flux.size(), 0.0);
  double variance = 0.0;
  for (std::size_t index = 0; index < analysis.flux.size(); ++index) {
    centred[index] = static_cast<double>(analysis.flux[index]) - mean;
    variance += centred[index] * centred[index];
  }
  variance /= static_cast<double>(analysis.flux.size());

  // Correlation at every lag, out past the search range.
  //
  // PAST IT ON PURPOSE: the score below is a COMB, and a candidate's multiples
  // have to be available to comb over. A candidate at the top of the tempo range
  // has its second harmonic outside the range entirely.
  const std::size_t furthest = std::min(kCombPartials * longest, centred.size() / 2);
  std::vector<double> correlation(furthest + 1, 0.0);
  for (std::size_t lag = shortest; lag <= furthest; ++lag) {
    double sum = 0.0;
    const std::size_t overlap = centred.size() - lag;
    for (std::size_t index = 0; index < overlap; ++index) {
      sum += centred[index] * centred[index + lag];
    }
    // Normalised by the overlap, so a long lag is not penalised for having less
    // of the file to correlate over.
    correlation[lag] = sum / static_cast<double>(overlap);
  }

  double best_score = 0.0;
  double best_raw = 0.0;
  std::size_t best_lag = 0;
  std::size_t counted = 0;

  for (std::size_t lag = shortest; lag <= longest; ++lag) {
    // A COMB OVER THE MULTIPLES, which is what separates a beat from its own
    // subdivisions. Autocorrelation cannot tell 172 from 86 -- both are exactly
    // periodic in a 172 pulse train -- and a preference curve alone picks
    // whichever is nearer 120, which on this material was neither: measured raw
    // correlations of 0.0224 at 86 BPM, 0.0209 at the true 173 and 0.0203 at
    // 114.8, and the 114.8 won on preference because it sat closest to the
    // centre of the curve.
    //
    // The fundamental is the lag whose MULTIPLES also correlate. 86 has none
    // inside the file; 173 has 86 as its second. Summing over the multiples with
    // a 1/k weight rewards the one that explains the others.
    double score = 0.0;
    for (std::size_t partial = 1; partial <= kCombPartials; ++partial) {
      const std::size_t at = lag * partial;
      if (at > furthest) {
        break;
      }
      score += correlation[at] / static_cast<double>(partial);
    }

    ++counted;

    const double bpm = 60.0 / (static_cast<double>(lag) * frame_seconds);
    score *= preference(bpm);
    if (score > best_score) {
      best_score = score;
      best_raw = correlation[lag];
      best_lag = lag;
    }
  }

  if (best_lag == 0 || best_raw <= 0.0 || counted == 0) {
    return out;
  }

  const double bpm = 60.0 / (static_cast<double>(best_lag) * frame_seconds);

  out.bpm_x100 = static_cast<std::uint32_t>(std::lround(bpm * 100.0));
  out.bpm_x100 = std::clamp(out.bpm_x100, kMinBpmX100, kMaxBpmX100);

  // NORMALISED AGAINST THE VARIANCE, which is the correlation at lag zero. A
  // perfectly periodic signal correlates with itself at its period as strongly
  // as at zero, so this approaches 1; noise approaches 0.
  //
  // TWO EARLIER VERSIONS WERE MEANINGLESS. Dividing by the mean correlation gave
  // exactly 0 for tempi it had found correctly, because the flux is mean-centred
  // and the correlations average to nothing. Dividing by their RMS gave 1.94 for
  // a HELD SINE TONE -- a signal with no beat and almost no flux at all, where
  // the peak and the spread are both near zero and their ratio says nothing. The
  // variance is an absolute scale, so a signal with nothing in it cannot score
  // well by having a slightly less small number on top.
  out.confidence =
      variance > 0.0 ? static_cast<float>(std::clamp(best_raw / variance, 0.0, 1.0)) : 0.0F;
  return out;
}

}  // namespace ingest
