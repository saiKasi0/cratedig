#ifndef CRATEDIG_INGEST_WINDOW_HPP
#define CRATEDIG_INGEST_WINDOW_HPP

#include <array>
#include <cstddef>
#include <numbers>

namespace ingest {

// Analysis windows, built at COMPILE TIME.
//
// CLAUDE.md: "constexpr tables for windows, sinc kernels, dB curves. No runtime
// table builds." The obstacle is that std::cos is not constexpr in C++20, so a
// constexpr Hann window needs a constexpr cosine, which is what the first half
// of this header is.
//
// Worth doing rather than working around. A runtime-built table has to be built
// somewhere, and the somewheres are all worse: a function-local static costs a
// guard variable check on every call, a namespace-scope one runs before main in
// an order nothing specifies, and building it per call is what the rule exists
// to prevent. A constexpr array is in .rodata and has no initialisation at all.

namespace detail {

inline constexpr double kPi = std::numbers::pi;
inline constexpr double kTwoPi = 2.0 * kPi;

// cos(x) for any real x, constexpr.
//
// Range-reduced to [0, pi/4] and evaluated by Taylor series. Not the fastest
// possible cosine and it does not need to be: it runs at compile time, and the
// only thing that matters is that the result matches std::cos closely enough
// that the window is the window everyone else means. window_test.cpp pins that
// against the real std::cos to a committed bound.
[[nodiscard]] constexpr double cos_taylor(double x) noexcept {
  // 13 terms is comfortably past double precision over [0, pi/4], where the
  // largest argument is 0.7854 and terms shrink by x^2/(2n(2n-1)) each time.
  double term = 1.0;
  double sum = 1.0;
  const double squared = x * x;
  for (int n = 1; n <= 13; ++n) {
    term *= -squared / static_cast<double>((2 * n) * ((2 * n) - 1));
    sum += term;
  }
  return sum;
}

[[nodiscard]] constexpr double sin_taylor(double x) noexcept {
  double term = x;
  double sum = x;
  const double squared = x * x;
  for (int n = 1; n <= 13; ++n) {
    term *= -squared / static_cast<double>((2 * n) * ((2 * n) + 1));
    sum += term;
  }
  return sum;
}

// The reduction: fold x into [0, 2pi), then into an octant, and use the
// symmetry that makes the Taylor series accurate everywhere rather than only
// near zero. Evaluating the series directly at, say, x = 6 would need far more
// terms and still lose precision to cancellation.
[[nodiscard]] constexpr double cos_constexpr(double x) noexcept {
  if (x < 0.0) {
    x = -x;  // cos is even
  }
  // Fold to [0, 2pi). A loop rather than fmod, which is not constexpr either.
  while (x >= kTwoPi) {
    x -= kTwoPi;
  }
  // cos(x) = -cos(x - pi), folding [pi, 2pi) onto [0, pi).
  bool negate = false;
  if (x >= kPi) {
    x -= kPi;
    negate = true;
  }
  // cos(x) = -cos(pi - x), folding [pi/2, pi) onto [0, pi/2).
  if (x > kPi / 2.0) {
    x = kPi - x;
    negate = !negate;
  }
  // cos(x) = sin(pi/2 - x), folding [pi/4, pi/2] onto [0, pi/4].
  const double value = x > kPi / 4.0 ? sin_taylor((kPi / 2.0) - x) : cos_taylor(x);
  return negate ? -value : value;
}

}  // namespace detail

// A periodic Hann window of N points.
//
// PERIODIC (divisor N) rather than symmetric (divisor N-1). The distinction is
// small and matters: a periodic window is the one whose overlapping copies sum
// to a constant, which is what an STFT needs. The symmetric form is for filter
// design. Getting this wrong produces a spectrum with a low-level ripple that
// looks like the signal rather than like a mistake.
//
// Stored in double and converted at use, so the table itself is not the source
// of any error.
template <std::size_t N>
[[nodiscard]] constexpr std::array<float, N> hann_window() noexcept {
  static_assert(N > 1, "hann_window: need at least two points");
  std::array<float, N> window{};
  for (std::size_t index = 0; index < N; ++index) {
    const double phase = detail::kTwoPi * static_cast<double>(index) / static_cast<double>(N);
    window[index] = static_cast<float>(0.5 - (0.5 * detail::cos_constexpr(phase)));
  }
  return window;
}

}  // namespace ingest

#endif  // CRATEDIG_INGEST_WINDOW_HPP
