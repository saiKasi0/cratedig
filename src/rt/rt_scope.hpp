#ifndef CRATEDIG_RT_RT_SCOPE_HPP
#define CRATEDIG_RT_RT_SCOPE_HPP

#include <cstdint>

// RT_SCOPE() — the first and most authoritative line of real-time safety
// enforcement (CLAUDE.md). Every audio-callback entry point opens one.
//
// Mechanism: a thread-local depth counter, plus replaced global operator new /
// operator delete. While the counter is non-zero on the current thread, any
// allocation invokes the violation handler, which by default writes a message
// and aborts. The replacements forward to malloc/free, so ASan and TSan keep
// working normally underneath.
//
// Why this and not malloc interposition: replacing operator new is standard
// C++, behaves identically on macOS and Linux, needs no platform code, and
// survives SIP and ctest child processes. It catches everything our own code can
// do to the heap (new, std::vector growth, std::string, std::function
// assignment). It does NOT catch a raw C malloc from inside a third-party
// library — nothing in the callback is permitted to call such a library, and
// malloc-level interposition is tracked as later hardening in
// docs/ARCHITECTURE.md.
//
// Enabled by the CRATEDIG_RT_GUARD compile definition. When off, RT_SCOPE()
// expands to nothing and the operator replacements are not compiled at all.

// ThreadSanitizer detection. TSan's runtime defines the whole operator
// new/delete family as STRONG symbols on Linux, so our replacements collide at
// link time ("multiple definition of operator new"). ASan's equivalents are weak
// and coexist fine; macOS sanitizer runtimes interpose instead of defining, so
// this only bites on Linux + TSan — which is precisely why the Docker CI path
// exists.
//
// Under TSan we therefore compile out the operator replacements but keep scope
// tracking. This is applied on every platform, not just Linux, so a TSan build
// behaves the same everywhere — a guard that is present on macOS and absent on
// Linux would be worse than one that is predictably absent on both.
//
// Nothing goes unchecked overall: TSan's job is race detection, and dev/asan/
// ubsan still enforce the allocation rule on the same code. Tests that assert
// allocation *detection* skip themselves loudly rather than passing vacuously —
// see rt::kAllocationDetectionEnabled.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CRATEDIG_RT_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define CRATEDIG_RT_TSAN 1
#endif
#ifndef CRATEDIG_RT_TSAN
#define CRATEDIG_RT_TSAN 0
#endif

#if CRATEDIG_RT_GUARD && !CRATEDIG_RT_TSAN
#define CRATEDIG_RT_DETECT_ALLOCATIONS 1
#else
#define CRATEDIG_RT_DETECT_ALLOCATIONS 0
#endif

namespace rt {

// True when allocation inside an RT_SCOPE is actually detected and reported.
// False under TSan (see above) and when the guard is compiled out. Scope depth
// tracking works either way.
inline constexpr bool kAllocationDetectionEnabled = CRATEDIG_RT_DETECT_ALLOCATIONS != 0;

// Called on the offending thread, at the point of allocation. Must be
// async-signal-safe: it may run inside the audio callback.
using ViolationHandler = void (*)(const char* what);

// Replaces the violation handler and returns the previous one.
//
// This exists for tests, which need to count violations rather than die on the
// first one. Production code must leave the default (write + abort) installed:
// an allocation in the audio callback is a defect, and continuing past it just
// produces a dropout somewhere less debuggable.
ViolationHandler set_violation_handler(ViolationHandler handler) noexcept;

[[nodiscard]] ViolationHandler violation_handler() noexcept;

// True while the calling thread is inside at least one RT_SCOPE.
[[nodiscard]] bool in_rt_scope() noexcept;

// Current nesting depth on the calling thread.
[[nodiscard]] std::uint32_t rt_scope_depth() noexcept;

// RAII guard. Nesting is supported and counted; only leaving the outermost
// scope re-permits allocation on that thread.
class ScopedRtGuard {
 public:
  ScopedRtGuard() noexcept;
  ~ScopedRtGuard();

  ScopedRtGuard(const ScopedRtGuard&) = delete;
  ScopedRtGuard& operator=(const ScopedRtGuard&) = delete;
  ScopedRtGuard(ScopedRtGuard&&) = delete;
  ScopedRtGuard& operator=(ScopedRtGuard&&) = delete;
};

}  // namespace rt

// The guard variable is named per line so that nesting RT_SCOPE() in an inner
// block does not shadow the outer one (-Wshadow is an error here).
#define CRATEDIG_RT_CONCAT_INNER(a, b) a##b
#define CRATEDIG_RT_CONCAT(a, b) CRATEDIG_RT_CONCAT_INNER(a, b)

#if CRATEDIG_RT_GUARD
#define RT_SCOPE() \
  const ::rt::ScopedRtGuard CRATEDIG_RT_CONCAT(cratedig_rt_guard_, __LINE__) {}
#else
#define RT_SCOPE() static_cast<void>(0)
#endif

#endif  // CRATEDIG_RT_RT_SCOPE_HPP
