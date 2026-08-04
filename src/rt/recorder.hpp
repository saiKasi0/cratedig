#ifndef CRATEDIG_RT_RECORDER_HPP
#define CRATEDIG_RT_RECORDER_HPP

#include "rt/spsc_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rt {

// Capture, on the audio thread, into storage somebody else owns.
//
// THE PROBLEM. A take has no length. Somebody drops a needle and stops when the
// break ends, which might be four seconds or four minutes, and the audio thread
// cannot grow a buffer to find out -- growing means allocating, and allocating
// in the callback is the one thing this whole codebase is built not to do.
//
// THE SHAPE. A pool of fixed chunks, preallocated by the control thread, passed
// to the audio thread and back through two SPSC rings:
//
//     control --[ empty ]--> audio        audio fills a chunk
//     control <--[ full ]--  audio        and hands it back full
//
// The audio thread never owns more than one chunk at a time and never allocates
// one; the control thread appends what comes back to a growing take and returns
// the chunk to the empty ring. Recording length is bounded by disk and patience
// rather than by anything here. If the control thread falls behind far enough
// that the pool empties, the audio thread DROPS FRAMES AND COUNTS THEM -- see
// dropped_frames(), and read the note there before treating it as a detail.
//
// This is the same division of labour as the pad handoff (rt/handoff_ring.hpp)
// pointed the other way: there the control thread builds something and the audio
// thread adopts it, here the audio thread fills something and the control thread
// collects it. Both exist so that no audio-thread operation can reach the heap.

// What the recorder is listening to.
enum class RecordSource : std::uint8_t {
  // The duplex stream's input: a turntable, a mic, whatever is plugged in.
  kInput = 0,

  // The master bus -- the sampler resampling itself. Same tap point as the
  // output, so it captures the mixer, the limiter and the metronome, which is
  // what "record what you hear" has to mean to be worth having.
  kMaster,
};

enum class RecordState : std::uint8_t {
  kIdle = 0,

  // Listening but not keeping. Either waiting for a punch-in, or watching for
  // the source to cross a threshold.
  kArmed,

  kRecording,
};

// An instruction for the recorder, control -> audio.
//
// A VALUE MESSAGE rather than a handoff, for the reason the master section's is:
// it owns nothing, so it travels the way a PadEvent does and leaves nothing to
// retire. The recorder's own methods are audio-thread-only; this is how the
// control thread reaches them.
struct RecordCommand {
  enum class Type : std::uint8_t {
    kArm,
    kStart,
    kStop,
  };

  Type type = Type::kStop;
  RecordSource source = RecordSource::kInput;

  // Linear amplitude, and only meaningful for kArm. Zero means "wait for a
  // punch-in" rather than "trigger on silence".
  float threshold = 0.0F;

  std::uint32_t preroll_frames = 0;
};

// The widest source the recorder will take, matching io::AudioDevice::kMaxChannels.
//
// Here as its own constant rather than including that header: src/rt/ depends on
// nothing outside src/rt/ (CLAUDE.md), and the device layer is exactly the thing
// it must not know about. The static_assert that keeps the two in step lives in
// src/io/, which is allowed to see both.
inline constexpr std::size_t kMaxRecordChannels = 8;

// One slice of a take.
//
// A view rather than storage: the frames live in one flat allocation the owner
// made, so a chunk is a pointer, a length and a fill level. That keeps the pool
// contiguous -- one allocation for a whole recording session -- and keeps this
// struct small enough to be worth moving around by value.
struct RecordChunk {
  // Planar, `capacity` frames per channel: channel c starts at data + c * capacity.
  // The same layout as rt::Sample and as the mixer graph, so assembling a take
  // is a copy per channel rather than a de-interleave.
  float* data = nullptr;
  std::size_t capacity = 0;
  std::uint16_t channels = 0;

  // How many of those frames are audio. Written by the audio thread while it is
  // filling, read by the control thread after the chunk comes back through the
  // full ring -- which is what makes the read safe, since the ring's release
  // store publishes this alongside the samples.
  std::uint32_t frames = 0;

  [[nodiscard]] std::span<const float> channel(std::uint16_t index) const noexcept {
    assert(index < channels && "RecordChunk: channel out of range");
    return std::span<const float>{data + (static_cast<std::size_t>(index) * capacity), frames};
  }

  [[nodiscard]] float* mutable_channel(std::uint16_t index) noexcept {
    assert(index < channels && "RecordChunk: channel out of range");
    return data + (static_cast<std::size_t>(index) * capacity);
  }
};

class Recorder {
 public:
  // Deeper than any pool it will be given, so a push into either ring cannot
  // fail: every chunk is in exactly one of the two rings or in the audio
  // thread's hand, so neither ring can ever hold more than the pool size.
  // A full ring here would mean a LOST chunk rather than a dropped frame, which
  // is a different and much worse failure, so it is designed out instead of
  // handled.
  static constexpr std::size_t kRingCapacity = 64;

  Recorder() = default;
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;
  Recorder(Recorder&&) = delete;
  Recorder& operator=(Recorder&&) = delete;
  ~Recorder() = default;

  // CONTROL THREAD, and only while no audio thread is running -- the engine
  // constructor. Hands over the pool and the pre-roll buffer.
  //
  // `pool` must be smaller than kRingCapacity. `preroll` is planar like a chunk,
  // `preroll_frames` per channel, and may be empty for no pre-roll at all.
  void reset(std::span<RecordChunk> pool, std::span<float> preroll, std::size_t preroll_frames,
             std::uint16_t channels) noexcept {
    assert(pool.size() < kRingCapacity && "Recorder: pool must fit in the rings");
    assert(channels <= kMaxRecordChannels && "Recorder: too many channels");
    assert(preroll.size() >= preroll_frames * channels && "Recorder: pre-roll storage too small");

    m_pool = pool;
    m_preroll = preroll;
    m_preroll_capacity = preroll_frames;
    m_channels = channels;
    m_current = kNoChunk;

    for (std::size_t index = 0; index < pool.size(); ++index) {
      m_pool[index].frames = 0;
      static_cast<void>(m_empty.try_push(static_cast<std::uint16_t>(index)));
    }
  }

  // --- audio thread --------------------------------------------------------

  // Listen, but keep nothing yet.
  //
  // `threshold` is a LINEAR amplitude. Zero means "wait for a punch-in"; above
  // zero means start the moment the source reaches it, which is the mode that
  // matters when the other hand is on a turntable.
  //
  // `preroll_frames` is how much of what came BEFORE the trigger to keep. Not a
  // refinement: a threshold trigger cannot begin before the threshold, so a take
  // opens on a STEP from nothing straight to the trigger level, which is a click
  // at the head of every take. Measured in tests/unit/recorder_test.cpp on a
  // 20 ms linear swell triggering at -20 dBFS: the take opens 2.0 ms into the
  // swell at a tenth of full scale. Pre-roll is what makes it open at silence,
  // from the real audio that preceded it.
  //
  // THE PREVIOUS TAKE MUST ALREADY BE COLLECTED. This resets the counters but
  // cannot reach chunks already in flight, so arming over an undrained take
  // would splice the two together. The caller owns that ordering -- in this
  // codebase, engine::Engine, which refuses to arm until its own collection has
  // seen the recorder go idle.
  void arm(RecordSource source, float threshold, std::size_t preroll_frames) noexcept {
    m_source = source;
    m_threshold = threshold;
    m_preroll_frames = std::min(preroll_frames, m_preroll_capacity);
    m_preroll_write = 0;
    m_preroll_filled = 0;
    begin_take();
    m_state.store(static_cast<std::uint8_t>(RecordState::kArmed), std::memory_order_release);
  }

  // Punch in now, from idle or from armed.
  //
  // From ARMED this still flushes the pre-roll, and that is deliberate: the
  // person armed the recorder, watched the meter, and hit the key when they
  // heard the thing start -- which is always a little late. Giving them back
  // what they asked to keep is the point of having asked.
  void start(RecordSource source) noexcept {
    if (state() != RecordState::kArmed) {
      m_source = source;
      m_preroll_frames = 0;
      begin_take();
    }
    m_threshold = 0.0F;
    engage();
  }

  // Stop keeping. The partially filled chunk goes back so the tail is not lost.
  void stop() noexcept {
    if (state() == RecordState::kRecording) {
      flush_chunk();
    }
    m_threshold = 0.0F;
    // release: everything above -- the last chunk's push, the frame counts --
    // must be visible to a control thread that has seen this store and then
    // drains. It is what makes "idle means the take is complete" true.
    m_state.store(static_cast<std::uint8_t>(RecordState::kIdle), std::memory_order_release);
  }

  // AUDIO THREAD, once per block. Meters the source, and keeps it if we are
  // keeping.
  //
  // `channels` is the tapped source in the engine's planar layout. A source
  // narrower than the pool -- a mono mic into a stereo take -- is spread by
  // repeating its last channel, so the take is mono-in-both rather than silent
  // on one side. A source wider than the pool has its extra channels metered and
  // dropped.
  void capture(std::span<const float* const> channels, std::size_t num_frames) noexcept {
    m_peak.store(measure_peak(channels, num_frames), std::memory_order_relaxed);  // display only

    const RecordState now = state();
    if (now == RecordState::kIdle || num_frames == 0) {
      return;
    }

    if (channels.empty()) {
      // Asked to record and handed nothing. Counting it as dropped rather than
      // ignoring it is what lets the interface say the input produced no audio,
      // instead of presenting a take that is quietly shorter than the take.
      if (now == RecordState::kRecording) {
        m_dropped.fetch_add(num_frames, std::memory_order_relaxed);  // diagnostic only
      }
      return;
    }

    if (now == RecordState::kArmed) {
      if (m_threshold <= 0.0F) {
        write_preroll(channels, 0, num_frames);
        return;
      }

      // PER SAMPLE, not per block. A block-granular trigger would start the take
      // at the block boundary, which at 256 frames is 5.3 ms of slop on the one
      // feature whose entire job is to catch a moment.
      const std::size_t crossing = first_crossing(channels, num_frames);
      if (crossing == num_frames) {
        write_preroll(channels, 0, num_frames);
        return;
      }
      write_preroll(channels, 0, crossing);
      engage();
      write_frames(channels, crossing, num_frames - crossing);
      return;
    }

    write_frames(channels, 0, num_frames);
  }

  // AUDIO THREAD. What the recorder is tapping, so the caller knows which
  // buffer to hand capture().
  [[nodiscard]] RecordSource source() const noexcept { return m_source; }

  // --- control thread ------------------------------------------------------

  // A filled chunk, or false when there is none waiting. The caller must return
  // it with recycle() once it has copied what it wants.
  //
  // KNOWING WHEN A TAKE IS COMPLETE. Drain until state() reads kIdle AND you
  // hold frames_captured() frames. Both halves, and the count is the half that
  // matters: stopping at kIdle alone would mean trusting that the audio thread
  // pushed its last chunk before it published the state. It does -- see stop()
  // -- but that is an ordering nothing outside this class can observe, so a
  // later edit could reverse it and every test here would still pass. A
  // collector that counts cannot be wrong about it, and cannot be broken by an
  // edit either. Verified by the threading case in tests/unit/recorder_test.cpp.
  [[nodiscard]] bool try_collect(std::uint16_t& index) noexcept { return m_full.try_pop(index); }

  [[nodiscard]] const RecordChunk& chunk(std::uint16_t index) const noexcept {
    assert(index < m_pool.size() && "Recorder: chunk index out of range");
    return m_pool[index];
  }

  void recycle(std::uint16_t index) noexcept {
    assert(index < m_pool.size() && "Recorder: chunk index out of range");
    // Cannot fail: kRingCapacity exceeds the pool, so every chunk has a slot.
    const bool accepted = m_empty.try_push(index);
    assert(accepted && "Recorder: the empty ring must always have room");
    static_cast<void>(accepted);
  }

  // --- any thread ----------------------------------------------------------

  // acquire, paired with the release stores in the transitions above: a control
  // thread that reads kIdle here and then drains the full ring is guaranteed to
  // see every chunk of the take. Reading it relaxed would let the last chunk of
  // a take arrive after the take was declared finished.
  [[nodiscard]] RecordState state() const noexcept {
    return static_cast<RecordState>(m_state.load(std::memory_order_acquire));
  }

  [[nodiscard]] bool is_recording() const noexcept { return state() == RecordState::kRecording; }

  // Loudest absolute sample in the last block of the source, whatever the state.
  // The meter you watch before committing to a take.
  [[nodiscard]] float source_peak() const noexcept {
    return m_peak.load(std::memory_order_relaxed);  // display only
  }

  // Frames kept in the current take, pre-roll included.
  [[nodiscard]] std::uint64_t frames_captured() const noexcept {
    return m_frames.load(std::memory_order_relaxed);  // display only
  }

  // Frames the pool could not hold.
  //
  // NON-ZERO MEANS THE TAKE IS NOT WHAT WAS PLAYED. The missing frames are not
  // silence -- they are absent, so what was either side of them is now spliced
  // together, and a splice in the middle of a note is audible as a click. There
  // is no way to fix that after the fact and no honest way to hide it, so the
  // number is exposed and the interface says so.
  [[nodiscard]] std::uint64_t dropped_frames() const noexcept {
    return m_dropped.load(std::memory_order_relaxed);  // display only
  }

  // Chunks the audio thread is waiting for. Zero means the next chunk boundary
  // starts dropping frames, so this is the early warning the counter above is
  // the obituary of.
  [[nodiscard]] std::size_t chunks_available() const noexcept { return m_empty.size_approx(); }

  // Chunks filled and not yet collected. Non-zero between takes means audio from
  // the last one is still in flight, which is what makes arming again unsafe --
  // see arm().
  [[nodiscard]] std::size_t pending_chunks() const noexcept { return m_full.size_approx(); }

 private:
  static constexpr std::uint16_t kNoChunk = 0xFFFF;

  // AUDIO THREAD. Clears the counters for a new take.
  void begin_take() noexcept {
    m_frames.store(0, std::memory_order_relaxed);   // display only
    m_dropped.store(0, std::memory_order_relaxed);  // display only
  }

  // AUDIO THREAD. Arm -> recording, flushing whatever pre-roll was asked for.
  void engage() noexcept {
    // release: publishes the pre-roll's chunks, and the state a control thread
    // polls to know a threshold has fired.
    m_state.store(static_cast<std::uint8_t>(RecordState::kRecording), std::memory_order_release);
    flush_preroll();
  }

  // AUDIO THREAD. Takes a chunk from the empty ring, or reports that the pool is
  // dry.
  [[nodiscard]] bool acquire_chunk() noexcept {
    if (!m_empty.try_pop(m_current)) {
      m_current = kNoChunk;
      return false;
    }
    m_pool[m_current].frames = 0;
    return true;
  }

  // AUDIO THREAD. Hands the current chunk back, however full it is.
  //
  // AN EMPTY CHUNK IS KEPT RATHER THAN HANDED OVER, which happens whenever a
  // stop lands exactly on a chunk boundary. Pushing it would make the control
  // thread special-case a zero-frame chunk on every take; pushing it to the
  // EMPTY ring instead would put a second producer on a single-producer ring,
  // which is the race rt/spsc_ring.hpp's contract exists to forbid. Holding it
  // costs nothing: the next acquire_chunk() finds it already in hand.
  void flush_chunk() noexcept {
    if (m_current == kNoChunk || m_pool[m_current].frames == 0) {
      return;
    }
    const bool accepted = m_full.try_push(m_current);
    assert(accepted && "Recorder: the full ring must always have room");
    static_cast<void>(accepted);
    m_current = kNoChunk;
  }

  // AUDIO THREAD. Copies [from, from + count) of the source into chunks.
  void write_frames(std::span<const float* const> channels, std::size_t from,
                    std::size_t count) noexcept {
    std::size_t done = 0;
    while (done < count) {
      // A full chunk is handed over HERE, at the top, rather than after the copy
      // below. The two are equivalent while everything works and are not
      // equivalent when something is wrong: with the flush at the bottom, a
      // chunk that stayed full offered zero room, the copy made no progress, and
      // this loop spun for ever inside the audio callback. Found by a negative
      // control, which hung the test suite instead of failing it.
      if (m_current != kNoChunk && m_pool[m_current].frames == m_pool[m_current].capacity) {
        flush_chunk();
      }
      if (m_current == kNoChunk && !acquire_chunk()) {
        m_dropped.fetch_add(count - done, std::memory_order_relaxed);  // diagnostic only
        return;
      }

      RecordChunk& chunk = m_pool[m_current];
      const std::size_t run = std::min(chunk.capacity - chunk.frames, count - done);
      if (run == 0) {
        // Unreachable with a pool of non-empty chunks, and here anyway: the cost
        // of being wrong about that is a hung audio thread, and the cost of the
        // branch is nothing. A zero-capacity chunk drops its audio and says so,
        // which is this class's answer to every other kind of shortfall too.
        m_dropped.fetch_add(count - done, std::memory_order_relaxed);  // diagnostic only
        return;
      }

      for (std::uint16_t channel = 0; channel < m_channels; ++channel) {
        const float* source = channels[source_channel(channel, channels.size())] + from + done;
        std::copy_n(source, run, chunk.mutable_channel(channel) + chunk.frames);
      }

      chunk.frames += static_cast<std::uint32_t>(run);
      done += run;
      m_frames.fetch_add(run, std::memory_order_relaxed);  // display only
    }
  }

  // AUDIO THREAD. Keeps the tail of what we are not recording, circularly.
  void write_preroll(std::span<const float* const> channels, std::size_t from,
                     std::size_t count) noexcept {
    if (m_preroll_frames == 0 || m_preroll_capacity == 0 || count == 0) {
      return;
    }

    // Only the last capacity frames can survive, so a block longer than the
    // buffer skips its own beginning rather than writing it and overwriting it.
    std::size_t offset = from;
    std::size_t remaining = count;
    if (remaining > m_preroll_capacity) {
      offset += remaining - m_preroll_capacity;
      remaining = m_preroll_capacity;
    }

    while (remaining != 0) {
      const std::size_t run = std::min(remaining, m_preroll_capacity - m_preroll_write);
      for (std::uint16_t channel = 0; channel < m_channels; ++channel) {
        const float* source = channels[source_channel(channel, channels.size())] + offset;
        std::copy_n(source, run, m_preroll.data() + preroll_offset(channel) + m_preroll_write);
      }
      m_preroll_write = (m_preroll_write + run) % m_preroll_capacity;
      m_preroll_filled = std::min(m_preroll_filled + run, m_preroll_capacity);
      offset += run;
      remaining -= run;
    }
  }

  // AUDIO THREAD. Writes the last `m_preroll_frames` of the circular buffer into
  // the take, oldest first.
  void flush_preroll() noexcept {
    const std::size_t want = std::min(m_preroll_frames, m_preroll_filled);
    if (want == 0) {
      return;
    }

    // Channel heads as write_frames wants them. A stack array, sized at compile
    // time: building this any other way would be the allocation this class
    // exists to avoid.
    std::array<const float*, kMaxRecordChannels> heads{};
    for (std::uint16_t channel = 0; channel < m_channels; ++channel) {
      heads[channel] = m_preroll.data() + preroll_offset(channel);
    }
    const std::span<const float* const> source{heads.data(), m_channels};

    const std::size_t start = (m_preroll_write + m_preroll_capacity - want) % m_preroll_capacity;
    const std::size_t first = std::min(want, m_preroll_capacity - start);
    write_frames(source, start, first);
    if (first < want) {
      write_frames(source, 0, want - first);
    }

    m_preroll_filled = 0;
  }

  [[nodiscard]] std::size_t preroll_offset(std::uint16_t channel) const noexcept {
    return static_cast<std::size_t>(channel) * m_preroll_capacity;
  }

  // A source narrower than the take repeats its last channel. See capture().
  [[nodiscard]] static std::size_t source_channel(std::uint16_t channel,
                                                  std::size_t available) noexcept {
    return std::min(static_cast<std::size_t>(channel), available - 1);
  }

  [[nodiscard]] static float measure_peak(std::span<const float* const> channels,
                                          std::size_t num_frames) noexcept {
    float peak = 0.0F;
    for (const float* channel : channels) {
      for (std::size_t frame = 0; frame < num_frames; ++frame) {
        peak = std::max(peak, std::fabs(channel[frame]));
      }
    }
    return peak;
  }

  // The first frame in which ANY channel reaches the threshold, or num_frames.
  [[nodiscard]] std::size_t first_crossing(std::span<const float* const> channels,
                                           std::size_t num_frames) const noexcept {
    for (std::size_t frame = 0; frame < num_frames; ++frame) {
      for (const float* channel : channels) {
        if (std::fabs(channel[frame]) >= m_threshold) {
          return frame;
        }
      }
    }
    return num_frames;
  }

  // Storage the owner allocated. AUDIO THREAD after reset(), except that the
  // control thread reads a chunk's frames between try_collect() and recycle() --
  // a window the rings make exclusive.
  std::span<RecordChunk> m_pool;
  std::span<float> m_preroll;
  std::size_t m_preroll_capacity = 0;
  std::uint16_t m_channels = 0;

  // control -> audio, and audio -> control. The two halves of the loop a chunk
  // travels around; see the diagram at the top.
  SpscRing<std::uint16_t, kRingCapacity> m_empty;
  SpscRing<std::uint16_t, kRingCapacity> m_full;

  // AUDIO THREAD ONLY.
  std::uint16_t m_current = kNoChunk;
  RecordSource m_source = RecordSource::kInput;
  float m_threshold = 0.0F;
  std::size_t m_preroll_frames = 0;
  std::size_t m_preroll_write = 0;
  std::size_t m_preroll_filled = 0;

  // Written by the audio thread, read by the control thread. The state carries
  // the ordering for the whole take -- see stop().
  std::atomic<std::uint8_t> m_state{static_cast<std::uint8_t>(RecordState::kIdle)};
  std::atomic<float> m_peak{0.0F};
  std::atomic<std::uint64_t> m_frames{0};
  std::atomic<std::uint64_t> m_dropped{0};
};

}  // namespace rt

#endif  // CRATEDIG_RT_RECORDER_HPP
