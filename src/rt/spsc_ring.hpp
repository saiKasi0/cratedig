#ifndef CRATEDIG_RT_SPSC_RING_HPP
#define CRATEDIG_RT_SPSC_RING_HPP

#include "rt/arch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <span>
#include <type_traits>

namespace rt {

// Wait-free single-producer / single-consumer ring.
//
// Exactly one thread may call the push operations and exactly one (different)
// thread may call the pop operations. This is the only channel the control lane
// uses to reach the audio lane (PadEvent, ParamChange) and back.
//
// Storage is a fixed inline array, so no operation allocates, locks, throws, or
// touches the heap — it is safe to call from the audio callback under RT_SCOPE().
//
// Correctness authority is the TSan stress test, not the single-threaded unit
// tests (docs/TESTING.md).
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>,
                "SpscRing<T>: T must be trivially copyable — a slot is overwritten by "
                "assignment with no destructor call. For owning types use rt::GarbageRing.");
  static_assert(std::has_single_bit(Capacity), "SpscRing: Capacity must be a power of two");
  static_assert(Capacity >= 2, "SpscRing: Capacity must be at least 2");

 public:
  using ValueType = T;

  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;
  ~SpscRing() = default;

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  // --- producer side -------------------------------------------------------

  [[nodiscard]] bool try_push(const T& item) noexcept {
    // relaxed: this thread is the only writer of m_write_index, so its own most
    // recent value is always visible to it without synchronization.
    const std::size_t write = m_write_index.load(std::memory_order_relaxed);

    if (write - m_read_cache == Capacity) {
      // acquire: we are about to overwrite a slot, so we must see the consumer's
      // reads of that slot as completed. Pairs with the release store in pop.
      m_read_cache = m_read_index.load(std::memory_order_acquire);
      if (write - m_read_cache == Capacity) {
        return false;
      }
    }

    m_buffer[write & kIndexMask] = item;

    // release: publishes the slot write above to the consumer's acquire load.
    m_write_index.store(write + 1, std::memory_order_release);
    return true;
  }

  // Pushes as many items as fit. Returns the number accepted, which may be 0 or
  // a partial prefix — the caller decides whether a partial batch is acceptable.
  std::size_t try_push(std::span<const T> items) noexcept {
    const std::size_t write = m_write_index.load(std::memory_order_relaxed);  // sole writer

    std::size_t free_slots = Capacity - (write - m_read_cache);
    if (free_slots < items.size()) {
      // acquire: see the consumer's completed reads before reusing slots.
      m_read_cache = m_read_index.load(std::memory_order_acquire);
      free_slots = Capacity - (write - m_read_cache);
    }

    const std::size_t count = std::min(free_slots, items.size());
    for (std::size_t i = 0; i < count; ++i) {
      m_buffer[(write + i) & kIndexMask] = items[i];
    }

    if (count != 0) {
      // release: publishes every slot written above.
      m_write_index.store(write + count, std::memory_order_release);
    }
    return count;
  }

  // --- consumer side -------------------------------------------------------

  [[nodiscard]] bool try_pop(T& out) noexcept {
    // relaxed: this thread is the only writer of m_read_index.
    const std::size_t read = m_read_index.load(std::memory_order_relaxed);

    if (read == m_write_cache) {
      // acquire: pairs with the producer's release store, making the slot
      // contents written before that store visible to this thread.
      m_write_cache = m_write_index.load(std::memory_order_acquire);
      if (read == m_write_cache) {
        return false;
      }
    }

    out = m_buffer[read & kIndexMask];

    // release: tells the producer this slot is free to overwrite, and orders our
    // read of it before that becomes visible.
    m_read_index.store(read + 1, std::memory_order_release);
    return true;
  }

  // Pops up to out.size() items into the front of out. Returns the number popped.
  std::size_t try_pop(std::span<T> out) noexcept {
    const std::size_t read = m_read_index.load(std::memory_order_relaxed);  // sole writer

    std::size_t available = m_write_cache - read;
    if (available < out.size()) {
      // acquire: pairs with the producer's release store (see try_pop above).
      m_write_cache = m_write_index.load(std::memory_order_acquire);
      available = m_write_cache - read;
    }

    const std::size_t count = std::min(available, out.size());
    for (std::size_t i = 0; i < count; ++i) {
      out[i] = m_buffer[(read + i) & kIndexMask];
    }

    if (count != 0) {
      // release: frees every slot read above for the producer to reuse.
      m_read_index.store(read + count, std::memory_order_release);
    }
    return count;
  }

  // --- diagnostics ---------------------------------------------------------

  // Approximate by nature: either side may move on before the caller acts on the
  // answer. For metrics and assertions only — never for flow control.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    // acquire on the write index then relaxed on the read index: this can only
    // over-report (a concurrent pop makes it stale-high), never claim data that
    // was never published.
    const std::size_t write = m_write_index.load(std::memory_order_acquire);
    const std::size_t read = m_read_index.load(std::memory_order_relaxed);
    return write - read;
  }

  [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

 private:
  static constexpr std::size_t kIndexMask = Capacity - 1;

  // Indices are monotonic and never reduced modulo Capacity; only the *offset*
  // into the buffer is masked. That makes full and empty unambiguous without
  // wasting a slot: the distance (write - read) is the exact element count.
  //
  // Unsigned wraparound at SIZE_MAX is correct rather than merely tolerated:
  // 2^64 is an exact multiple of any power-of-two Capacity, so the difference
  // and the masked offset both stay consistent across the wrap. Reaching it
  // requires 2^64 operations regardless.
  std::array<T, Capacity> m_buffer{};

  // Producer-owned line: the published write index, plus this thread's cached
  // view of the consumer's index. The cache is what makes the common case
  // wait-free *and* quiet — the shared read index is only touched when the ring
  // looks full, instead of on every single push.
  alignas(kCacheLine) std::atomic<std::size_t> m_write_index{0};
  std::size_t m_read_cache{0};

  // Consumer-owned line, mirror image of the above.
  alignas(kCacheLine) std::atomic<std::size_t> m_read_index{0};
  std::size_t m_write_cache{0};

  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "SpscRing requires lock-free size_t atomics");
};

}  // namespace rt

#endif  // CRATEDIG_RT_SPSC_RING_HPP
