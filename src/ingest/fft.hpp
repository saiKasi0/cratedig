#ifndef CRATEDIG_INGEST_FFT_HPP
#define CRATEDIG_INGEST_FFT_HPP

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace ingest {

// A real-input FFT, for onset detection.
//
// WORKER THREAD ONLY. Construction allocates, and PFFFT's own scratch buffers
// are allocated once here rather than per transform -- but nothing about this
// type is real-time safe and nothing on the audio path may touch it. Analysis
// happens on a worker and publishes results; that is the whole shape of
// src/ingest/.
//
// Wrapping PFFFT rather than using it directly, for three reasons that all bit
// during the first attempt at this:
//
//   1. Its buffers must be 16-byte aligned or the SIMD paths read across the
//      alignment boundary. pffft_aligned_malloc exists for this, and forgetting
//      it produces results that are *nearly* right.
//   2. PFFFT_REAL output is NOT the layout anyone expects: it is N/2 complex
//      bins packed into N floats, with DC's imaginary part and Nyquist's real
//      part sharing slot [1] because both imaginary parts are known to be zero.
//      Reading it as an ordinary interleaved array silently mixes DC into the
//      wrong bin.
//   3. A PFFFT_Setup is expensive and reusable. Building one per frame of a
//      five-minute file would dominate the analysis.
//
// So this exposes exactly one operation -- windowed real input to magnitude
// spectrum -- and keeps the layout question inside.
class Fft {
 public:
  // PFFFT requires real transform sizes to be a multiple of 32. 1024 at 48 kHz
  // is a 21 ms window: long enough to resolve a kick's fundamental, short
  // enough that two hits 30 ms apart do not share one frame.
  static constexpr std::size_t kSize = 1'024;
  static constexpr std::size_t kBins = (kSize / 2) + 1;

  Fft();
  ~Fft();

  Fft(const Fft&) = delete;
  Fft& operator=(const Fft&) = delete;
  Fft(Fft&&) noexcept;
  Fft& operator=(Fft&&) noexcept;

  // Transforms kSize real samples and writes kBins magnitudes into `magnitudes`.
  //
  // Magnitudes, not powers and not decibels: spectral flux is a difference of
  // magnitudes, and taking a square root here once is cheaper than the caller
  // discovering it needed one. Bin 0 is DC and bin kBins-1 is Nyquist.
  void magnitude_spectrum(std::span<const float> input, std::span<float> magnitudes) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace ingest

#endif  // CRATEDIG_INGEST_FFT_HPP
