// The PFFFT wrapper.
//
// What is actually being checked is the LAYOUT, not the arithmetic. PFFFT is a
// well-tested FFT; the thing that goes wrong when wrapping it is the packing of
// its real-transform output, where DC's imaginary part and Nyquist's real part
// share one slot because both imaginary parts are known to be zero. Read as an
// ordinary interleaved array, Nyquist's energy folds into bin 0 and everything
// still looks plausible.
//
// So every case here puts energy in a known bin and asserts it came out of that
// bin and no other.

#include "ingest/fft.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::size_t kSize = ingest::Fft::kSize;
constexpr std::size_t kBins = ingest::Fft::kBins;

// A cosine at exactly `bin` cycles per window, so it lands in one bin with no
// leakage into its neighbours at all.
[[nodiscard]] std::vector<float> bin_centred_cosine(std::size_t bin, float amplitude = 1.0F) {
  std::vector<float> signal(kSize, 0.0F);
  for (std::size_t index = 0; index < kSize; ++index) {
    const double phase = 2.0 * std::numbers::pi * static_cast<double>(bin) *
                         static_cast<double>(index) / static_cast<double>(kSize);
    signal[index] = amplitude * static_cast<float>(std::cos(phase));
  }
  return signal;
}

// Which bin holds the most energy, and how much the runner-up has.
struct Peak {
  std::size_t bin = 0;
  float magnitude = 0.0F;
  float next_largest = 0.0F;
};

[[nodiscard]] Peak find_peak(const std::vector<float>& magnitudes) {
  Peak peak;
  for (std::size_t bin = 0; bin < magnitudes.size(); ++bin) {
    if (magnitudes[bin] > peak.magnitude) {
      peak.next_largest = peak.magnitude;
      peak.magnitude = magnitudes[bin];
      peak.bin = bin;
    } else if (magnitudes[bin] > peak.next_largest) {
      peak.next_largest = magnitudes[bin];
    }
  }
  return peak;
}

}  // namespace

TEST_CASE("Fft puts a bin-centred tone in exactly that bin", "[unit]") {
  const ingest::Fft fft;
  std::vector<float> magnitudes(kBins, 0.0F);

  for (const std::size_t bin : {1U, 7U, 64U, 256U, 511U}) {
    const std::vector<float> signal = bin_centred_cosine(bin);
    fft.magnitude_spectrum(signal, magnitudes);

    const Peak peak = find_peak(magnitudes);
    INFO("expected bin " << bin << ", got " << peak.bin << " at " << peak.magnitude << " (next "
                         << peak.next_largest << ")");
    CHECK(peak.bin == bin);

    // A bin-centred tone is one basis function, so everything else is rounding.
    CHECK(peak.next_largest < peak.magnitude / 1'000.0F);
  }
}

TEST_CASE("Fft separates DC from Nyquist", "[unit]") {
  // THE test of this wrapper. These two share a slot in PFFFT's output, and the
  // failure mode of reading it naively is that they are added together.
  const ingest::Fft fft;
  std::vector<float> magnitudes(kBins, 0.0F);

  SECTION("a constant is DC and nothing else") {
    const std::vector<float> signal(kSize, 0.5F);
    fft.magnitude_spectrum(signal, magnitudes);

    const Peak peak = find_peak(magnitudes);
    CHECK(peak.bin == 0);
    CHECK(magnitudes[kBins - 1] < peak.magnitude / 1'000.0F);
  }

  SECTION("an alternating signal is Nyquist and nothing else") {
    std::vector<float> signal(kSize, 0.0F);
    for (std::size_t index = 0; index < kSize; ++index) {
      signal[index] = (index % 2 == 0) ? 0.5F : -0.5F;
    }
    fft.magnitude_spectrum(signal, magnitudes);

    const Peak peak = find_peak(magnitudes);
    CHECK(peak.bin == kBins - 1);
    CHECK(magnitudes[0] < peak.magnitude / 1'000.0F);
  }

  SECTION("DC and Nyquist together stay apart") {
    // The case a naive read passes for the wrong reason: with both present,
    // folding one into the other still leaves both bins non-zero. The
    // discriminator is their RATIO -- here DC is three times Nyquist.
    std::vector<float> signal(kSize, 0.0F);
    for (std::size_t index = 0; index < kSize; ++index) {
      const float nyquist = (index % 2 == 0) ? 0.2F : -0.2F;
      signal[index] = 0.6F + nyquist;
    }
    fft.magnitude_spectrum(signal, magnitudes);

    REQUIRE(magnitudes[kBins - 1] > 0.0F);
    const float ratio = magnitudes[0] / magnitudes[kBins - 1];
    INFO("DC/Nyquist ratio " << ratio << " (expected 3)");
    CHECK(ratio > 2.9F);
    CHECK(ratio < 3.1F);
  }
}

TEST_CASE("Fft magnitude scales with amplitude", "[unit]") {
  const ingest::Fft fft;
  std::vector<float> quiet(kBins, 0.0F);
  std::vector<float> loud(kBins, 0.0F);

  fft.magnitude_spectrum(bin_centred_cosine(32, 0.25F), quiet);
  fft.magnitude_spectrum(bin_centred_cosine(32, 1.0F), loud);

  // Linear, so four times the amplitude is four times the magnitude. Spectral
  // flux is a difference of these, and a non-linear scale would make the
  // threshold mean something different at every level.
  const float ratio = loud[32] / quiet[32];
  INFO("magnitude ratio " << ratio << " for a 4x amplitude ratio");
  CHECK(ratio > 3.99F);
  CHECK(ratio < 4.01F);
}

TEST_CASE("Fft returns silence for silence", "[unit]") {
  const ingest::Fft fft;
  const std::vector<float> silence(kSize, 0.0F);
  std::vector<float> magnitudes(kBins, -1.0F);

  fft.magnitude_spectrum(silence, magnitudes);
  CHECK(
      std::all_of(magnitudes.begin(), magnitudes.end(), [](float value) { return value == 0.0F; }));
}

TEST_CASE("Fft is reusable and deterministic", "[unit]") {
  // One setup, many transforms -- building a PFFFT_Setup per frame would
  // dominate the analysis of a long file. Repeated use must not accumulate
  // state in the scratch buffers.
  const ingest::Fft fft;
  const std::vector<float> signal = bin_centred_cosine(100);

  std::vector<float> first(kBins, 0.0F);
  fft.magnitude_spectrum(signal, first);

  for (std::size_t round = 1; round <= 10; ++round) {
    std::vector<float> again(kBins, 0.0F);
    fft.magnitude_spectrum(bin_centred_cosine(round), again);
  }

  std::vector<float> last(kBins, 0.0F);
  fft.magnitude_spectrum(signal, last);
  CHECK(first == last);
}
