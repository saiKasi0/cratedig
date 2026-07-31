#include "ingest/fft.hpp"

#include <pffft.h>

#include <cassert>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>

namespace ingest {
namespace {

// PFFFT's SIMD paths require 16-byte alignment and give no diagnostic when they
// do not get it -- the results are merely wrong in a way that looks like a bug
// in whatever consumes them.
struct AlignedFree {
  void operator()(float* pointer) const noexcept {
    if (pointer != nullptr) {
      pffft_aligned_free(pointer);
    }
  }
};

}  // namespace

struct Fft::Impl {
  PFFFT_Setup* setup = nullptr;
  std::unique_ptr<float[], AlignedFree> input;
  std::unique_ptr<float[], AlignedFree> output;
  std::unique_ptr<float[], AlignedFree> work;

  Impl()
      : setup(pffft_new_setup(static_cast<int>(Fft::kSize), PFFFT_REAL)),
        input(static_cast<float*>(pffft_aligned_malloc(Fft::kSize * sizeof(float)))),
        output(static_cast<float*>(pffft_aligned_malloc(Fft::kSize * sizeof(float)))),
        work(static_cast<float*>(pffft_aligned_malloc(Fft::kSize * sizeof(float)))) {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  ~Impl() {
    if (setup != nullptr) {
      pffft_destroy_setup(setup);
    }
  }
};

Fft::Fft() : m_impl(std::make_unique<Impl>()) {}

Fft::~Fft() = default;
Fft::Fft(Fft&&) noexcept = default;
Fft& Fft::operator=(Fft&&) noexcept = default;

void Fft::magnitude_spectrum(std::span<const float> input, std::span<float> magnitudes) const {
  assert(input.size() == kSize && "Fft: input must be exactly kSize frames");
  assert(magnitudes.size() == kBins && "Fft: output must be exactly kBins");
  if (input.size() != kSize || magnitudes.size() != kBins || m_impl->setup == nullptr) {
    return;
  }

  float* scratch_in = m_impl->input.get();
  for (std::size_t index = 0; index < kSize; ++index) {
    scratch_in[index] = input[index];
  }

  // PFFFT_FORWARD with PFFFT_REAL writes N floats representing N/2 complex
  // bins, in an order pffft_transform_ordered normalises for us -- worth the
  // extra pass, because the unordered layout is internal and undocumented
  // beyond "do not interpret it".
  pffft_transform_ordered(m_impl->setup, scratch_in, m_impl->output.get(), m_impl->work.get(),
                          PFFFT_FORWARD);

  const float* spectrum = m_impl->output.get();

  // The packing that makes this wrapper worth having. DC and Nyquist are both
  // purely real, so rather than storing two zero imaginary parts PFFFT puts
  // Nyquist's real part in the slot where DC's imaginary part would go. Reading
  // this as a plain interleaved array folds Nyquist's energy into bin 0.
  magnitudes[0] = std::abs(spectrum[0]);
  magnitudes[kBins - 1] = std::abs(spectrum[1]);

  for (std::size_t bin = 1; bin + 1 < kBins; ++bin) {
    const float real = spectrum[2 * bin];
    const float imaginary = spectrum[(2 * bin) + 1];
    magnitudes[bin] = std::sqrt((real * real) + (imaginary * imaginary));
  }
}

}  // namespace ingest
