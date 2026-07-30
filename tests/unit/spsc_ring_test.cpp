#include "rt/spsc_ring.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

// A stand-in for the real message types (PadEvent, ParamChange): trivially
// copyable, small, no ownership.
struct PadEvent {
  std::uint32_t pad;
  std::uint32_t frame_offset;
  float velocity;
};

// Batch size for the bulk stress test. Namespace scope because std::min binds it
// by const reference, which odr-uses it inside the producer lambda.
constexpr std::size_t kStressBatch = 7;  // coprime with capacity, so batches straddle

}  // namespace

TEST_CASE("SpscRing starts empty", "[unit]") {
  rt::SpscRing<PadEvent, 8> ring;
  PadEvent event{};

  CHECK(ring.size_approx() == 0);
  CHECK(ring.empty_approx());
  CHECK_FALSE(ring.try_pop(event));
}

TEST_CASE("SpscRing round-trips a value", "[unit]") {
  rt::SpscRing<PadEvent, 8> ring;

  REQUIRE(ring.try_push(PadEvent{.pad = 3, .frame_offset = 128, .velocity = 0.75F}));
  CHECK(ring.size_approx() == 1);

  PadEvent event{};
  REQUIRE(ring.try_pop(event));
  CHECK(event.pad == 3);
  CHECK(event.frame_offset == 128);
  CHECK(event.velocity == 0.75F);
  CHECK(ring.empty_approx());
}

TEST_CASE("SpscRing uses its full capacity and then refuses", "[unit]") {
  // Monotonic indices mean all Capacity slots are usable — no sacrificial slot
  // to disambiguate full from empty.
  constexpr std::size_t kCapacity = 8;
  rt::SpscRing<std::uint64_t, kCapacity> ring;

  for (std::uint64_t i = 0; i < kCapacity; ++i) {
    INFO("pushing element " << i);
    REQUIRE(ring.try_push(i));
  }
  CHECK(ring.size_approx() == kCapacity);
  CHECK_FALSE(ring.try_push(999));

  std::uint64_t value = 0;
  for (std::uint64_t i = 0; i < kCapacity; ++i) {
    INFO("popping element " << i);
    REQUIRE(ring.try_pop(value));
    CHECK(value == i);
  }
  CHECK(ring.empty_approx());
  CHECK_FALSE(ring.try_pop(value));
}

TEST_CASE("SpscRing survives many wraparounds", "[unit]") {
  constexpr std::size_t kCapacity = 4;
  rt::SpscRing<std::uint64_t, kCapacity> ring;

  // Far more cycles than the capacity, at a stride that keeps the ring partly
  // full so reads and writes land on different offsets each lap.
  std::uint64_t next_written = 0;
  std::uint64_t next_expected = 0;
  for (int cycle = 0; cycle < 1000; ++cycle) {
    REQUIRE(ring.try_push(next_written++));
    REQUIRE(ring.try_push(next_written++));
    REQUIRE(ring.try_push(next_written++));

    std::uint64_t value = 0;
    for (int i = 0; i < 3; ++i) {
      REQUIRE(ring.try_pop(value));
      REQUIRE(value == next_expected++);
    }
  }
  CHECK(ring.empty_approx());
}

TEST_CASE("SpscRing bulk push accepts a partial batch", "[unit]") {
  constexpr std::size_t kCapacity = 8;
  rt::SpscRing<std::uint64_t, kCapacity> ring;

  const std::array<std::uint64_t, 6> first{0, 1, 2, 3, 4, 5};
  CHECK(ring.try_push(std::span<const std::uint64_t>{first}) == 6);

  // Only two slots remain, so a four-element batch is truncated rather than
  // rejected — the caller decides what to do with the remainder.
  const std::array<std::uint64_t, 4> second{6, 7, 8, 9};
  CHECK(ring.try_push(std::span<const std::uint64_t>{second}) == 2);
  CHECK(ring.size_approx() == kCapacity);

  const std::array<std::uint64_t, 1> overflow{99};
  CHECK(ring.try_push(std::span<const std::uint64_t>{overflow}) == 0);

  std::array<std::uint64_t, kCapacity> drained{};
  CHECK(ring.try_pop(std::span<std::uint64_t>{drained}) == kCapacity);
  for (std::uint64_t i = 0; i < kCapacity; ++i) {
    CHECK(drained[i] == i);
  }
}

TEST_CASE("SpscRing bulk pop returns only what is available", "[unit]") {
  rt::SpscRing<std::uint64_t, 8> ring;

  const std::array<std::uint64_t, 3> pushed{10, 11, 12};
  REQUIRE(ring.try_push(std::span<const std::uint64_t>{pushed}) == 3);

  std::array<std::uint64_t, 8> out{};
  CHECK(ring.try_pop(std::span<std::uint64_t>{out}) == 3);
  CHECK(out[0] == 10);
  CHECK(out[1] == 11);
  CHECK(out[2] == 12);

  CHECK(ring.try_pop(std::span<std::uint64_t>{out}) == 0);
}

TEST_CASE("SpscRing bulk operations wrap correctly", "[unit]") {
  constexpr std::size_t kCapacity = 8;
  rt::SpscRing<std::uint64_t, kCapacity> ring;

  // Offset the indices so the next batch straddles the end of the buffer.
  const std::array<std::uint64_t, 6> priming{0, 1, 2, 3, 4, 5};
  REQUIRE(ring.try_push(std::span<const std::uint64_t>{priming}) == 6);
  std::array<std::uint64_t, 6> discard{};
  REQUIRE(ring.try_pop(std::span<std::uint64_t>{discard}) == 6);

  const std::array<std::uint64_t, 5> straddling{100, 101, 102, 103, 104};
  REQUIRE(ring.try_push(std::span<const std::uint64_t>{straddling}) == 5);

  std::array<std::uint64_t, 5> out{};
  REQUIRE(ring.try_pop(std::span<std::uint64_t>{out}) == 5);
  for (std::size_t i = 0; i < out.size(); ++i) {
    CHECK(out[i] == 100 + i);
  }
}

// The single-threaded cases above prove semantics; only this one can prove the
// memory ordering, and only when run under TSan (docs/TESTING.md).
TEST_CASE("SpscRing transfers every element across threads", "[unit][stress]") {
  constexpr std::size_t kCapacity = 64;
  constexpr std::uint64_t kMessages = 500'000;

  rt::SpscRing<std::uint64_t, kCapacity> ring;

  std::thread producer([&ring] {
    for (std::uint64_t i = 0; i < kMessages; ++i) {
      while (!ring.try_push(i)) {
        std::this_thread::yield();
      }
    }
  });

  std::uint64_t received = 0;
  std::uint64_t checksum = 0;
  bool in_order = true;

  while (received < kMessages) {
    std::uint64_t value = 0;
    if (ring.try_pop(value)) {
      // Strict sequence: an SPSC ring must never reorder, duplicate, or drop.
      if (value != received) {
        in_order = false;
      }
      checksum += value;
      ++received;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();

  constexpr std::uint64_t kExpectedChecksum = (kMessages - 1) * kMessages / 2;
  CHECK(in_order);
  CHECK(received == kMessages);
  CHECK(checksum == kExpectedChecksum);
  CHECK(ring.empty_approx());
}

TEST_CASE("SpscRing bulk transfer is lossless across threads", "[unit][stress]") {
  constexpr std::size_t kCapacity = 64;
  constexpr std::uint64_t kMessages = 200'000;

  rt::SpscRing<std::uint64_t, kCapacity> ring;

  std::thread producer([&ring] {
    std::uint64_t next = 0;
    std::array<std::uint64_t, kStressBatch> batch{};
    while (next < kMessages) {
      const std::size_t chunk =
          std::min<std::size_t>(kStressBatch, static_cast<std::size_t>(kMessages - next));
      for (std::size_t i = 0; i < chunk; ++i) {
        batch[i] = next + i;
      }
      std::size_t offset = 0;
      while (offset < chunk) {
        offset +=
            ring.try_push(std::span<const std::uint64_t>{batch.data() + offset, chunk - offset});
        if (offset < chunk) {
          std::this_thread::yield();
        }
      }
      next += chunk;
    }
  });

  std::uint64_t received = 0;
  bool in_order = true;
  std::array<std::uint64_t, kStressBatch> out{};

  while (received < kMessages) {
    const std::size_t count = ring.try_pop(std::span<std::uint64_t>{out});
    if (count == 0) {
      std::this_thread::yield();
      continue;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (out[i] != received + i) {
        in_order = false;
      }
    }
    received += count;
  }
  producer.join();

  CHECK(in_order);
  CHECK(received == kMessages);
  CHECK(ring.empty_approx());
}
