#ifndef CRATEDIG_RT_SEQUENCER_HPP
#define CRATEDIG_RT_SEQUENCER_HPP

#include "rt/pad_event.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace rt {

// The pattern data the audio thread reads, and the arithmetic that places a step
// in time.
//
// WHY THIS IS IN src/rt/ AND NOT src/engine/
// ------------------------------------------
// Because the audio thread reads it. The M4 acceptance is "recorded pattern
// renders bit-exact offline", and offline bounce calls Engine::render() in a
// plain loop with no control thread in existence -- so the sequencer has to run
// inside render(), and everything it touches obeys the src/rt/ rules: fixed
// size, no allocation, no locks, trivially copyable, reached through one
// pointer.
//
// A sequencer that generated events from the control thread would be simpler to
// write and impossible to reproduce offline. That is not a trade-off; it fails
// the milestone.
//
// M6 SERIALISES THIS STRUCT DIRECTLY. It is plain data with no pointers and no
// indirection precisely so the project file has a model to write out rather than
// a second one to invent -- see docs/ARCHITECTURE.md. Adding a pointer, a
// std::string or a std::vector here breaks that and breaks RT-safety in the same
// stroke.

// Sixteenth notes. Four steps to the beat is what a 4x4 grid of pads and a
// 16-step lane both assume, and it is what every hardware box this is imitating
// does by default.
inline constexpr std::uint64_t kStepsPerBeat = 4;

// A pattern is at most this long. 32 steps is two bars of sixteenths, which is
// as much as the pattern lane can show at 100 columns without becoming a
// scrolling view of its own.
inline constexpr std::size_t kMaxSteps = 32;

// How many patterns live in memory at once, and how long a song can be.
inline constexpr std::size_t kMaxPatterns = 16;
inline constexpr std::size_t kMaxSongSlots = 64;

// Tempo bounds, clamped rather than asserted: this arrives from the control
// thread and the audio thread does not get to abort on bad input. Stored times
// 100 so a fractional tempo is exact -- 89.5 bpm is a real thing to want, and a
// float bpm would put a rounding into the one calculation that must not drift.
inline constexpr std::uint32_t kMinBpmX100 = 2'000;       // 20.00 bpm
inline constexpr std::uint32_t kMaxBpmX100 = 30'000;      // 300.00 bpm
inline constexpr std::uint32_t kDefaultBpmX100 = 12'000;  // 120.00 bpm

// Swing pushes the odd steps late, as a percentage of one step. 0 is straight.
//
// Capped below 100 because the arithmetic depends on it: a step pushed a full
// step late would land on top of its neighbour, and step positions would stop
// being monotonic -- which the block scan in Engine::render() relies on.
inline constexpr std::uint8_t kMaxSwingPercent = 75;

// One cell of the grid.
//
// Velocity is 0..127 rather than a float, because that is what MIDI hands us and
// what the pattern lane displays; the conversion to PadEvent's linear 0..1
// happens once, where the step is fired.
struct Step {
  bool on = false;
  std::uint8_t velocity = 100;
};

// One pattern: which pads fire on which steps.
//
// STEP-MAJOR, which is the audio thread's access pattern rather than the UI's.
// Firing a step reads all sixteen pads for one step index, so those sixteen
// bytes are contiguous; the pattern lane draws a row per pad and therefore
// strides, which is the right way round because the lane redraws at 30 Hz and
// the step scan runs every block.
struct Pattern {
  std::array<std::array<Step, kNumPads>, kMaxSteps> steps{};

  // Steps before it wraps, 1..kMaxSteps. Zero is treated as kMaxSteps rather
  // than as a division by zero.
  std::uint8_t length = 16;

  std::uint8_t swing = 0;
};

// The order patterns play in.
//
// A length of zero means "no song": the transport repeats whichever pattern is
// selected, which is what you want while writing one. Chaining is M4 Task 4;
// this carries the data from the start so the struct M6 serialises does not
// change shape halfway through the milestone.
struct Song {
  std::array<std::uint8_t, kMaxSongSlots> order{};
  std::uint8_t length = 0;
};

// Everything the sequencer reads, as one immutable published object.
//
// Swapped whole through rt::HandoffRing, exactly as PadConfig is: editing one
// step means building a new state and publishing it. That is the same
// one-pointer rule, applied for the same reason -- a state the control thread
// mutated field by field could be read mid-edit, and a sequencer that plays half
// of one pattern and half of another is not a bug anyone would diagnose quickly.
//
// It is ~8 KB, so a step toggle memcpys 8 KB on the control thread. That is a
// few microseconds and happens at typing speed; the alternative is a mutable
// shared structure, which is the thing this design exists to avoid.
struct SequencerState {
  std::array<Pattern, kMaxPatterns> patterns{};
  Song song{};

  std::uint32_t bpm_x100 = kDefaultBpmX100;

  // Which pattern plays when the song is empty.
  std::uint8_t selected_pattern = 0;

  // OFF by default, which is the only defensible default: a metronome that
  // starts on would put a click in the first render of every session, including
  // an offline bounce, where it would be baked into the file.
  //
  // Here rather than in Engine::Config because it is changed while running and
  // must reach the audio thread the same way everything else does -- and because
  // M6 serialises this struct, so "was the metronome on" is saved with the
  // project rather than being a setting that resets every launch.
  bool metronome = false;
};

static_assert(std::is_trivially_copyable_v<SequencerState>,
              "SequencerState must be trivially copyable: the audio thread reads it and M6 "
              "serialises it");

// What the transport is doing. Audio-thread-owned; the control thread changes it
// by sending a TransportCommand.
struct Transport {
  bool playing = false;

  // Frames since the transport last started from zero. THE position -- every
  // step boundary is derived from it, and nothing accumulates alongside it.
  std::uint64_t position_frames = 0;
};

enum class TransportCommandKind : std::uint8_t {
  kStop = 0,
  kPlay,
  kSeek,  // moves the position without changing whether it is playing
};

// Control -> audio, through an SpscRing.
//
// A command rather than two atomics. "Play from the top" is a position and a
// state together, and two atomics would let the audio thread see one without the
// other -- the same reasoning that packed the playhead and the pad glow into
// single words.
struct TransportCommand {
  TransportCommandKind kind = TransportCommandKind::kStop;
  std::uint64_t position_frames = 0;
};

static_assert(std::is_trivially_copyable_v<TransportCommand>,
              "TransportCommand travels through SpscRing");

// Frames per step, times 100, so the caller can divide without losing the
// fraction. Exposed for the tests and for the UI's readout.
[[nodiscard]] constexpr std::uint64_t clamp_bpm_x100(std::uint32_t bpm_x100) noexcept {
  return std::clamp(static_cast<std::uint64_t>(bpm_x100), static_cast<std::uint64_t>(kMinBpmX100),
                    static_cast<std::uint64_t>(kMaxBpmX100));
}

// Where step `step` begins, in frames from the transport origin.
//
// COMPUTED FROM THE STEP INDEX, NEVER ACCUMULATED. This is the same rule
// chop_grid() follows and for the same reason: accumulating a frames-per-step
// increment lets rounding drift, so a pattern that is exactly in time at the
// start is a few milliseconds out four minutes later. It is also what makes the
// result independent of the block size, which the offline-bounce acceptance
// rests on -- an accumulator advanced once per block gives different answers at
// 64 frames and at 2048.
//
// Integer arithmetic throughout, so there is no float to round differently on
// another platform.
[[nodiscard]] constexpr std::uint64_t step_frame(std::uint64_t step, std::uint32_t sample_rate,
                                                 std::uint32_t bpm_x100,
                                                 std::uint8_t swing) noexcept {
  const std::uint64_t bpm = clamp_bpm_x100(bpm_x100);
  const std::uint64_t rate = sample_rate;

  // frames per step = rate * 60 / (bpm * steps_per_beat), with bpm scaled by 100.
  const std::uint64_t numerator = rate * 60 * 100;
  const std::uint64_t denominator = bpm * kStepsPerBeat;

  const std::uint64_t base = (step * numerator) / denominator;
  if (swing == 0 || (step % 2) == 0) {
    return base;
  }

  // Odd steps are pushed late by a fraction of THIS step's duration, measured
  // from the next base rather than from a stored frames-per-step -- so the shift
  // inherits the same no-drift property as the base itself.
  const std::uint64_t next = ((step + 1) * numerator) / denominator;
  const std::uint64_t duration = next - base;
  const std::uint64_t shift = (duration * std::min(swing, kMaxSwingPercent)) / 100;
  return base + shift;
}

// Which step `frame` falls in, ignoring swing. The exact inverse of the base
// position — this is "what step is it", and it is what the transport readout
// wants.
[[nodiscard]] constexpr std::uint64_t step_index_at(std::uint64_t frame, std::uint32_t sample_rate,
                                                    std::uint32_t bpm_x100) noexcept {
  const std::uint64_t bpm = clamp_bpm_x100(bpm_x100);
  const std::uint64_t rate = sample_rate;
  if (rate == 0) {
    return 0;
  }
  return (frame * bpm * kStepsPerBeat) / (rate * 60 * 100);
}

// Where to begin walking forward when looking for the steps inside a block.
//
// DELIBERATELY AN UNDER-ESTIMATE, and named for the job rather than for the
// value so it cannot be mistaken for "the current step". Swing moves a step
// later than its base, so a step whose shift carried it into this block has a
// base that sits before it — starting the walk at the exact index would skip
// that step, and a silently missing note is the hardest kind to trace.
//
// It was mistaken for the current step exactly once, by the transport readout,
// which reported every step one early until a test caught it. Hence the name.
[[nodiscard]] constexpr std::uint64_t step_scan_start(std::uint64_t frame,
                                                      std::uint32_t sample_rate,
                                                      std::uint32_t bpm_x100) noexcept {
  const std::uint64_t step = step_index_at(frame, sample_rate, bpm_x100);
  return step == 0 ? 0 : step - 1;
}

// Steps before the pattern wraps. Zero length means the full grid rather than a
// division by zero.
[[nodiscard]] constexpr std::size_t pattern_length(const Pattern& pattern) noexcept {
  const std::size_t length = pattern.length == 0 ? kMaxSteps : pattern.length;
  return std::min(length, kMaxSteps);
}

// How many song slots are actually in use, clamped. `song.length` crossed a
// thread boundary like everything else here.
[[nodiscard]] constexpr std::size_t song_slots(const Song& song) noexcept {
  return std::min(static_cast<std::size_t>(song.length), kMaxSongSlots);
}

// Which pattern a song slot names, clamped into range rather than trusted -- an
// out-of-range index would be a read past the end of the pattern array on the
// audio thread.
[[nodiscard]] constexpr std::uint8_t song_pattern(const Song& song, std::size_t slot) noexcept {
  if (slot >= kMaxSongSlots) {
    return 0;
  }
  return static_cast<std::uint8_t>(
      std::min(static_cast<std::size_t>(song.order[slot]), kMaxPatterns - 1));
}

// Where an absolute step index lands: which song slot, which pattern, and which
// step inside it.
struct SongPosition {
  std::uint8_t pattern = 0;
  std::size_t step = 0;  // within `pattern`
  std::size_t slot = 0;  // 0 when there is no song
};

// Total steps in one pass through the song. Zero when the song is empty.
//
// Summed rather than assumed uniform, because patterns may differ in length --
// chaining a 16-step verse to a 12-step fill is an ordinary thing to want, and a
// song model that assumed a fixed length would silently truncate the longer one.
[[nodiscard]] constexpr std::size_t song_length_steps(const SequencerState& state) noexcept {
  const std::size_t slots = song_slots(state.song);
  std::size_t total = 0;
  for (std::size_t slot = 0; slot < slots; ++slot) {
    total += pattern_length(state.patterns[song_pattern(state.song, slot)]);
  }
  return total;
}

// Resolve an absolute step index against the song.
//
// AN EMPTY SONG IS NOT AN ERROR, it is the normal state while writing a pattern:
// the selected pattern repeats forever. Chaining only takes over once there is a
// song to chain, which is what lets `:song` be an addition rather than a mode
// the interface has to be put into.
//
// The walk over slots is O(song length) and bounded at kMaxSongSlots, with no
// allocation -- fine on the audio thread, where it runs at most a couple of
// times per block rather than per frame.
[[nodiscard]] constexpr SongPosition song_position_at(const SequencerState& state,
                                                      std::uint64_t absolute_step) noexcept {
  const std::size_t slots = song_slots(state.song);
  if (slots == 0) {
    const std::uint8_t pattern = static_cast<std::uint8_t>(
        std::min(static_cast<std::size_t>(state.selected_pattern), kMaxPatterns - 1));
    const std::size_t length = pattern_length(state.patterns[pattern]);
    return SongPosition{
        .pattern = pattern, .step = static_cast<std::size_t>(absolute_step % length), .slot = 0};
  }

  const std::size_t total = song_length_steps(state);
  if (total == 0) {
    return SongPosition{};  // unreachable: pattern_length() is never zero
  }

  auto into = static_cast<std::size_t>(absolute_step % total);
  for (std::size_t slot = 0; slot < slots; ++slot) {
    const std::uint8_t pattern = song_pattern(state.song, slot);
    const std::size_t length = pattern_length(state.patterns[pattern]);
    if (into < length) {
      return SongPosition{.pattern = pattern, .step = into, .slot = slot};
    }
    into -= length;
  }

  // Unreachable while `total` is the sum of the same lengths, but falling out of
  // the loop must still produce a valid position rather than whatever was last
  // in the locals.
  return SongPosition{.pattern = song_pattern(state.song, 0), .step = 0, .slot = 0};
}

}  // namespace rt

#endif  // CRATEDIG_RT_SEQUENCER_HPP
