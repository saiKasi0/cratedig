#ifndef CRATEDIG_RT_GARBAGE_RING_HPP
#define CRATEDIG_RT_GARBAGE_RING_HPP

#include "rt/arch.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace rt {

// Where the audio thread puts things it must stop referencing but must not
// destroy (CLAUDE.md rule 5: "Drop the last std::shared_ptr reference to
// anything" is forbidden on the audio thread — freeing is I/O-adjacent work that
// can take a lock inside the allocator).
//
// The audio thread retires a shared_ptr here; the janitor thread collects it and
// lets the destructor run on its own time.
//
// This is deliberately NOT SpscRing<std::shared_ptr<void>>: SpscRing requires a
// trivially copyable element because it overwrites slots by assignment without
// running a destructor. A shared_ptr is neither trivially copyable nor safe to
// overwrite that way. The invariant that makes this ring safe is different in
// kind — see retire() below — so it gets its own type.
template <std::size_t Capacity>
class GarbageRing {
  static_assert(std::has_single_bit(Capacity), "GarbageRing: Capacity must be a power of two");
  static_assert(Capacity >= 2, "GarbageRing: Capacity must be at least 2");

 public:
  GarbageRing() = default;
  GarbageRing(const GarbageRing&) = delete;
  GarbageRing& operator=(const GarbageRing&) = delete;
  GarbageRing(GarbageRing&&) = delete;
  GarbageRing& operator=(GarbageRing&&) = delete;
  ~GarbageRing() = default;

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  // AUDIO THREAD. Hands ownership of sp to the janitor. Allocation-free,
  // lock-free, and never runs a destructor on this thread.
  //
  // Converting shared_ptr<T> to shared_ptr<const void> shares the existing
  // control block — it is a pointer copy plus a refcount bump, with no
  // allocation. The destination slot is guaranteed empty (see the class
  // invariant), so move-assigning into it cannot release a previous reference
  // here either.
  //
  // The slot type is shared_ptr<const void>, not shared_ptr<void>: almost
  // everything retired here is a shared_ptr<const Something> (samples are
  // immutable once published), and that does not convert to shared_ptr<void>
  // because it would cast away const. Type erasure keeps the original deleter
  // either way, so the true type is still destroyed correctly on the janitor.
  //
  // Returns false when the ring is full. The caller then STILL OWNS sp and must
  // keep it alive and retry on a later block. Dropping it on the floor would
  // destroy it on the audio thread, which is precisely what this type exists to
  // prevent.
  template <typename T>
  [[nodiscard]] bool retire(std::shared_ptr<T>&& sp) noexcept {
    if (sp == nullptr) {
      return true;  // nothing to retire; treat as success so callers need no special case
    }

    // relaxed: the audio thread is the only writer of m_write_index.
    const std::size_t write = m_write_index.load(std::memory_order_relaxed);

    // acquire: we must observe the janitor's completed collection of this slot
    // (which left it empty) before we move into it. Pairs with the release store
    // in collect().
    if (write - m_read_index.load(std::memory_order_acquire) == Capacity) {
      // relaxed: a monotonic diagnostic counter; no other state is ordered by it.
      m_overflow_count.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    m_slots[write & kIndexMask] = std::move(sp);

    // release: publishes the slot's new contents to the janitor's acquire load.
    m_write_index.store(write + 1, std::memory_order_release);
    return true;
  }

  // JANITOR THREAD. Releases every retired reference. Destructors — and any
  // resulting free() — run here, on this thread. Returns how many were released.
  std::size_t collect() noexcept {
    std::size_t released = 0;

    // relaxed: the janitor is the only writer of m_read_index.
    std::size_t read = m_read_index.load(std::memory_order_relaxed);

    // acquire: pairs with retire()'s release store, making the retired
    // shared_ptr's contents visible here.
    const std::size_t write = m_write_index.load(std::memory_order_acquire);

    while (read != write) {
      {
        // Moving out leaves the slot empty (a moved-from shared_ptr is
        // guaranteed null), then this local's destructor runs the deleter here
        // on the janitor thread. Emptying the slot before publishing the new
        // read index is the invariant that lets retire() move into its target
        // slot without ever releasing a reference on the audio thread.
        const std::shared_ptr<const void> collected = std::move(m_slots[read & kIndexMask]);
      }
      ++read;
      ++released;

      // release: publishes "this slot is empty" to the audio thread's acquire
      // load in retire(). Inside the loop so a full ring drains incrementally
      // rather than making the audio thread wait for the whole batch.
      m_read_index.store(read, std::memory_order_release);
    }

    return released;
  }

  // Number of retire() calls rejected because the ring was full. Non-zero means
  // the janitor is not keeping up, or the ring is undersized.
  [[nodiscard]] std::uint64_t overflow_count() const noexcept {
    return m_overflow_count.load(std::memory_order_relaxed);  // diagnostic only
  }

  // Approximate: the janitor may collect between the reads. Diagnostics only.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::size_t write = m_write_index.load(std::memory_order_acquire);
    const std::size_t read = m_read_index.load(std::memory_order_relaxed);
    return write - read;
  }

 private:
  static constexpr std::size_t kIndexMask = Capacity - 1;

  // Monotonic indices, masked only to find the slot — same scheme as SpscRing,
  // so the full capacity is usable and full/empty are unambiguous.
  std::array<std::shared_ptr<const void>, Capacity> m_slots{};

  alignas(kCacheLine) std::atomic<std::size_t> m_write_index{0};
  alignas(kCacheLine) std::atomic<std::size_t> m_read_index{0};
  std::atomic<std::uint64_t> m_overflow_count{0};

  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "GarbageRing requires lock-free size_t atomics");
};

}  // namespace rt

#endif  // CRATEDIG_RT_GARBAGE_RING_HPP
