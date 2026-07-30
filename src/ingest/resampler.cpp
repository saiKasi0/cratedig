#include "ingest/resampler.hpp"

#include <samplerate.h>

#include <cmath>
#include <cstddef>
#include <vector>

namespace ingest {
namespace {

// SRC_SINC_BEST_QUALITY: this runs once per file on a worker thread, so the
// difference between "best" and "fastest" is milliseconds at load time and a
// permanent difference in what every subsequent playback sounds like.
constexpr int kConverterType = SRC_SINC_BEST_QUALITY;

}  // namespace

ResampleResult resample(const PlanarAudio& input, std::uint32_t input_rate,
                        std::uint32_t output_rate) {
  ResampleResult result;

  if (input.empty() || input.front().empty()) {
    result.status = ResampleStatus::kNoInput;
    return result;
  }
  if (input_rate == 0 || output_rate == 0) {
    result.status = ResampleStatus::kBadRate;
    return result;
  }

  // Unity ratio short-circuit. See the header: running a sinc converter at 1.0
  // would still filter, so this is a correctness requirement rather than a
  // shortcut.
  if (input_rate == output_rate) {
    result.channels = input;
    return result;
  }

  const std::size_t channel_count = input.size();
  const std::size_t input_frames = input.front().size();
  const double ratio = static_cast<double>(output_rate) / static_cast<double>(input_rate);

  // libsamplerate speaks interleaved, so the planar data is woven together and
  // taken apart again around the call. Both loops are worker-side.
  std::vector<float> interleaved(input_frames * channel_count);
  for (std::size_t channel = 0; channel < channel_count; ++channel) {
    const std::vector<float>& source = input[channel];
    for (std::size_t frame = 0; frame < input_frames; ++frame) {
      interleaved[(frame * channel_count) + channel] = source[frame];
    }
  }

  // Rounding up and adding a small margin: src_simple reports how many frames it
  // actually produced, and a buffer one frame short would silently truncate.
  const auto estimated =
      static_cast<std::size_t>(std::ceil(static_cast<double>(input_frames) * ratio)) + 16;
  std::vector<float> converted(estimated * channel_count);

  SRC_DATA data{};
  data.data_in = interleaved.data();
  data.data_out = converted.data();
  data.input_frames = static_cast<long>(input_frames);
  data.output_frames = static_cast<long>(estimated);
  data.src_ratio = ratio;
  data.end_of_input = 1;

  const int error = src_simple(&data, kConverterType, static_cast<int>(channel_count));
  if (error != 0) {
    result.status = ResampleStatus::kConverterFailed;
    result.library_error = error;
    return result;
  }

  const auto output_frames = static_cast<std::size_t>(data.output_frames_gen);
  result.channels.assign(channel_count, std::vector<float>(output_frames, 0.0F));
  for (std::size_t channel = 0; channel < channel_count; ++channel) {
    std::vector<float>& destination = result.channels[channel];
    for (std::size_t frame = 0; frame < output_frames; ++frame) {
      destination[frame] = converted[(frame * channel_count) + channel];
    }
  }
  return result;
}

}  // namespace ingest
