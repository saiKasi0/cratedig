// Compile-only check: every src/rt/ header must build with exceptions and RTTI
// disabled, because src/engine/ and future real-time targets compile that way
// (CLAUDE.md). cratedig_rt is an INTERFACE target and so cannot carry the flags
// itself — this translation unit is where the constraint is actually enforced.
//
// The same headers are compiled *with* exceptions by the Catch2 test TUs, so
// they must work both ways. Nothing here runs; it only has to compile.

#include "rt/arch.hpp"
#include "rt/garbage_ring.hpp"
#include "rt/result.hpp"
#include "rt/spsc_ring.hpp"

#include <cstddef>
#include <cstdint>

namespace {

enum class ProbeError : std::uint8_t { kNone, kOverflow };

// Force instantiation of every template, since an uninstantiated template body
// is only partially checked.
using ProbeResult = rt::Result<std::size_t, ProbeError>;
using ProbeRing = rt::SpscRing<std::uint64_t, 8>;
using ProbeGarbage = rt::GarbageRing<4>;

constexpr ProbeResult kOkResult{std::size_t{7}};
constexpr ProbeResult kErrResult{rt::Err<ProbeError>{ProbeError::kOverflow}};

static_assert(kOkResult.ok());
static_assert(kOkResult.value() == 7);
static_assert(!kErrResult.ok());
static_assert(kErrResult.error() == ProbeError::kOverflow);
static_assert(kErrResult.value_or(99) == 99);

static_assert(ProbeRing::capacity() == 8);
static_assert(ProbeGarbage::capacity() == 4);
static_assert(rt::kCacheLine == 64);

[[maybe_unused]] void instantiate_ring_members(ProbeRing& ring, std::uint64_t& slot) noexcept {
  std::uint64_t batch[2] = {1, 2};
  static_cast<void>(ring.try_push(slot));
  static_cast<void>(ring.try_push(std::span<const std::uint64_t>{batch}));
  static_cast<void>(ring.try_pop(slot));
  static_cast<void>(ring.try_pop(std::span<std::uint64_t>{batch}));
  static_cast<void>(ring.size_approx());
  static_cast<void>(ring.empty_approx());
}

}  // namespace
