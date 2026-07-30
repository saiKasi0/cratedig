// rt::HandoffRing — the control -> audio half of the live reconfiguration
// protocol (docs/ARCHITECTURE.md).
//
// The property that actually matters here is not "handles arrive". It is that
// the CONSUMER never runs a destructor: the consumer is the audio thread, and a
// destructor there means a free() inside the callback. A single-threaded test
// cannot see that, so the test that matters records which thread each payload
// died on and asserts it was never the consumer's — with the GarbageRing
// downstream, exactly as Engine wires them.
//
// Correctness authority for the ordering is the stress case under TSan
// (docs/TESTING.md), not the single-threaded cases below.

#include "rt/handoff_ring.hpp"

#include "rt/garbage_ring.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

// Set on the consumer thread only. A thread_local rather than an atomic
// thread::id: the destructor's question is "am I the forbidden thread", which a
// thread_local answers with no shared state for TSan to have an opinion about.
thread_local bool t_is_consumer_thread = false;

// A payload that remembers where it was destroyed.
struct Tracked {
  std::size_t id = 0;

  Tracked() = default;

  explicit Tracked(std::size_t value) : id(value) {}

  Tracked(const Tracked&) = delete;
  Tracked& operator=(const Tracked&) = delete;
  Tracked(Tracked&&) = delete;
  Tracked& operator=(Tracked&&) = delete;

  ~Tracked() {
    s_destroyed.fetch_add(1, std::memory_order_relaxed);
    if (t_is_consumer_thread) {
      s_destroyed_on_consumer.fetch_add(1, std::memory_order_relaxed);
    }
  }

  static std::atomic<std::size_t> s_destroyed;
  static std::atomic<std::size_t> s_destroyed_on_consumer;
};

std::atomic<std::size_t> Tracked::s_destroyed{0};
std::atomic<std::size_t> Tracked::s_destroyed_on_consumer{0};

void reset_tracking() {
  Tracked::s_destroyed.store(0, std::memory_order_relaxed);
  Tracked::s_destroyed_on_consumer.store(0, std::memory_order_relaxed);
}

constexpr std::size_t kCapacity = 4;
using Ring = rt::HandoffRing<Tracked, kCapacity>;

[[nodiscard]] std::shared_ptr<const Tracked> make(std::size_t id) {
  return std::make_shared<const Tracked>(id);
}

}  // namespace

TEST_CASE("HandoffRing carries a handle from producer to consumer", "[unit]") {
  Ring ring;
  const std::shared_ptr<const Tracked> payload = make(7);

  REQUIRE(ring.try_publish(std::shared_ptr<const Tracked>{payload}));
  CHECK(ring.size_approx() == 1);

  std::shared_ptr<const Tracked> received;
  REQUIRE(ring.try_take(received));
  CHECK(received == payload);
  CHECK(received->id == 7);
  CHECK(ring.size_approx() == 0);
}

TEST_CASE("HandoffRing take on an empty ring leaves the destination alone", "[unit]") {
  Ring ring;
  std::shared_ptr<const Tracked> received;
  CHECK_FALSE(ring.try_take(received));
  CHECK(received == nullptr);
}

TEST_CASE("HandoffRing refuses a null handle", "[unit]") {
  Ring ring;
  // Refused rather than counted as an overflow: a null handle carries no pad
  // index, so the audio thread could not act on it even if it arrived.
  CHECK_FALSE(ring.try_publish(std::shared_ptr<const Tracked>{}));
  CHECK(ring.size_approx() == 0);
  CHECK(ring.rejected_count() == 0);
}

TEST_CASE("HandoffRing uses its whole capacity and leaves a refused handle with the caller",
          "[unit]") {
  Ring ring;
  std::vector<std::shared_ptr<const Tracked>> kept;
  for (std::size_t index = 0; index < kCapacity; ++index) {
    kept.push_back(make(index));
    REQUIRE(ring.try_publish(std::shared_ptr<const Tracked>{kept.back()}));
  }
  CHECK(ring.size_approx() == kCapacity);

  // One past capacity. The contract is that the caller still owns it — the same
  // as GarbageRing::retire, because a silently dropped edit is worse than a
  // refused one.
  std::shared_ptr<const Tracked> refused = make(99);
  const auto* raw = refused.get();
  // Inspecting a moved-from object is the assertion, not an accident: the
  // contract is that a refused publish does NOT consume its argument.
  // NOLINTBEGIN(bugprone-use-after-move,hicpp-invalid-access-moved)
  CHECK_FALSE(ring.try_publish(std::move(refused)));
  CHECK(refused != nullptr);
  CHECK(refused.get() == raw);
  // NOLINTEND(bugprone-use-after-move,hicpp-invalid-access-moved)
  CHECK(ring.rejected_count() == 1);

  for (std::size_t index = 0; index < kCapacity; ++index) {
    std::shared_ptr<const Tracked> received;
    REQUIRE(ring.try_take(received));
    CHECK(received->id == index);  // FIFO, not whichever slot came to hand
  }
}

TEST_CASE("HandoffRing releases every reference it held once drained", "[unit]") {
  Ring ring;
  {
    const std::shared_ptr<const Tracked> payload = make(1);
    REQUIRE(ring.try_publish(std::shared_ptr<const Tracked>{payload}));
    CHECK(payload.use_count() == 2);  // the test's, and the ring's

    std::shared_ptr<const Tracked> received;
    REQUIRE(ring.try_take(received));
    // Taking MOVES out of the slot. If it copied instead, the ring would keep a
    // reference forever and every published object would leak until the ring
    // wrapped around onto that slot again.
    CHECK(payload.use_count() == 2);  // the test's, and `received`'s
  }
}

TEST_CASE("HandoffRing indices wrap without losing or reordering handles", "[unit]") {
  Ring ring;
  // Well past Capacity so the masked slot offsets are reused many times over.
  constexpr std::size_t kRounds = (kCapacity * 4) + 3;
  for (std::size_t round = 0; round < kRounds; ++round) {
    REQUIRE(ring.try_publish(make(round)));
    std::shared_ptr<const Tracked> received;
    REQUIRE(ring.try_take(received));
    REQUIRE(received->id == round);
  }
  CHECK(ring.size_approx() == 0);
}

TEST_CASE("HandoffRing never destroys a payload on the consumer thread", "[stress]") {
  // THE test for this type. Producer publishes, consumer takes and retires into
  // a GarbageRing, and a janitor collects — the exact pairing Engine uses. If
  // the consumer ever released the last reference, a destructor would run there
  // and this would catch it.
  reset_tracking();

  constexpr std::size_t kCount = 2'000;
  rt::HandoffRing<Tracked, 8> ring;
  rt::GarbageRing<16> garbage;
  std::atomic<bool> producing{true};
  std::atomic<std::size_t> taken{0};
  std::atomic<std::size_t> out_of_order{0};

  std::thread consumer{[&] {
    t_is_consumer_thread = true;

    // The consumer's held handle is a long-lived variable rather than a local
    // inside the loop, for the same reason Engine::m_retiring is a member: a
    // local going out of scope while non-null would destroy here.
    std::shared_ptr<const Tracked> held;
    std::size_t expected = 0;
    while (producing.load(std::memory_order_relaxed) || ring.size_approx() > 0 || held != nullptr) {
      if (held == nullptr && ring.try_take(held)) {
        if (held->id != expected) {
          out_of_order.fetch_add(1, std::memory_order_relaxed);
        }
        ++expected;
        taken.fetch_add(1, std::memory_order_relaxed);
      }
      if (held != nullptr && garbage.retire(std::move(held))) {
        // held is null again; next iteration may take another.
      }
    }
  }};

  std::thread janitor{[&] {
    while (producing.load(std::memory_order_relaxed) || garbage.size_approx() > 0 ||
           taken.load(std::memory_order_relaxed) < kCount) {
      static_cast<void>(garbage.collect());
    }
  }};

  for (std::size_t index = 0; index < kCount; ++index) {
    // Constructed ONCE per index and retried by copying the handle, not by
    // calling make() again. Building a fresh payload on every retry would
    // construct and destroy thousands of extras on this thread, and the
    // destruction count below -- which is what proves every payload actually
    // died somewhere -- would stop meaning anything.
    const std::shared_ptr<const Tracked> payload = make(index);
    while (!ring.try_publish(std::shared_ptr<const Tracked>{payload})) {
      // Refused because the ring is full; we still own it, so just try again.
    }
  }
  producing.store(false, std::memory_order_relaxed);

  consumer.join();
  janitor.join();
  static_cast<void>(garbage.collect());

  CHECK(taken.load(std::memory_order_relaxed) == kCount);
  CHECK(out_of_order.load(std::memory_order_relaxed) == 0);
  CHECK(Tracked::s_destroyed.load(std::memory_order_relaxed) == kCount);

  // The whole point.
  CHECK(Tracked::s_destroyed_on_consumer.load(std::memory_order_relaxed) == 0);

  reset_tracking();
}
