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

}  // namespace rt

#endif  // CRATEDIG_RT_ARCH_HPP
