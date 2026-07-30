#include "rt/garbage_ring.hpp"

#include "rt/rt_scope.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

std::atomic<int> g_violation_count{0};

void counting_handler(const char* /*what*/) {
  g_violation_count.fetch_add(1, std::memory_order_relaxed);
}

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

// Stands in for a decoded Sample: records where it was destroyed, which is the
// whole point of the type under test.
struct TrackedBuffer {
  static inline std::atomic<int> live{0};
  static inline std::atomic<int> destroyed{0};
  static inline std::atomic<std::thread::id> last_destroy_thread{};

  TrackedBuffer() { live.fetch_add(1, std::memory_order_relaxed); }

  ~TrackedBuffer() {
    live.fetch_sub(1, std::memory_order_relaxed);
    destroyed.fetch_add(1, std::memory_order_relaxed);
    last_destroy_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
  }

  TrackedBuffer(const TrackedBuffer&) = delete;
  TrackedBuffer& operator=(const TrackedBuffer&) = delete;
  TrackedBuffer(TrackedBuffer&&) = delete;
  TrackedBuffer& operator=(TrackedBuffer&&) = delete;

  static void reset_counters() {
    live.store(0, std::memory_order_relaxed);
    destroyed.store(0, std::memory_order_relaxed);
  }
};

}  // namespace

TEST_CASE("GarbageRing starts empty", "[unit]") {
  rt::GarbageRing<4> ring;

  CHECK(ring.size_approx() == 0);
  CHECK(ring.collect() == 0);
  CHECK(ring.overflow_count() == 0);
}

TEST_CASE("GarbageRing defers destruction until collect", "[unit]") {
  TrackedBuffer::reset_counters();
  rt::GarbageRing<4> ring;

  {
    auto buffer = std::make_shared<TrackedBuffer>();
    REQUIRE(TrackedBuffer::live.load() == 1);
    REQUIRE(ring.retire(std::move(buffer)));
    // The local reference is gone, but the ring is holding the object alive.
    CHECK(TrackedBuffer::live.load() == 1);
    CHECK(TrackedBuffer::destroyed.load() == 0);
  }

  CHECK(TrackedBuffer::live.load() == 1);
  CHECK(ring.size_approx() == 1);

  CHECK(ring.collect() == 1);
  CHECK(TrackedBuffer::live.load() == 0);
  CHECK(TrackedBuffer::destroyed.load() == 1);
  CHECK(ring.size_approx() == 0);
}

TEST_CASE("GarbageRing retire does not allocate", "[unit]") {
  // The reason this type exists: retiring must be callable from the audio
  // callback. Creating the object allocates, so that happens outside the scope.
  TrackedBuffer::reset_counters();
  rt::GarbageRing<4> ring;
  auto buffer = std::make_shared<TrackedBuffer>();

  const HandlerSwap swap;
  {
    RT_SCOPE();
    const bool accepted = ring.retire(std::move(buffer));
    CHECK(accepted);
  }
  CHECK(HandlerSwap::count() == 0);

  // Destruction is deferred, so nothing was freed inside the scope either.
  CHECK(TrackedBuffer::live.load() == 1);

  // Self-check: prove the guard was actually armed in this exact context, so
  // that the zero above means "retire did not allocate" rather than "nothing was
  // watching". Creating a shared_ptr inside a scope must be reported.
  {
    RT_SCOPE();
    auto inside = std::make_shared<TrackedBuffer>();
    CHECK(inside != nullptr);
    static_cast<void>(ring.retire(std::move(inside)));
  }
  CHECK(HandlerSwap::count() > 0);

  ring.collect();
}

TEST_CASE("GarbageRing full ring leaves the caller's reference intact", "[unit]") {
  TrackedBuffer::reset_counters();
  constexpr std::size_t kCapacity = 2;
  rt::GarbageRing<kCapacity> ring;

  for (std::size_t i = 0; i < kCapacity; ++i) {
    REQUIRE(ring.retire(std::make_shared<TrackedBuffer>()));
  }
  REQUIRE(TrackedBuffer::live.load() == static_cast<int>(kCapacity));

  auto rejected = std::make_shared<TrackedBuffer>();
  const auto* raw = rejected.get();

  CHECK_FALSE(ring.retire(std::move(rejected)));
  CHECK(ring.overflow_count() == 1);

  // The contract: on refusal the caller still owns the pointer and the object is
  // still alive. Losing it here would mean destroying it on the audio thread —
  // the exact failure this type prevents.
  REQUIRE(rejected != nullptr);
  CHECK(rejected.get() == raw);
  CHECK(rejected.use_count() == 1);
  CHECK(TrackedBuffer::live.load() == static_cast<int>(kCapacity) + 1);

  // After a collect the same pointer is accepted.
  CHECK(ring.collect() == kCapacity);
  CHECK(ring.retire(std::move(rejected)));
  CHECK(ring.collect() == 1);
  CHECK(TrackedBuffer::live.load() == 0);
}

TEST_CASE("GarbageRing wraps around", "[unit]") {
  TrackedBuffer::reset_counters();
  rt::GarbageRing<2> ring;

  for (int cycle = 0; cycle < 100; ++cycle) {
    INFO("cycle " << cycle);
    REQUIRE(ring.retire(std::make_shared<TrackedBuffer>()));
    REQUIRE(ring.retire(std::make_shared<TrackedBuffer>()));
    REQUIRE(ring.collect() == 2);
  }

  CHECK(TrackedBuffer::live.load() == 0);
  CHECK(TrackedBuffer::destroyed.load() == 200);
  CHECK(ring.overflow_count() == 0);
}

TEST_CASE("GarbageRing retiring null is a no-op", "[unit]") {
  rt::GarbageRing<4> ring;
  std::shared_ptr<TrackedBuffer> empty;

  CHECK(ring.retire(std::move(empty)));
  CHECK(ring.size_approx() == 0);
  CHECK(ring.collect() == 0);
}

TEST_CASE("GarbageRing destroys on the collecting thread only", "[unit][stress]") {
  TrackedBuffer::reset_counters();
  constexpr int kObjects = 20'000;
  rt::GarbageRing<64> ring;

  const std::thread::id audio_thread_id = std::this_thread::get_id();
  std::atomic<bool> producer_done{false};
  std::atomic<int> collected_total{0};

  std::thread janitor([&] {
    while (!producer_done.load(std::memory_order_acquire) || ring.size_approx() != 0) {
      collected_total.fetch_add(static_cast<int>(ring.collect()), std::memory_order_relaxed);
      std::this_thread::yield();
    }
    collected_total.fetch_add(static_cast<int>(ring.collect()), std::memory_order_relaxed);
  });

  const std::thread::id janitor_id = janitor.get_id();

  for (int i = 0; i < kObjects; ++i) {
    // Allocation happens outside the RT scope, as it would on a worker thread.
    auto buffer = std::make_shared<TrackedBuffer>();
    while (!ring.retire(std::move(buffer))) {
      std::this_thread::yield();
    }
  }
  producer_done.store(true, std::memory_order_release);
  janitor.join();

  CHECK(collected_total.load() == kObjects);
  CHECK(TrackedBuffer::destroyed.load() == kObjects);
  CHECK(TrackedBuffer::live.load() == 0);

  // Every destructor must have run on the janitor, never on the retiring thread.
  const std::thread::id last = TrackedBuffer::last_destroy_thread.load();
  CHECK(last == janitor_id);
  CHECK(last != audio_thread_id);
}
