#ifndef CRATEDIG_INGEST_RESAMPLER_HPP
#define CRATEDIG_INGEST_RESAMPLER_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ingest {

// Sample-rate conversion for whole buffers, on a worker thread.
//
// libsamplerate rather than swresample, even though FFmpeg is already linked and
// could do it in the same pass. Two reasons, both about determinism: the
// resampler's output is baked into every golden hash in the suite, and FFmpeg is
// a *system* library whose version varies between a contributor's machine
// (Homebrew 8.1 here) and CI (Ubuntu 24.04 ships 6.1). A pinned, statically
// linked libsamplerate produces the same bytes everywhere; swresample would make
// the goldens a property of whatever the distro shipped that month.

// Planar audio: one vector per channel, all the same length.
using PlanarAudio = std::vector<std::vector<float>>;

enum class ResampleStatus : std::uint8_t {
  kOk = 0,
  kNoInput,
  kBadRate,
  kConverterFailed,
};

struct ResampleResult {
  PlanarAudio channels;
  ResampleStatus status = ResampleStatus::kOk;
  int library_error = 0;

  [[nodiscard]] bool ok() const noexcept { return status == ResampleStatus::kOk; }
};

// Converts `input` from `input_rate` to `output_rate`.
//
// When the rates already match this returns the input unchanged, byte for byte,
// rather than running it through the converter at ratio 1.0. That is not just an
// optimisation: a sinc converter at unity ratio still filters, so a 48 kHz file
// played on a 48 kHz engine would come back subtly altered. Short-circuiting is
// what makes "load a file at the engine's rate and play it" bit-exact.
[[nodiscard]] ResampleResult resample(const PlanarAudio& input, std::uint32_t input_rate,
                                      std::uint32_t output_rate);

}  // namespace ingest

#endif  // CRATEDIG_INGEST_RESAMPLER_HPP
