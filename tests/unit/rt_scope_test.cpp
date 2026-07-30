#include "rt/rt_scope.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

std::atomic<int> g_violation_count{0};

// The compiler may elide a call to operator new whose result is never observed
// ([expr.new]/10), and at -O2 it does — including eliding new/delete pairs
// outright. Any test here that wants a *real* allocation must route the pointer
// through this sink, otherwise it silently tests nothing.
volatile const void* g_escape_sink = nullptr;

void escape(const void* pointer) noexcept {
  g_escape_sink = pointer;
}

// Must not allocate: it runs from inside operator new, so allocating here would
// recurse. Counting into an atomic is the whole budget.
void counting_handler(const char* /*what*/) {
  g_violation_count.fetch_add(1, std::memory_order_relaxed);
}

// Restores the previous handler however the test exits, so one failing
// expectation cannot leave the abort-on-allocate handler swapped out for the
// rest of the run.
class HandlerSwap {
 public:
  HandlerSwap() : m_previous(rt::set_violation_handler(&counting_handler)) {
    g_violation_count.store(0, std::memory_order_relaxed);
  }

  ~HandlerSwap() { rt::set_violation_handler(m_previous); }

  HandlerSwap(const HandlerSwap&) = delete;
  HandlerSwap& operator=(const HandlerSwap&) = delete;
  HandlerSwap(HandlerSwap&&) = delete;
  HandlerSwap& operator=(HandlerSwap&&) = delete;

  [[nodiscard]] static int count() { return g_violation_count.load(std::memory_order_relaxed); }

 private:
  rt::ViolationHandler m_previous;
};

}  // namespace

TEST_CASE("no RT scope means no violations", "[unit]") {
  if constexpr (!rt::kAllocationDetectionEnabled) {
    SKIP("allocation detection is compiled out (TSan build) — see rt_scope.hpp");
  }

  const HandlerSwap swap;

  std::vector<int> values;
  values.reserve(1024);  // definitely allocates
  values.push_back(1);
  escape(values.data());

  CHECK_FALSE(rt::in_rt_scope());
  CHECK(HandlerSwap::count() == 0);
}

TEST_CASE("allocating inside an RT scope is a violation", "[unit]") {
  if constexpr (!rt::kAllocationDetectionEnabled) {
    SKIP("allocation detection is compiled out (TSan build) — see rt_scope.hpp");
  }

  const HandlerSwap swap;

  // Allocate the vector's storage BEFORE the scope; only the growth inside it
  // should be reported.
  std::vector<int> values;
  values.reserve(1);
  escape(values.data());
  const int before = HandlerSwap::count();

  {
    RT_SCOPE();
    CHECK(rt::in_rt_scope());
    values.push_back(1);
    values.push_back(2);  // forces reallocation
    values.push_back(3);
    escape(values.data());
  }

  CHECK_FALSE(rt::in_rt_scope());
  CHECK(HandlerSwap::count() > before);
}

TEST_CASE("std::string allocation inside an RT scope is caught", "[unit]") {
  if constexpr (!rt::kAllocationDetectionEnabled) {
    SKIP("allocation detection is compiled out (TSan build) — see rt_scope.hpp");
  }

  const HandlerSwap swap;

  {
    RT_SCOPE();
    // Long enough to defeat the small-string optimization on every platform.
    const std::string message(512, 'x');
    escape(message.data());
    CHECK(message.size() == 512);
  }

  CHECK(HandlerSwap::count() > 0);
}

TEST_CASE("RT scopes nest and only the outermost re-permits allocation", "[unit]") {
  if constexpr (!rt::kAllocationDetectionEnabled) {
    SKIP("allocation detection is compiled out (TSan build) — see rt_scope.hpp");
  }

  const HandlerSwap swap;

  CHECK(rt::rt_scope_depth() == 0);
  {
    RT_SCOPE();
    CHECK(rt::rt_scope_depth() == 1);
    {
      RT_SCOPE();
      CHECK(rt::rt_scope_depth() == 2);
    }
    // Leaving the inner scope must NOT re-permit allocation.
    CHECK(rt::rt_scope_depth() == 1);
    CHECK(rt::in_rt_scope());

    const int before = HandlerSwap::count();
    std::vector<int> values;
    values.push_back(1);
    escape(values.data());
    CHECK(HandlerSwap::count() > before);
  }
  CHECK(rt::rt_scope_depth() == 0);
  CHECK_FALSE(rt::in_rt_scope());
}

TEST_CASE("RT scope depth is per-thread", "[unit]") {
  const HandlerSwap swap;

  RT_SCOPE();
  REQUIRE(rt::in_rt_scope());

  // Another thread must be unaffected — the audio thread being in a scope does
  // not constrain the worker threads.
  bool other_thread_in_scope = true;
  std::uint32_t other_depth = 99;
  std::thread observer([&] {
    other_thread_in_scope = rt::in_rt_scope();
    other_depth = rt::rt_scope_depth();
  });
  observer.join();

  CHECK_FALSE(other_thread_in_scope);
  CHECK(other_depth == 0);
  CHECK(rt::in_rt_scope());
}

TEST_CASE("the guard is actually linked in", "[unit]") {
  if constexpr (!rt::kAllocationDetectionEnabled) {
    SKIP("allocation detection is compiled out (TSan build) — see rt_scope.hpp");
  }

  // Guards against the OBJECT-library-versus-static-library trap: if the
  // replacement operators were dropped at link time, every other test in this
  // file would pass vacuously because no violation could ever be reported.
  const HandlerSwap swap;
  {
    RT_SCOPE();
    // A raw ::operator new call routed through the escape sink — the one form
    // the optimizer cannot elide.
    void* block = ::operator new(64);
    escape(block);
    ::operator delete(block);
  }
  CHECK(HandlerSwap::count() > 0);
}
