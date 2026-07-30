#ifndef CRATEDIG_INGEST_DECODER_HPP
#define CRATEDIG_INGEST_DECODER_HPP

#include "rt/sample.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace ingest {

// Decoding lives entirely on worker threads. It allocates, it does file I/O, and
// it can fail — none of which is permitted anywhere near the audio callback. The
// only thing that crosses into the real-time lane is a finished, immutable
// rt::Sample behind a shared_ptr.

enum class DecodeError : std::uint8_t {
  kNone = 0,
  kFileNotFound,
  kOpenFailed,
  kNoAudioStream,
  kNoDecoder,
  kDecoderOpenFailed,
  kDecodeFailed,
  kResamplerFailed,
  kEmptyStream,
  kUnsupportedChannelCount,
};

[[nodiscard]] std::string_view describe(DecodeError error) noexcept;

// The outcome of a load.
//
// Deliberately not rt::Result<T, E>. That type is constrained to trivially
// copyable payloads because it exists for the exception-free real-time lane,
// where an error is a small enum and there is nothing sensible to say about it.
// Here the useful part of a failure is usually FFmpeg's own message — "No such
// file or directory", "Invalid data found when processing input" — which a
// caller wants to put in front of a user verbatim. Widening rt::Result to carry
// a std::string would drag allocation into a header the audio thread includes,
// to serve a caller that is not on that thread.
struct SampleLoad {
  // Non-null exactly when the load succeeded.
  std::shared_ptr<const rt::Sample> sample;
  DecodeError error = DecodeError::kNone;

  // FFmpeg's description of what went wrong, when it had one. Empty on success.
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return sample != nullptr; }
};

// Decodes `path` and returns a Sample at `target_sample_rate`.
//
// Resampling happens here, once, at load — not per frame during playback. That
// keeps the voice interpolator responsible only for pitch, lets the high-quality
// sinc converter run where it costs nothing, and means a sample already at the
// engine's rate plays back bit-exactly (the interpolator's fraction is always
// zero). Passing target_sample_rate == 0 skips resampling and keeps the file's
// native rate.
[[nodiscard]] SampleLoad load_sample(const std::filesystem::path& path,
                                     std::uint32_t target_sample_rate);

// The license and configure line of the FFmpeg build actually linked at runtime.
//
// docs/LICENSING.md scopes the LGPL requirement to distributed binaries, and
// CRATEDIG_REQUIRE_LGPL_FFMPEG enforces it at configure time. This is the other
// half: `cratedig --version` prints it, so which build is in play is observable
// rather than assumed.
[[nodiscard]] std::string_view ffmpeg_license() noexcept;
[[nodiscard]] std::string_view ffmpeg_configuration() noexcept;
[[nodiscard]] std::string ffmpeg_versions();

}  // namespace ingest

#endif  // CRATEDIG_INGEST_DECODER_HPP
