#ifndef CRATEDIG_RT_ARCH_HPP
#define CRATEDIG_RT_ARCH_HPP

#include <cstddef>

namespace rt {

// Cache line size used to pad cross-thread state so a producer and a consumer
// never share a line. Deliberately a fixed constant rather than
// std::hardware_destructive_interference_size: that value differs between
// compilers and standard library versions, which would silently change struct
// layout across toolchains. Layout stability matters here because the same ring
// types are exercised by TSan on Linux and by the audio thread on macOS, and
// because determinism is a product requirement (docs/TESTING.md).
//
// 64 is correct for x86-64 and for the 64-byte L1 line on Apple silicon. Apple's
// M-series cores use 128-byte L2 sectors; over-padding there costs a little
// memory and nothing else, whereas under-padding would cost false sharing.
inline constexpr std::size_t kCacheLine = 64;

// Recursive state below this magnitude is flushed to zero. -600 dBFS.
//
// Far below anything audible and far above the smallest normal float (about
// 1.2e-38). Anything with feedback -- a biquad, a compressor's envelope
// follower -- decays geometrically toward zero and eventually produces
// denormals, which on several CPUs cost hundreds of cycles each. In an audio
// callback that is a dropout caused by a tail nobody can hear.
//
// FLUSHED IN THE DSP RATHER THAN LEFT TO FTZ/DAZ, deliberately.
// rt::ScopedDenormalDisable sets those at audio-thread start, but the offline
// renderer never opens a device and so never sets them. Relying on the FPU mode
// would make a live render and a bounce of the same material disagree in the far
// tail, and "same input, same bytes" is a promise this project makes
// (docs/TESTING.md). Doing it explicitly makes the behaviour identical either
// way, which is worth more than the last 1e-30 of a decay.
//
// Here rather than in one DSP header because it is a property of the float
// format, not of any particular filter, and two copies would eventually differ.
inline constexpr float kDenormalFloor = 1.0e-30F;

// Flushes a recursive state value, leaving NaN alone.
//
// NaN fails both comparisons and passes through unchanged, which is the honest
// behaviour: a NaN in the audio is a bug to find, not one to hide by quietly
// turning it into silence.
[[nodiscard]] constexpr float flush_denormal(float value) noexcept {
  return (value > -kDenormalFloor && value < kDenormalFloor) ? 0.0F : value;
}

}  // namespace rt

#endif  // CRATEDIG_RT_ARCH_HPP
