#include "rt/rt_scope.hpp"

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace rt {
namespace {

// Depth rather than a bool so nested scopes compose: an inner guard must not
// re-permit allocation when it exits.
thread_local std::uint32_t t_rt_depth = 0;

void default_violation_handler(const char* what) {
  // Async-signal-safe only: this can run inside the audio callback, where
  // iostreams, printf, and anything that takes a lock are all unsafe. write(2)
  // and abort() are the whole budget.
  static constexpr char kPrefix[] = "RT_SCOPE violation: ";
  static_cast<void>(::write(STDERR_FILENO, kPrefix, sizeof(kPrefix) - 1));

  std::size_t length = 0;
  while (what != nullptr && what[length] != '\0') {
    ++length;
  }
  if (length != 0) {
    static_cast<void>(::write(STDERR_FILENO, what, length));
  }
  static_cast<void>(::write(STDERR_FILENO, "\n", 1));

  std::abort();
}

// seq_cst is deliberate: the handler is swapped only by tests, at setup time,
// and read on a path that is already fatal in production. Correctness under any
// interleaving is worth more here than saving a fence on a cold branch.
std::atomic<ViolationHandler> g_violation_handler{&default_violation_handler};

#if CRATEDIG_RT_GUARD
// Single choke point for every replaced operator new below.
void check_rt_allocation(const char* what) noexcept {
  if (t_rt_depth == 0) {
    return;
  }
  const ViolationHandler handler = g_violation_handler.load(std::memory_order_seq_cst);
  if (handler != nullptr) {
    handler(what);
  }
}

// Forwarding to malloc rather than to the library's operator new is what keeps
// ASan and TSan effective: they interpose malloc, so their bookkeeping is
// unchanged by this replacement.
void* allocate(std::size_t size, const char* what) noexcept {
  check_rt_allocation(what);
  // A zero-size allocation must still return a unique pointer.
  return std::malloc(size != 0 ? size : 1);
}

void* allocate_aligned(std::size_t size, std::size_t alignment, const char* what) noexcept {
  check_rt_allocation(what);
  if (size == 0) {
    size = alignment;
  }
  // std::aligned_alloc requires size to be a multiple of alignment.
  const std::size_t rounded = ((size + alignment - 1) / alignment) * alignment;
  return std::aligned_alloc(alignment, rounded);
}

// The throwing forms of operator new must report exhaustion as std::bad_alloc.
[[noreturn]] void throw_bad_alloc() {
  throw std::bad_alloc();
}

void* allocate_or_throw(std::size_t size, const char* what) {
  void* memory = allocate(size, what);
  if (memory == nullptr) {
    throw_bad_alloc();
  }
  return memory;
}

void* allocate_aligned_or_throw(std::size_t size, std::size_t alignment, const char* what) {
  void* memory = allocate_aligned(size, alignment, what);
  if (memory == nullptr) {
    throw_bad_alloc();
  }
  return memory;
}
#endif  // CRATEDIG_RT_GUARD

}  // namespace

ViolationHandler set_violation_handler(ViolationHandler handler) noexcept {
  return g_violation_handler.exchange(handler != nullptr ? handler : &default_violation_handler,
                                      std::memory_order_seq_cst);
}

ViolationHandler violation_handler() noexcept {
  return g_violation_handler.load(std::memory_order_seq_cst);
}

bool in_rt_scope() noexcept {
  return t_rt_depth != 0;
}

std::uint32_t rt_scope_depth() noexcept {
  return t_rt_depth;
}

ScopedRtGuard::ScopedRtGuard() noexcept {
  ++t_rt_depth;
}

ScopedRtGuard::~ScopedRtGuard() {
  --t_rt_depth;
}

}  // namespace rt

#if CRATEDIG_RT_GUARD

// Replacements for the global allocation functions. All eight forms must be
// replaced together: leaving one unreplaced would let that path allocate
// unnoticed, and mixing our malloc'd pointers with the library's operator delete
// would be undefined.
//
// IMPORTANT: these must be linked directly into every executable. In a static
// library the linker only pulls in objects that resolve an undefined symbol, and
// replacement operators resolve nothing — the object would be silently dropped
// and the guard would appear to work while enforcing nothing. That is why this
// file is built as an OBJECT library (see src/CMakeLists.txt).

void* operator new(std::size_t size) {
  return rt::allocate_or_throw(size, "operator new");
}

void* operator new[](std::size_t size) {
  return rt::allocate_or_throw(size, "operator new[]");
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return rt::allocate(size, "operator new(nothrow)");
}

void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return rt::allocate(size, "operator new[](nothrow)");
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return rt::allocate_aligned_or_throw(size, static_cast<std::size_t>(alignment),
                                       "operator new(align)");
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return rt::allocate_aligned_or_throw(size, static_cast<std::size_t>(alignment),
                                       "operator new[](align)");
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t& /*tag*/) noexcept {
  return rt::allocate_aligned(size, static_cast<std::size_t>(alignment),
                              "operator new(align, nothrow)");
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& /*tag*/) noexcept {
  return rt::allocate_aligned(size, static_cast<std::size_t>(alignment),
                              "operator new[](align, nothrow)");
}

// Deallocation is NOT checked. Freeing inside the callback is equally forbidden,
// but the audio thread never holds the last reference to anything — retired
// buffers go to rt::GarbageRing for the janitor thread (CLAUDE.md rule 5). A
// check here would instead fire on every scope-exit of an object allocated
// before the scope was entered, which is legal and common.

void operator delete(void* memory) noexcept {
  std::free(memory);
}

void operator delete[](void* memory) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept {
  std::free(memory);
}

void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, std::align_val_t /*alignment*/) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, std::size_t /*size*/,
                       std::align_val_t /*alignment*/) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::align_val_t /*alignment*/,
                     const std::nothrow_t& /*tag*/) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, std::align_val_t /*alignment*/,
                       const std::nothrow_t& /*tag*/) noexcept {
  std::free(memory);
}

#endif  // CRATEDIG_RT_GUARD
