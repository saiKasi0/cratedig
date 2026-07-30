#ifndef CRATEDIG_ENGINE_ENGINE_HPP
#define CRATEDIG_ENGINE_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace engine {

// The engine facade. Everything audible eventually happens behind render().
//
// This header must never see a device API. RtAudio and RtMidi live in src/io/
// and are the only place allowed to include their headers (CLAUDE.md). Keeping
// the engine device-free is what makes offline bounce, e2e tests, and Docker CI
// possible: render() works in a plain loop with no audio hardware present.
//
// M0 renders silence. The structure that matters now is the contract:
// allocation-free, deterministic, and block-size invariant.
class Engine {
 public:
  struct Config {
    std::uint32_t sample_rate = 48'000;
    std::uint16_t num_channels = 2;
    std::uint32_t max_block_frames = 2'048;

    // Anchors the determinism contract (docs/TESTING.md). Nothing consumes it
    // until the first stochastic element lands — humanize, noise, dither in M6 —
    // but it is part of the signature from the start so that "same seed, same
    // input, same bytes" is a promise the API always made, not one retrofitted
    // after something started varying.
    std::uint64_t seed = 0;
  };

  explicit Engine(const Config& config) noexcept;

  // REAL-TIME. Called from the audio callback; opens its own RT_SCOPE.
  //
  // channels is a span of per-channel pointers (the CLAUDE.md `float** out`
  // reconciled with the std::span rule) — channels.size() must equal
  // config.num_channels, and each pointer must address at least num_frames
  // floats. num_frames must not exceed config.max_block_frames. All three are
  // debug-asserted; in release they are the caller's contract.
  void render(std::span<float* const> channels, std::size_t num_frames) noexcept;

  [[nodiscard]] std::uint64_t frames_rendered() const noexcept { return m_frames_rendered; }

  [[nodiscard]] const Config& config() const noexcept { return m_config; }

 private:
  Config m_config;
  std::uint64_t m_frames_rendered = 0;
};

}  // namespace engine

#endif  // CRATEDIG_ENGINE_ENGINE_HPP
