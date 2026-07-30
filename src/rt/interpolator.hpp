#ifndef CRATEDIG_RT_INTERPOLATOR_HPP
#define CRATEDIG_RT_INTERPOLATOR_HPP

#include <cstddef>

namespace rt {

// Fractional-position readers for sample playback.
//
// A voice plays through its Sample at some rate ratio, so almost every output
// frame lands between two input frames. The interpolator is what fills the gap,
// and its quality is the difference between a pitched-down break sounding like a
// record and sounding like a phone call.
//
// The kernel is the 4-point, 3rd-order Hermite form (equivalently: the uniform
// Catmull-Rom spline). It is the standard choice for samplers because it costs
// four multiplies, needs only one frame of history, and has no ringing — and
// because, unlike linear interpolation, its error falls off steeply with
// oversampling, which is the regime musical material actually lives in.
//
// Everything here is constexpr, branch-free, and allocation-free: it is called
// from the audio callback, once per output frame per voice.

// How far outside [i, i+1] the kernel reads. rt::Sample guards its channel
// storage by at least these amounts so the inner loop needs no bounds check;
// the coupling is enforced by a static_assert there rather than by comment.
inline constexpr std::size_t kHermiteTapsBefore = 1;
inline constexpr std::size_t kHermiteTapsAfter = 2;

// Interpolates between x0 and x1 at fraction t in [0, 1), using x[-1] and x[+2]
// to estimate the slope at each end.
//
// Exactness properties, which the tests pin down because they are the difference
// between this kernel and a subtly broken one:
//   - t == 0 returns x0 *bit-exactly* (c0 is x0 and every other term is
//     multiplied by t). This is what makes playback at ratio 1.0 bit-exact, and
//     therefore what makes the determinism goldens meaningful.
//   - It reproduces polynomials of degree <= 2 exactly. It does NOT reproduce
//     cubics: the centred-difference slope estimate (x1 - xm1)/2 is not the true
//     derivative of a cubic. Anyone tempted to assert cubic exactness should
//     check t = 0.25 on f(t) = t^3 first.
[[nodiscard]] constexpr float hermite4(float xm1, float x0, float x1, float x2, float t) noexcept {
  const float c0 = x0;
  const float c1 = 0.5F * (x1 - xm1);
  const float c2 = xm1 - (2.5F * x0) + (2.0F * x1) - (0.5F * x2);
  const float c3 = (0.5F * (x2 - xm1)) + (1.5F * (x0 - x1));
  return (((((c3 * t) + c2) * t) + c1) * t) + c0;
}

// Kept for the characterisation tests, not for playback. Asserting only "Hermite
// SNR is above N dB" can pass for a kernel that quietly degraded, so the tests
// also assert Hermite beats this by a wide margin — which requires having it.
[[nodiscard]] constexpr float linear2(float x0, float x1, float t) noexcept {
  return x0 + (t * (x1 - x0));
}

}  // namespace rt

#endif  // CRATEDIG_RT_INTERPOLATOR_HPP
