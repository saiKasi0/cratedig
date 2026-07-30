#ifndef CRATEDIG_RT_HANDOFF_RING_HPP
#define CRATEDIG_RT_HANDOFF_RING_HPP

#include "rt/arch.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace rt {

// How the control lane changes what the audio lane is using, while it is using
// it (docs/ARCHITECTURE.md, "Live reconfiguration: one problem, one protocol").
//
// The control thread builds a whole new immutable object -- a PadConfig, later a
// recorded Sample (M6) or a plugin chain (M8) -- and publishes an owning handle
// to it here. The audio thread takes the handle, swaps it into place, and hands
// whatever it displaced to the GarbageRing. Nothing is constructed or destroyed
// on the audio thread at any point.
//
// GarbageRing runs the same direction of travel backwards, and the two are the
// matching halves of one mechanism: this ring carries new objects TOWARDS the
// audio thread, that one carries retired objects AWAY from it. Neither is
// SpscRing, for the reason stated there -- SpscRing overwrites slots by
// assignment and never runs a destructor, which a shared_ptr slot cannot
// tolerate.
//
// THE INVARIANT, and why it differs from GarbageRing's
// ---------------------------------------------------
// GarbageRing keeps destruction off the *producer* (audio) by having the
// consumer empty each slot. Here the roles are reversed -- the *consumer* is the
// audio thread -- so the discipline moves with it:
//
//   - try_take() MOVES OUT of the slot, so the slot is left empty and the
//     consumer releases nothing. It requires its `out` parameter to already be
//     empty, so the move-assignment into it cannot release a reference either.
//   - try_publish() therefore always moves into an already-empty slot, which is
//     asserted rather than assumed.
//
// The consequence for callers is the part worth internalising: after a
// successful take, the AUDIO THREAD OWNS A REFERENCE and must put it somewhere
// that is not its own stack. That somewhere is the GarbageRing -- and when the
// garbage ring is full, the handle has to survive to the next block, which is
// why Engine holds it in a member rather than a local (see Engine::m_retiring).
template <typename T, std::size_t Capacity>
class HandoffRing {
  static_assert(std::has_single_bit(Capacity), "HandoffRing: Capacity must be a power of two");
  static_assert(Capacity >= 2, "HandoffRing: Capacity must be at least 2");

 public:
  using Handle = std::shared_ptr<const T>;

  HandoffRing() = default;
  HandoffRing(const HandoffRing&) = delete;
  HandoffRing& operator=(const HandoffRing&) = delete;
  HandoffRing(HandoffRing&&) = delete;
  HandoffRing& operator=(HandoffRing&&) = delete;
  ~HandoffRing() = default;

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  // CONTROL THREAD. Publishes `value` to the audio thread.
  //
  // Returns false when the ring is full, in which case THE CALLER STILL OWNS
  // `value` -- same contract as GarbageRing::retire, and for the same reason:
  // silently dropping it would make an edit vanish with no way to notice.
  // Blocking instead would put the audio thread's schedule into the UI.
  //
  // A full ring means the control thread is publishing faster than blocks are
  // being rendered, which for pad edits driven by a keyboard cannot happen; a
  // non-zero rejected_count() means either the stream has stopped or something
  // is publishing in a loop.
  [[nodiscard]] bool try_publish(Handle&& value) noexcept {
    if (value == nullptr) {
      // Nothing to publish. Refused rather than counted: a null handle carries
      // no pad index, so the audio thread could not act on it even if it arrived.
      return false;
    }

    // relaxed: the control thread is the only writer of m_write_index.
    const std::size_t write = m_write_index.load(std::memory_order_relaxed);

    // acquire: we must observe the audio thread's completed take of this slot
    // (which left it empty) before moving into it. Pairs with try_take's release.
    if (write - m_read_index.load(std::memory_order_acquire) == Capacity) {
      m_rejected_count.fetch_add(1, std::memory_order_relaxed);  // diagnostic only
      return false;
    }

    assert(m_slots[write & kIndexMask] == nullptr &&
           "HandoffRing: publishing into a slot the consumer has not emptied");
    m_slots[write & kIndexMask] = std::move(value);

    // release: publishes the slot contents to the audio thread's acquire load.
    m_write_index.store(write + 1, std::memory_order_release);
    return true;
  }

  // AUDIO THREAD. Moves the oldest published handle into `out`.
  //
  // `out` MUST be empty; otherwise this would release its previous reference
  // here, on the audio thread. That is the one thing the whole mechanism exists
  // to prevent, so it is asserted rather than handled -- a caller that got this
  // wrong wants to find out in a debug build, not to have it quietly papered
  // over in release.
  //
  // Returns false when nothing has been published, leaving `out` empty.
  [[nodiscard]] bool try_take(Handle& out) noexcept {
    assert(out == nullptr && "HandoffRing::try_take: out must be empty (it would release here)");

    // relaxed: the audio thread is the only writer of m_read_index.
    const std::size_t read = m_read_index.load(std::memory_order_relaxed);

    // acquire: pairs with try_publish's release store, making the published
    // handle's contents visible to this thread.
    if (read == m_write_index.load(std::memory_order_acquire)) {
      return false;
    }

    // Moving out leaves the slot empty (a moved-from shared_ptr is guaranteed
    // null) and releases nothing here. Emptying the slot before publishing the
    // new read index is what lets try_publish move into its target slot without
    // the control thread ever racing a live handle.
    out = std::move(m_slots[read & kIndexMask]);

    // release: publishes "this slot is empty" to try_publish's acquire load.
    m_read_index.store(read + 1, std::memory_order_release);
    return true;
  }

  // Number of try_publish calls rejected because the ring was full.
  [[nodiscard]] std::uint64_t rejected_count() const noexcept {
    return m_rejected_count.load(std::memory_order_relaxed);  // diagnostic only
  }

  // Approximate: the audio thread may take between the two reads. Diagnostics
  // and assertions only, never flow control.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::size_t write = m_write_index.load(std::memory_order_acquire);
    const std::size_t read = m_read_index.load(std::memory_order_relaxed);
    return write - read;
  }

 private:
  static constexpr std::size_t kIndexMask = Capacity - 1;

  // Monotonic indices masked only when indexing, exactly as in SpscRing and
  // GarbageRing: the whole capacity is usable and full/empty are unambiguous
  // without a sacrificial slot.
  std::array<Handle, Capacity> m_slots{};

  alignas(kCacheLine) std::atomic<std::size_t> m_write_index{0};
  alignas(kCacheLine) std::atomic<std::size_t> m_read_index{0};
  std::atomic<std::uint64_t> m_rejected_count{0};

  static_assert(std::atomic<std::size_t>::is_always_lock_free,
                "HandoffRing requires lock-free size_t atomics");
};

}  // namespace rt

#endif  // CRATEDIG_RT_HANDOFF_RING_HPP
