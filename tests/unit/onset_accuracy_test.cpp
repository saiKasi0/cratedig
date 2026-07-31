// The M3 acceptance criterion: "onset precision/recall >= 0.9 on labeled set".
//
// Three sets, with deliberately different provenance, because where the ground
// truth came from is what a precision/recall number actually means:
//
//   1. SYNTHETIC, labelled by construction. Hits placed at frames this file
//      chose, so the truth is a fact. Never skips -- the acceptance holds on a
//      machine with no fixtures and no network.
//
//   2. REAL RECORDED HITS, arranged by this file. Two CC0 kick recordings
//      placed at known times. The transients and spectra are real; the labels
//      are still exact, because we put them there. This is the closest thing to
//      real material whose ground truth is not somebody's opinion.
//
//   3. REAL FILES, labelled by INSPECTION, from the committed *.onsets.txt.
//      Two single-hit recordings. Small, but genuinely independent of the
//      detector.
//
// WHAT IS NOT HERE, and why. The CC0 percussion loops in the starter pack are
// dense sustained textures -- reverberant industrial noise, hand-percussion
// washes, tabla rolls -- not discrete drum patterns. Their amplitude envelopes
// were examined at millisecond resolution while writing this; none has isolated
// attacks that could be labelled with any confidence. Labelling them anyway
// would produce ground truth that could not be defended, and a precision/recall
// figure computed against it would be a number rather than a measurement. A
// real performance, labelled by a human with ears, remains the gap -- recorded
// in docs/TESTING.md rather than papered over here.

#include "ingest/decoder.hpp"
#include "ingest/onset.hpp"
#include "rt/sample.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::uint32_t kRate = 48'000;

// 25 ms either side. The standard tolerance in the onset-detection literature
// is 50 ms; half that is used here because a chop is a stricter application
// than "did you notice the note".
constexpr double kToleranceSeconds = 0.025;

struct Score {
  std::size_t true_positives = 0;
  std::size_t false_positives = 0;
  std::size_t false_negatives = 0;

  [[nodiscard]] double precision() const {
    const std::size_t reported = true_positives + false_positives;
    return reported == 0 ? 1.0
                         : static_cast<double>(true_positives) / static_cast<double>(reported);
  }

  [[nodiscard]] double recall() const {
    const std::size_t actual = true_positives + false_negatives;
    return actual == 0 ? 1.0 : static_cast<double>(true_positives) / static_cast<double>(actual);
  }
};

// Greedy one-to-one matching, nearest first.
//
// One-to-one matters: without it, a detector that reported fifty onsets on top
// of one true hit would score perfect recall, and two detections of the same
// hit would both count as correct.
[[nodiscard]] Score score(std::vector<double> detected, std::vector<double> truth,
                          double tolerance) {
  std::sort(detected.begin(), detected.end());
  std::sort(truth.begin(), truth.end());

  Score out;
  std::vector<bool> claimed(truth.size(), false);
  for (const double at : detected) {
    std::size_t best = truth.size();
    double best_distance = tolerance;
    for (std::size_t index = 0; index < truth.size(); ++index) {
      if (claimed[index]) {
        continue;
      }
      const double distance = std::abs(at - truth[index]);
      if (distance <= best_distance) {
        best_distance = distance;
        best = index;
      }
    }
    if (best < truth.size()) {
      claimed[best] = true;
      ++out.true_positives;
    } else {
      ++out.false_positives;
    }
  }
  out.false_negatives = static_cast<std::size_t>(std::count(claimed.begin(), claimed.end(), false));
  return out;
}

[[nodiscard]] std::vector<double> seconds_of(const ingest::OnsetResult& result,
                                             std::uint32_t rate) {
  std::vector<double> out;
  out.reserve(result.frames.size());
  for (const std::size_t frame : result.frames) {
    out.push_back(static_cast<double>(frame) / static_cast<double>(rate));
  }
  return out;
}

void add_hit(std::vector<float>& signal, std::size_t at, float amplitude, std::size_t decay_frames,
             std::uint32_t seed) {
  std::uint32_t state = seed;
  for (std::size_t offset = 0; offset < decay_frames && at + offset < signal.size(); ++offset) {
    state = (state * 1'664'525U) + 1'013'904'223U;
    const auto noise =
        (static_cast<float>(static_cast<std::int32_t>(state >> 8U) % 2'000) / 1'000.0F) - 1.0F;
    const float envelope = 1.0F - (static_cast<float>(offset) / static_cast<float>(decay_frames));
    signal[at + offset] += amplitude * envelope * envelope * noise;
  }
}

[[nodiscard]] rt::Sample sample_from(const std::vector<float>& mono) {
  rt::Sample sample{kRate, 1, mono.size()};
  std::copy(mono.begin(), mono.end(), sample.mutable_channel(0).begin());
  return sample;
}

[[nodiscard]] std::filesystem::path pack_path(const std::string& name) {
  return std::filesystem::path{CRATEDIG_STARTER_PACK_DIR} / name;
}

// Reads a committed *.onsets.txt: one time in seconds per line, '#' comments.
[[nodiscard]] std::vector<double> read_labels(const std::filesystem::path& path) {
  std::vector<double> out;
  std::ifstream file{path};
  std::string line;
  while (std::getline(file, line)) {
    const std::size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    out.push_back(std::stod(line.substr(first)));
  }
  return out;
}

}  // namespace

TEST_CASE("onset precision and recall on a synthetic labeled set", "[unit]") {
  // Never skips. This is the acceptance criterion; it must hold on a machine
  // with no fixtures and no network (docs/TESTING.md).
  //
  // Deliberately varied: four groups at different tempos, levels and decay
  // lengths, so a detector tuned to one spacing cannot score well by accident.
  std::vector<float> signal(static_cast<std::size_t>(kRate) * 10, 0.0F);
  std::vector<double> truth;

  struct Group {
    std::size_t start_ms;
    std::size_t spacing_ms;
    std::size_t count;
    float amplitude;
    std::size_t decay_ms;
  };

  constexpr Group kGroups[] = {
      {250, 250, 8, 0.80F, 40},     // slow and loud
      {2'500, 120, 12, 0.45F, 30},  // faster, quieter
      {4'200, 93, 16, 0.70F, 20},   // 16ths at 160 bpm
      {6'500, 400, 6, 0.25F, 90},   // slow, soft, long decay
  };

  std::uint32_t seed = 1'000;
  for (const Group& group : kGroups) {
    for (std::size_t hit = 0; hit < group.count; ++hit) {
      const std::size_t at = ((group.start_ms + (hit * group.spacing_ms)) * kRate) / 1'000;
      if (at >= signal.size()) {
        break;
      }
      truth.push_back(static_cast<double>(at) / static_cast<double>(kRate));
      add_hit(signal, at, group.amplitude, (group.decay_ms * kRate) / 1'000, seed++);
    }
  }

  const rt::Sample sample = sample_from(signal);
  const ingest::OnsetResult result = ingest::detect_onsets(sample);
  const Score scored = score(seconds_of(result, kRate), truth, kToleranceSeconds);

  INFO("truth " << truth.size() << ", detected " << result.frames.size() << " -- TP "
                << scored.true_positives << ", FP " << scored.false_positives << ", FN "
                << scored.false_negatives << ", precision " << scored.precision() << ", recall "
                << scored.recall());

  CHECK(scored.precision() >= 0.9);
  CHECK(scored.recall() >= 0.9);
}

TEST_CASE("onset precision and recall on real recorded hits", "[fixture]") {
  // Real transients and real spectra, arranged by this test so the labels are
  // still exact. Between the synthetic set and a labelled performance, and
  // honest about being so.
  const std::filesystem::path kick = pack_path("drum_heavy_kick.flac");
  const std::filesystem::path haus = pack_path("bd_haus.flac");
  if (!std::filesystem::exists(kick) || !std::filesystem::exists(haus)) {
    SKIP("missing starter pack — run scripts/fetch_starter_pack.sh");
  }

  const ingest::SampleLoad kick_load = ingest::load_sample(kick, kRate);
  const ingest::SampleLoad haus_load = ingest::load_sample(haus, kRate);
  REQUIRE(kick_load.ok());
  REQUIRE(haus_load.ok());

  // The two files' own onsets, from their committed labels, so the arrangement
  // below places the ATTACK at each chosen time rather than the file's first
  // sample -- bd_haus opens with 5 ms of inaudible pre-ring.
  const std::vector<double> kick_labels = read_labels(pack_path("drum_heavy_kick.onsets.txt"));
  const std::vector<double> haus_labels = read_labels(pack_path("bd_haus.onsets.txt"));
  REQUIRE(kick_labels.size() == 1);
  REQUIRE(haus_labels.size() == 1);

  std::vector<float> signal(static_cast<std::size_t>(kRate) * 8, 0.0F);
  std::vector<double> truth;

  // A pattern with a real feel to it: a kick on the beat and a second hit on
  // the offbeat, sixteen bars of it, at varying levels.
  constexpr std::size_t kBeatMs = 500;
  for (std::size_t beat = 0; beat < 14; ++beat) {
    const bool offbeat = beat % 2 == 1;
    const ingest::SampleLoad& source = offbeat ? haus_load : kick_load;
    const double own_onset = offbeat ? haus_labels[0] : kick_labels[0];
    const float level = offbeat ? 0.5F : 0.9F;

    const std::size_t attack_at = ((beat + 1) * kBeatMs * kRate) / 1'000;
    const auto own_offset = static_cast<std::size_t>(own_onset * kRate);
    if (attack_at < own_offset) {
      continue;
    }
    const std::size_t place_at = attack_at - own_offset;

    const std::span<const float> data = source.sample->channel(0);
    for (std::size_t frame = 0; frame < data.size(); ++frame) {
      if (place_at + frame >= signal.size()) {
        break;
      }
      signal[place_at + frame] += level * data[frame];
    }
    truth.push_back(static_cast<double>(attack_at) / static_cast<double>(kRate));
  }

  const rt::Sample sample = sample_from(signal);
  const ingest::OnsetResult result = ingest::detect_onsets(sample);
  const Score scored = score(seconds_of(result, kRate), truth, kToleranceSeconds);

  INFO("truth " << truth.size() << ", detected " << result.frames.size() << " -- TP "
                << scored.true_positives << ", FP " << scored.false_positives << ", FN "
                << scored.false_negatives << ", precision " << scored.precision() << ", recall "
                << scored.recall());

  CHECK(scored.precision() >= 0.9);
  CHECK(scored.recall() >= 0.9);
}

TEST_CASE("onsets match the committed labels for single-hit recordings", "[fixture]") {
  // The only genuinely inspection-labelled ground truth in the pack. Small, and
  // it still catches something the synthetic sets cannot: a real kick's decay
  // must not produce a SECOND onset, and bd_haus's inaudible pre-ring must not
  // be mistaken for its attack.
  struct Case {
    const char* audio;
    const char* labels;
  };

  constexpr Case kCases[] = {
      {"drum_heavy_kick.flac", "drum_heavy_kick.onsets.txt"},
      {"bd_haus.flac", "bd_haus.onsets.txt"},
  };

  for (const Case& item : kCases) {
    const std::filesystem::path audio = pack_path(item.audio);
    if (!std::filesystem::exists(audio)) {
      SKIP("missing starter pack — run scripts/fetch_starter_pack.sh");
    }

    const ingest::SampleLoad load = ingest::load_sample(audio, kRate);
    REQUIRE(load.ok());
    const std::vector<double> truth = read_labels(pack_path(item.labels));
    REQUIRE_FALSE(truth.empty());

    const ingest::OnsetResult result = ingest::detect_onsets(*load.sample);
    const std::vector<double> detected = seconds_of(result, kRate);
    const Score scored = score(detected, truth, kToleranceSeconds);

    INFO(item.audio << ": truth " << truth.size() << ", detected " << detected.size()
                    << ", first at " << (detected.empty() ? -1.0 : detected.front())
                    << " s -- precision " << scored.precision() << ", recall " << scored.recall());

    CHECK(scored.recall() >= 0.9);
    CHECK(scored.precision() >= 0.9);
  }
}
