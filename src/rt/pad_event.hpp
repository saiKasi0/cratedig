#ifndef CRATEDIG_RT_PAD_EVENT_HPP
#define CRATEDIG_RT_PAD_EVENT_HPP

#include <cstdint>
#include <type_traits>

namespace rt {

// How many pads the engine addresses. Sixteen because that is the 4x4 grid in
// docs/design and the shape of every hardware sampler this thing is imitating.
inline constexpr std::uint8_t kNumPads = 16;

// What a PadEvent is telling the audio thread to do.
enum class PadEventKind : std::uint8_t {
  // Start a voice.
  kNoteOn = 0,

  // Let go of the pad. Only a pad in TriggerMode::kGate acts on it; a one-shot
  // ignores it, which is the definition of one-shot rather than a special case
  // anywhere else.
  //
  // The engine understands this from M3 even though M3 has only one producer of
  // it (the Kitty keyboard protocol, where the terminal supports key release).
  // M4's MIDI note-off and M5's sequencer become additional producers of an
  // event the engine already handles, rather than each bringing their own idea
  // of what letting go means.
  kNoteOff,

  // Stop this pad NOW, whatever its trigger mode.
  //
  // Not a note-off with a flag: a note-off means "the player let go", which a
  // one-shot is entitled to ignore, and this means "stop making that sound",
  // which nothing is. Conflating them would make `:stop 3` silently do nothing
  // on exactly the pads it is most wanted for -- one-shots long enough that
  // waiting for them is the problem.
  //
  // Released through the pad's declick floor rather than cut, so it is silence
  // rather than a click (PadConfig::release_floor_frames).
  kStop,

  // The same for every sounding voice. `pad` is ignored.
  //
  // The panic key. A separate kind rather than kStop with a sentinel pad,
  // because "all" is not a pad number and encoding it as one is how a stray
  // 255 turns into a silent room.
  kStopAll,

  // Stop whatever is being AUDITIONED. `pad` is ignored.
  //
  // An audition is not a pad -- it is played on its own voices, out of its own
  // pool, so that it cannot light a pad, appear in a strip meter or choke
  // anything (docs/ARCHITECTURE.md). It therefore needs its own way to be
  // stopped, and it rides this ring rather than growing a second one: the ring
  // already exists, already has a producer on each of the two threads that would
  // want it, and carries "stop making that sound" messages already.
  kStopAudition,
};

// A pad hit, sent control -> audio through an SpscRing.
//
// Trivially copyable by construction, because SpscRing overwrites slots by
// assignment and never runs a destructor. That constraint is what keeps the
// message path allocation-free and lock-free, so it is enforced below rather
// than left to reviewer attention.
struct PadEvent {
  // Which pad. Out-of-range values are dropped by the engine rather than
  // trusted: this arrives from a key handler, and later from MIDI.
  std::uint8_t pad = 0;

  PadEventKind kind = PadEventKind::kNoteOn;

  // Linear gain in [0, 1]. Normalised here rather than carrying MIDI's 7-bit
  // velocity, so that the src/io/ MIDI layer owns the curve choice in M4 and
  // the engine never has to know where a hit came from.
  float velocity = 1.0F;

  // Offset in frames from the start of the block this event is drained in.
  //
  // M1 always triggers at the block start and therefore always sets 0. The field
  // exists now because M4 makes triggers sample-accurate, and adding it later
  // would mean changing a wire format that MIDI, the sequencer and the offline
  // renderer all already speak.
  std::uint32_t frame_offset = 0;
};

static_assert(std::is_trivially_copyable_v<PadEvent>,
              "PadEvent must be trivially copyable to travel through SpscRing");

// A pad that was PLAYED, reported audio -> control. The other direction.
//
// Everything else in this header travels from a keyboard or a MIDI port toward
// the audio thread. This goes back, and it exists because live recording needs
// something the control thread cannot work out for itself: WHEN, exactly.
//
// The audio thread places every live trigger at a known frame inside its block
// (PadEvent::frame_offset) and is the only thread that knows where the transport
// was at that moment. By the time the control thread's 30 Hz tick notices a key
// was pressed, the transport has moved on by up to a thirtieth of a second --
// which at 120 bpm is a third of a sixteenth-note step, so a take timed on the
// control thread would quantise to the wrong step roughly whenever the player
// was slightly ahead of the beat. Which is to say: whenever they were playing.
//
// WHAT IS NOT HERE, deliberately: the quantised step. Rounding a frame to a step
// is policy -- the resolution is a setting, and coarser grids are useful -- and
// policy on the audio thread is a setting that cannot be changed without a
// message. The control thread has the frame, the tempo and the sample rate, so
// it can do the arithmetic itself and change its mind for free.
struct PadHit {
  // Absolute transport position, in frames, at which the pad sounded. Absolute
  // rather than block-relative because the block is gone by the time anyone
  // reads this.
  std::uint64_t frame = 0;

  std::uint8_t pad = 0;

  // 0..127, matching rt::Step rather than PadEvent's linear float above.
  //
  // Converted HERE, at the one place a played note becomes a recorded one, so
  // that a hit and the step it is written into cannot disagree about how loud it
  // was.
  std::uint8_t velocity = 0;
};

static_assert(std::is_trivially_copyable_v<PadHit>,
              "PadHit must be trivially copyable to travel through SpscRing");

}  // namespace rt

#endif  // CRATEDIG_RT_PAD_EVENT_HPP
