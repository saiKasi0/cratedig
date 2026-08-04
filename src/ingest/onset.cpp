#include "ingest/onset.hpp"

#include "ingest/fft.hpp"
#include "ingest/window.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ingest {
namespace {

constexpr std::size_t kWindowSize = Fft::kSize;
constexpr std::size_t kBins = Fft::kBins;

// Built once, at compile time, in .rodata (see window.hpp).
constexpr std::array<float, kWindowSize> kWindow = hann_window<kWindowSize>();

// Onset detection is about WHEN, not about stereo image, so everything runs on
// a mono sum. Averaging rather than summing keeps the result in the same range
// as the source whatever the channel count, which is what lets the normalised
// flux thresholds mean the same thing for mono and stereo material.
[[nodiscard]] std::vector<float> downmix(const rt::Sample& sample) {
  std::vector<float> mono(sample.num_frames(), 0.0F);
  const std::uint16_t channels = sample.num_channels();
  if (channels == 0) {
    return mono;
  }
  for (std::uint16_t channel = 0; channel < channels; ++channel) {
    const std::span<const float> data = sample.channel(channel);
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
      mono[frame] += data[frame];
    }
  }
  const float scale = 1.0F / static_cast<float>(channels);
  for (float& value : mono) {
    value *= scale;
  }
  return mono;
}

// Copies one analysis window out of `mono`, CENTRED on `centre`, zero-padded
// where it runs off either end, and applies the Hann window.
//
// Centred rather than left- or right-aligned. Left-aligned reports an onset as
// much as a full window early, because flux rises the moment any of the
// transient enters the window. Right-aligned puts the newest samples where the
// Hann taper is heading to zero, which is where sensitivity goes to die.
// Centring splits the difference at about one hop early, which is what the
// backtracking below is for.
void fill_window(const std::vector<float>& mono, std::size_t centre, std::span<float> out) {
  const auto half = static_cast<std::ptrdiff_t>(kWindowSize / 2);
  const auto start = static_cast<std::ptrdiff_t>(centre) - half;
  const auto total = static_cast<std::ptrdiff_t>(mono.size());

  for (std::size_t index = 0; index < kWindowSize; ++index) {
    const std::ptrdiff_t source = start + static_cast<std::ptrdiff_t>(index);
    const float value =
        (source >= 0 && source < total) ? mono[static_cast<std::size_t>(source)] : 0.0F;
    out[index] = value * kWindow[index];
  }
}

// The median of a window of the detection function.
//
// nth_element rather than a full sort: only the middle value is wanted, and
// this runs once per analysis frame. Copies into scratch because nth_element
// reorders, and reordering the flux would corrupt every later window.
[[nodiscard]] float local_median(const std::vector<float>& flux, std::size_t centre,
                                 std::size_t radius, std::vector<float>& scratch) {
  const std::size_t first = centre > radius ? centre - radius : 0;
  const std::size_t last = std::min(centre + radius + 1, flux.size());
  scratch.assign(flux.begin() + static_cast<std::ptrdiff_t>(first),
                 flux.begin() + static_cast<std::ptrdiff_t>(last));
  if (scratch.empty()) {
    return 0.0F;
  }
  const std::size_t middle = scratch.size() / 2;
  std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(middle),
                   scratch.end());
  return scratch[middle];
}

// Walks back from a detected peak to the nearest preceding local minimum of the
// detection function.
//
// This is the difference between a slice that starts on the transient and one
// that starts a few milliseconds into it. Bounded, so a slowly rising passage
// cannot drag a detection arbitrarily far back into the previous hit.
// The comparison is STRICTLY less-than, and that is the whole function.
//
// With <=, a flat run counts as descending, so the walk crosses the silence in
// front of a hit and keeps going until it hits the step limit. Measured: every
// detection landed exactly `limit` hops early -- 42 ms at the defaults, four
// times the tolerance anyone would accept. With <, the walk stops the moment
// the flux stops falling, which is the foot of the rise.
[[nodiscard]] std::size_t backtrack_to_minimum(const std::vector<float>& flux, std::size_t peak,
                                               std::size_t limit) {
  std::size_t index = peak;
  std::size_t steps = 0;
  while (index > 0 && steps < limit && flux[index - 1] < flux[index]) {
    --index;
    ++steps;
  }
  return index;
}

// Sample-resolution refinement, from the foot of the flux rise forward to where
// the waveform actually starts moving.
//
// Backtracking alone leaves the position ~10-15 ms early, because a centred
// 1024-point window starts seeing a transient half a window before its frame
// time. Early is the safe direction -- but 15 ms of the PREVIOUS hit's tail at
// the head of every slice is audible, and it is why chopped drums often sound
// slightly smeared.
//
// The attack must lie between the foot of the rise and the flux peak, so the
// search is bounded by both. The trigger is a fraction of the local peak rather
// than an absolute level, so it means the same thing for a quiet hi-hat and a
// loud kick.
[[nodiscard]] std::size_t refine_attack(const std::vector<float>& mono, std::size_t from,
                                        std::size_t until) {
  const std::size_t last = std::min(until, mono.size());
  if (last <= from) {
    return from;
  }

  float loudest = 0.0F;
  for (std::size_t index = from; index < last; ++index) {
    loudest = std::max(loudest, std::abs(mono[index]));
  }
  if (loudest <= 0.0F) {
    return from;
  }

  // -20 dB of the local peak. Low enough to catch the very start of a transient,
  // high enough that the noise floor in front of it does not trigger.
  const float threshold = loudest * 0.1F;
  for (std::size_t index = from; index < last; ++index) {
    if (std::abs(mono[index]) >= threshold) {
      return index;
    }
  }
  // Nothing crossed -- which happens when the previous hit is still ringing
  // above the threshold across the whole region. The backtracked position is
  // then already the best answer available.
  return from;
}

}  // namespace

OnsetParams params_for(ChopDensity density, std::uint32_t bpm_x100) noexcept {
  OnsetParams params;  // kStrum is the struct's own defaults
  if (density == ChopDensity::kStrum || bpm_x100 == 0) {
    return params;
  }

  // 60 / (bpm_x100 / 100).
  const double beat_seconds = 6'000.0 / static_cast<double>(bpm_x100);

  // HALF A BEAT FOR "A SLICE PER BEAT", AND THAT IS NOT A ROUNDING.
  //
  // min_gap is a MINIMUM SPACING, not a grid: it says how close two slices may
  // be, not where they go. Set to a whole beat it merges any pair straddling a
  // beat line -- and real playing is never exactly on the line -- so it loses
  // real hits rather than thinning them.
  //
  // Measured on a real record at ~172 BPM (beat 0.349 s), against 2.87 beats a
  // second: a gap of 0.250 s gave 2.17 slices/s and 0.175 s gave 2.67/s. Half a
  // beat lands nearest one-per-beat, because not every beat carries a detectable
  // attack and the ones that do sometimes carry two.
  constexpr double kBeatFraction = 0.5;

  params.min_gap_seconds =
      beat_seconds * kBeatFraction * (density == ChopDensity::kBar ? kBeatsPerBar : 1);

  // And less willing, at the coarser setting. A bar-length chop that still fired
  // on every weak attack would be a bar-length chop of whatever happened to be
  // more than a bar apart, rather than of the strong hits.
  if (density == ChopDensity::kBar) {
    params.threshold_lambda = 2.6F;
  }
  return params;
}

bool density_from_name(std::string_view name, ChopDensity& out) noexcept {
  if (name == "strum") {
    out = ChopDensity::kStrum;
    return true;
  }
  if (name == "beat") {
    out = ChopDensity::kBeat;
    return true;
  }
  if (name == "bar") {
    out = ChopDensity::kBar;
    return true;
  }
  return false;
}

std::string_view name_of(ChopDensity density) noexcept {
  switch (density) {
    case ChopDensity::kBeat:
      return "beat";
    case ChopDensity::kBar:
      return "bar";
    case ChopDensity::kStrum:
      break;
  }
  return "strum";
}

OnsetAnalysis analyse_onsets(const rt::Sample& sample, const OnsetParams& params) {
  OnsetAnalysis analysis;
  analysis.hop = params.hop;
  analysis.sample_rate = sample.sample_rate();
  if (sample.empty() || params.hop == 0) {
    return analysis;
  }

  analysis.mono = downmix(sample);
  const std::vector<float>& mono = analysis.mono;
  const std::size_t num_analysis = (mono.size() / params.hop) + 1;

  const Fft fft;
  std::vector<float> windowed(kWindowSize, 0.0F);
  std::vector<float> previous(kBins, 0.0F);
  std::vector<float> current(kBins, 0.0F);

  analysis.flux.assign(num_analysis, 0.0F);

  for (std::size_t frame = 0; frame < num_analysis; ++frame) {
    fill_window(mono, frame * params.hop, windowed);
    fft.magnitude_spectrum(windowed, current);

    // HALF-WAVE RECTIFIED: only bins that got LOUDER count. Energy leaving a
    // band is a note ending, and a detector that counted it would fire twice
    // per hit -- once at the attack and once at the decay.
    //
    // ON LOG MAGNITUDES, which is not cosmetic. A real kick's decay is a
    // pitch-falling sine, so as its fundamental sweeps downward it keeps
    // pushing energy into bins that were quiet -- and on linear magnitudes
    // those rises are large enough to look like attacks. Measured on a pattern
    // built from two real CC0 kick recordings: 43 detections for 14 hits, all
    // 29 extras inside the decays at consistent offsets. On log magnitudes: 14
    // for 14, none spurious.
    //
    // The reason it works is the shape of the compression. A hit is a rise
    // across the WHOLE spectrum at once; a pitch sweep is a large rise in a few
    // bins. log() flattens the large narrow rise and preserves the small broad
    // one, so the two stop being confusable. log1p rather than log because bins
    // are legitimately zero and log(0) is not.
    //
    // Bins below `first_bin` do not count at all. See OnsetParams::hf_emphasis.
    const float emphasis = std::clamp(params.hf_emphasis, 0.0F, kMaxHfEmphasis);
    const auto first_bin = static_cast<std::size_t>(emphasis * static_cast<float>(kBins));

    float sum = 0.0F;
    for (std::size_t bin = first_bin; bin < kBins; ++bin) {
      const float rise = std::log1p(current[bin]) - std::log1p(previous[bin]);
      if (rise > 0.0F) {
        sum += rise;
      }
    }
    // Frame 0 has no predecessor, so its "rise" is the whole spectrum appearing
    // out of nothing. Left at zero rather than reported as an enormous onset.
    analysis.flux[frame] = frame == 0 ? 0.0F : sum;
    previous.swap(current);
  }

  // Normalised so the parameters are properties of the ALGORITHM rather than of
  // the recording level. Without this, threshold_delta would have to be
  // re-chosen for every file.
  const float loudest = *std::max_element(analysis.flux.begin(), analysis.flux.end());
  if (loudest > 0.0F) {
    const float scale = 1.0F / loudest;
    for (float& value : analysis.flux) {
      value *= scale;
    }
  }
  return analysis;
}

OnsetResult pick_onsets(const OnsetAnalysis& analysis, const OnsetParams& params) {
  OnsetResult result;
  result.hop = analysis.hop;
  if (analysis.flux.empty() || analysis.hop == 0) {
    return result;
  }

  const std::vector<float>& mono = analysis.mono;
  const std::size_t num_analysis = analysis.flux.size();
  result.flux = analysis.flux;

  result.threshold.assign(num_analysis, 0.0F);
  std::vector<float> scratch;
  scratch.reserve((2 * params.median_radius) + 1);
  for (std::size_t frame = 0; frame < num_analysis; ++frame) {
    const float median = local_median(result.flux, frame, params.median_radius, scratch);
    result.threshold[frame] = params.threshold_delta + (params.threshold_lambda * median);
  }

  const auto min_gap_frames =
      static_cast<std::size_t>(params.min_gap_seconds * static_cast<double>(analysis.sample_rate));

  // Peak picking. A detection has to be all three of: a strict local maximum,
  // above the local threshold, and far enough from the previous detection.
  bool have_previous = false;
  std::size_t previous_onset = 0;
  for (std::size_t frame = 1; frame + 1 < num_analysis; ++frame) {
    const float value = result.flux[frame];
    if (value <= result.threshold[frame]) {
      continue;
    }
    if (value < result.flux[frame - 1] || value < result.flux[frame + 1]) {
      continue;
    }
    // Ties go to the earlier frame, so a plateau produces one detection at its
    // start rather than one at each end.
    if (value == result.flux[frame - 1]) {
      continue;
    }

    std::size_t position = std::min(frame * analysis.hop, mono.size() - 1);
    if (params.backtrack) {
      const std::size_t foot = backtrack_to_minimum(result.flux, frame, params.median_radius);

      // The search runs HALF A WINDOW PAST the flux peak, not up to it. A
      // centred window starts seeing a transient half a window before its frame
      // time, so the flux can peak before the attack itself -- and bounding the
      // search at the peak then excludes the very sample being looked for.
      // Measured: with the tighter bound, five of eight hits were refined
      // exactly and the other three were not refined at all.
      //
      // Half a window is 10.7 ms at the defaults, comfortably inside the 30 ms
      // refractory period, so this can never wander into the next hit.
      const std::size_t limit = position + (kWindowSize / 2);
      position = refine_attack(mono, foot * analysis.hop, limit);
    }

    if (have_previous && position < previous_onset + min_gap_frames) {
      continue;
    }
    result.frames.push_back(position);
    previous_onset = position;
    have_previous = true;
  }

  return result;
}

OnsetResult detect_onsets(const rt::Sample& sample, const OnsetParams& params) {
  return pick_onsets(analyse_onsets(sample, params), params);
}

}  // namespace ingest
