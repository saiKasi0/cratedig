#ifndef CRATEDIG_ENGINE_ENGINE_HPP
#define CRATEDIG_ENGINE_ENGINE_HPP

#include "rt/arch.hpp"
#include "rt/biquad.hpp"
#include "rt/click.hpp"
#include "rt/compressor.hpp"
#include "rt/garbage_ring.hpp"
#include "rt/handoff_ring.hpp"
#include "rt/limiter.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/recorder.hpp"
#include "rt/sample.hpp"
#include "rt/sequencer.hpp"
#include "rt/spsc_ring.hpp"
#include "rt/strip.hpp"
#include "rt/voice_pool.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace engine {

// How long ago a pad was last hit, and how hard.
//
// The signal the pad grid lights from. NOT the peak level, which is already
// published and already decays and is nevertheless the wrong answer: peak
// follows the audio, so a quiet sample barely lights its pad and a pad triggered
// into near-silence does not light at all. What a player wants to see is that
// the machine received the hit -- an acknowledgement, not a meter.
struct PadGlow {
  // Since the most recent trigger, in LISTENER TIME -- how long ago the hit
  // reached the ear rather than how long ago it was rendered.
  //
  // NEGATIVE means the sound has been rendered but has not arrived yet, which
  // only happens for sequenced hits on a device with real output latency. The
  // interface reads that as "not lit", and it is the honest value rather than a
  // sentinel: the hit did happen, it is simply still in flight.
  //
  // Grows without bound; the interface decides how fast to fade, because that is
  // a look rather than an engine fact.
  float seconds_since_trigger = 0.0F;

  // The velocity of that trigger, in [0, 1].
  float velocity = 0.0F;

  // False until the pad has been hit at least once. Distinguishes "hit a long
  // time ago" from "never hit", which otherwise look identical.
  bool triggered = false;

  // Whether the sequencer played it rather than a person.
  //
  // Hardware samplers distinguish these and the distinction is genuinely useful
  // when overdubbing: it is how you tell what you just added from what was
  // already there.
  bool sequenced = false;
};

// What the interface needs to know about what the audio thread is doing, as one
// consistent snapshot.
//
// None of this feeds back into the audio path, so it is deliberately NOT part of
// the determinism contract: the peak fall rate depends on the block size, which
// varies with the device, while render()'s output does not.
struct Telemetry {
  // Position within the sample of the most recently started voice still
  // sounding, and which pad it came from. `playing` is false when nothing is.
  std::uint64_t playhead_frame = 0;
  std::uint8_t playhead_pad = 0;
  bool playing = false;

  // Loudest absolute output sample, with a fall time so a meter read at 30 Hz
  // does not show whichever 5 ms block it happened to land on.
  float master_peak = 0.0F;

  // Per-pad level, same fall behaviour. Indexed by pad.
  //
  // WHAT THE PAD PLAYED, measured on the voices before the mixer touches them.
  // Not the same question as strip_peak below, and both are wanted: this one
  // drives the pad grid, where a muted pad should still acknowledge the hit.
  std::array<float, rt::kNumPads> pad_peak{};

  // WHAT THE STRIP CONTRIBUTED, measured after the fader and the balance, and
  // zero when the strip is muted or soloed out. This is the meter beside the
  // fader on the MIX screen: it answers "is this reaching the mix", which for a
  // muted strip is no, however hard the pad was hit.
  std::array<float, rt::kNumPads> strip_peak{};

  // Per-bus level, after every strip routed to it has summed in.
  std::array<float, rt::kNumBuses> bus_peak{};

  // The master limiter's gain, LINEAR. 1.0 is none. Reported whether or not the
  // limiter is engaged -- a disengaged limiter reads 1.0, which is the truth.
  float limiter_gain = 1.0F;

  // Worst compressor gain reduction on each strip, LINEAR, with the makeup gain
  // divided back out. 1.0 is none. Linear rather than dB for the reason
  // docs/MIXER.md gives for every level here: dB is a display unit, converted at
  // the boundary.
  std::array<float, rt::kNumPads> strip_reduction{};

  // Per-pad trigger acknowledgement. Indexed by pad. See PadGlow above for why
  // this is not the same thing as pad_peak.
  std::array<PadGlow, rt::kNumPads> pad_glow{};

  // Where the transport is, as the audio thread last left it.
  //
  // `step` is the position expressed in steps of the pattern that was playing,
  // already wrapped to its length -- computed on the audio thread, which is the
  // only place that knows both the position and the tempo it was rendered at.
  // The UI recomputing it from `transport_frames` would be reading the tempo it
  // has now, not the one those frames were rendered with.
  bool transport_playing = false;
  std::uint64_t transport_frames = 0;
  std::uint32_t transport_step = 0;
  std::uint8_t transport_pattern = 0;

  // Which song slot is playing. Always 0 when there is no song, which is the
  // same thing the interface should show: an empty song is not slot zero of
  // nothing, it is a pattern repeating.
  std::uint8_t transport_slot = 0;

  // What the recorder is doing and what it is hearing.
  //
  // `record_peak` is the level of whatever the recorder is TAPPING, which is not
  // necessarily the input: with the source set to master it meters the mix.
  // That is the useful answer either way, because the question a meter beside a
  // record button answers is "how loud is the thing I am about to keep".
  rt::RecordState record_state = rt::RecordState::kIdle;
  float record_peak = 0.0F;
  std::uint64_t recorded_frames = 0;

  // Non-zero means the take has holes in it. See rt::Recorder::dropped_frames --
  // this is not a quality metric, it is a correctness one.
  std::uint64_t record_dropped_frames = 0;
};

// The engine facade. Everything audible eventually happens behind render().
//
// This header must never see a device API. RtAudio and RtMidi live in src/io/
// and are the only place allowed to include their headers (CLAUDE.md). Keeping
// the engine device-free is what makes offline bounce, e2e tests, and Docker CI
// possible: render() works in a plain loop with no audio hardware present.
//
// Threading, in one place:
//   - CONTROL calls publish_pad_config() and trigger_pad(), at any time, running
//     or not. Nothing else.
//   - AUDIO calls render(). It allocates nothing, locks nothing, and never runs
//     a Sample or PadConfig destructor.
//   - JANITOR calls collect_garbage(). The engine spawns no threads of its own;
//     whoever owns the run loop decides when collection happens, which is what
//     keeps offline rendering single-threaded and reproducible.
class Engine {
 public:
  // Sixteen voices is the M1 budget, matching the pad count so a full grid can
  // sound at once. The perf target (64 voices at 64 frames) is M8's problem.
  static constexpr std::size_t kMaxVoices = 16;

  // Deep enough to swallow a burst of key repeats or a dense MIDI chord between
  // two audio blocks without dropping anything.
  static constexpr std::size_t kEventRingCapacity = 256;

  // Sized well above kMaxVoices: every voice could finish in the same block, and
  // the janitor may be a whole UI frame behind.
  static constexpr std::size_t kGarbageRingCapacity = 64;

  // Voices reserved for AUDITION -- previewing material that is on no pad.
  //
  // ITS OWN POOL, and that is the design rather than a convenience. An audition
  // voice must not light a pad, must not appear in a strip meter, must not choke
  // anything and must not be stolen by a pad hit. Every one of those exclusions
  // is free if the voice simply is not in the pad pool, and every one would be a
  // flag somebody has to remember to check if it were. rt::VoicePool::render_pad
  // even asserts that every voice it walks belongs to a strip, so a padless voice
  // in that pool would be silently never rendered.
  //
  // Two, so auditioning a second thing while the first is still ringing does not
  // cut it off mid-note. Not sixteen: this is a preview, not an instrument.
  static constexpr std::size_t kAuditionVoices = 2;

  // Audition configs in flight, control -> audio. Small: a person clicking
  // through a directory cannot outrun a 5 ms block by more than a couple.
  static constexpr std::size_t kAuditionHandoffCapacity = 8;

  // Pad reconfigurations in flight, control -> audio. `:chop transient`
  // publishes one config per pad in a single burst, so a full grid of sixteen
  // is the unit to size for -- and twice that, because the burst can land on
  // top of pads assigned earlier in the same block. A burst allowance, not a
  // throughput budget: render() drains the ring completely every block, and a
  // human editing pads cannot outrun 5 ms.
  //
  // With --no-audio nothing renders, so nothing ever drains and enough chops in
  // one session will fill it. That is not a leak to size around; it is what
  // that mode is. publish_pad_config() reports the refusal, the control-side
  // view stays truthful, and the interface says so on the mode line.
  static constexpr std::size_t kPadHandoffCapacity = 32;

  // Sequencer states in flight, control -> audio. Far smaller than the pad ring
  // because there is exactly one of these objects rather than sixteen: a step
  // toggle publishes one state, and a held key repeating at the terminal's rate
  // cannot outrun a 5 ms block by more than a couple.
  static constexpr std::size_t kSequencerHandoffCapacity = 8;

  // MIDI events in flight, MIDI thread -> audio. ITS OWN RING, not a second
  // producer on the keyboard's.
  //
  // rt::SpscRing is single-producer -- the header says so and TSan is the
  // authority on it -- and RtMidi delivers on a thread of its own. A MIDI
  // callback calling trigger_pad() would put two producers on one ring, which
  // would appear to work and would be a data race. One extra ring is cheap; the
  // alternative is a bug that only shows up under load, on someone else's
  // machine.
  //
  // Deep enough for a dense chord plus pedal traffic between two audio blocks.
  static constexpr std::size_t kMidiRingCapacity = 256;

  // Master section settings in flight, control -> audio.
  //
  // A VALUE RING, not the shared_ptr handoff the pads use, and the difference is
  // the reason the one-pointer rule exists at all: a PadConfig owns a Sample and
  // cannot be copied atomically, so it must be swapped by pointer. A
  // LimiterConfig owns nothing -- it is a handful of floats -- so it travels the
  // same way a PadEvent does, by value, with no garbage to retire afterwards.
  static constexpr std::size_t kMasterRingCapacity = 8;

  // Transport commands in flight. Play, stop and seek arrive at human speed; the
  // depth is here so a burst during a stalled stream is dropped visibly rather
  // than blocking the UI.
  static constexpr std::size_t kTransportRingCapacity = 32;

  // Recorder instructions in flight, control -> audio. Arm, punch in, stop --
  // three things a person does with their hands, so the depth is a burst
  // allowance rather than a throughput budget.
  static constexpr std::size_t kRecordRingCapacity = 8;

  // Live pad hits in flight, AUDIO -> CONTROL, waiting to be written into a
  // pattern.
  //
  // Sized for the gap between control-thread ticks rather than for a
  // performance: the UI drains this at about 30 Hz, and 128 hits in a
  // thirty-third of a second is roughly four thousand notes a second. Ten
  // fingers cannot do that, and neither can a MIDI pad controller; a full ring
  // here means the control thread stalled, which is why the overflow is counted
  // rather than sized around.
  static constexpr std::size_t kHitRingCapacity = 128;

  // The capture pool: 32 chunks of 4096 frames.
  //
  // 2.7 SECONDS OF SLACK at 48 kHz, which is what the number has to be measured
  // against -- not the length of a take, which this bounds not at all. It is how
  // far behind the control thread may fall before frames start being dropped,
  // and it is sized against the UI's frame tick (about 30 Hz) with three orders
  // of magnitude to spare, because the tick is where collect_take() runs and the
  // tick has been seen to stall for 80 ms on an onset analysis.
  //
  // 1 MB at stereo. The chunk length itself is not critical: a block is split
  // across chunks as needed, so this trades the number of ring operations
  // against how long the last partial chunk of a take sits unsent, which is one
  // block either way because stop() flushes it.
  static constexpr std::size_t kRecordChunkFrames = 4'096;
  static constexpr std::size_t kRecordPoolChunks = 32;

  // How much audio the recorder keeps behind a threshold trigger, at most.
  //
  // Half a second because that is comfortably longer than any attack a person
  // would want back, and 192 KB at stereo/48 kHz is not worth being clever
  // about. What is actually kept is whatever the caller asks arm() for, up to
  // this; the buffer only has to be big enough not to be the limit.
  static constexpr std::uint32_t kRecordPreRollMilliseconds = 500;

  struct Config {
    std::uint32_t sample_rate = 48'000;
    std::uint16_t num_channels = 2;
    std::uint32_t max_block_frames = 2'048;

    // Anchors the determinism contract (docs/TESTING.md). Nothing consumes it
    // until the first stochastic element lands — humanize, noise, dither in M6 —
    // but it is part of the signature from the start so that "same seed, same
    // input, same bytes" is a promise the API always made, not one retrofitted
    // after something started varying.
    std::uint64_t seed = 0;

    // How far behind the render the listener actually is, in frames.
    //
    // ZERO UNTIL M9 MEASURES IT, and exercised at zero on purpose so the path is
    // built and tested rather than bolted on later. It delays SEQUENCED glow
    // only: with 180 ms of output latency a pad that lights when its block
    // renders lights 180 ms early, and on a Bluetooth sink that is plainly
    // visible as the lights running ahead of the music.
    //
    // Live triggers get no such treatment, and cannot. Their reference is the
    // player's own finger, and nothing can be done about that gap -- delaying
    // the light would only add to it.
    //
    // NOT part of the determinism contract: it moves a light, never a sample.
    std::uint32_t output_latency_frames = 0;
  };

  explicit Engine(const Config& config) noexcept;

  // CONTROL THREAD, AT ANY TIME — running or not.
  //
  // Hands the audio thread a complete replacement for one pad. The config is
  // built here, where allocation and file I/O are legal; the audio thread swaps
  // a pointer and retires what it displaced. Nothing is constructed or destroyed
  // on the audio thread, which is what makes this safe mid-stream.
  //
  // This is the protocol M6's recording and M8's plugin chains reuse unchanged
  // (docs/ARCHITECTURE.md, "Live reconfiguration: one problem, one protocol").
  //
  // Returns false if the handoff ring is full, in which case THE CALLER STILL
  // OWNS `config` and the pad is unchanged — the edit did not happen, and
  // saying so is better than dropping it silently. `config` must be non-null and
  // name a pad below kNumPads; anything else is refused here rather than on the
  // audio thread, so a bad index costs a control-thread branch and not a block.
  [[nodiscard]] bool publish_pad_config(std::shared_ptr<const rt::PadConfig> config) noexcept;

  // CONTROL THREAD. Convenience for the common case: play this whole sample on
  // this pad, everything else default.
  //
  // Before M3 this wrote the pad table directly and was documented pre-start
  // only. It now goes through publish_pad_config() like everything else, so the
  // caveat is gone.
  [[nodiscard]] bool set_pad_sample(std::uint8_t pad,
                                    std::shared_ptr<const rt::Sample> sample) noexcept;

  // CONTROL THREAD. What this thread has most recently published for a pad —
  // NOT what the audio thread is currently using, which may still be one block
  // behind.
  //
  // That distinction is the honest one for a UI: the interface should show the
  // edit the moment it is made, and the block boundary is not a fact about the
  // pad. Reading the audio thread's table from here would be a data race, which
  // is exactly the problem this whole task exists to fix.
  [[nodiscard]] std::shared_ptr<const rt::PadConfig> pad_config(std::uint8_t pad) const noexcept;

  [[nodiscard]] std::shared_ptr<const rt::Sample> pad_sample(std::uint8_t pad) const noexcept;

  // CONTROL THREAD, AT ANY TIME. Plays something that is on no pad.
  //
  // THE ARRIVAL OF THE CONFIG IS THE TRIGGER. One message rather than a publish
  // and then a play: an audition is inherently "let me hear this now", and a
  // two-message version would have a window in which the config is loaded and
  // silent, which is a state nobody wants and everybody would have to handle.
  //
  // The config is an ordinary rt::PadConfig -- same slice range, same envelope,
  // same fades -- because previewing a slice is playing a slice. Its `pad` field
  // is ignored, and it is played on the audition pool, so nothing it does
  // reaches the pad grid or the mixer's strips.
  //
  // Returns false if the handoff ring is full, in which case THE CALLER STILL
  // OWNS `config` and nothing was auditioned.
  [[nodiscard]] bool audition(std::shared_ptr<const rt::PadConfig> config) noexcept;

  // CONTROL THREAD. Silences whatever is being auditioned, through the pad event
  // ring. Released rather than cut, like every other stop.
  [[nodiscard]] bool stop_audition() noexcept;

  // How many audition voices are sounding. For tests and for the interface;
  // deliberately separate from active_voices(), which counts pads.
  [[nodiscard]] std::size_t active_auditions() const noexcept;

  // Where the preview has got to, in frames from the start of its sample.
  // kNoAuditionPlayhead when nothing is being previewed.
  //
  // Frames from the START OF THE SAMPLE, not from the start of the region a
  // preview may be playing: BROWSE draws the whole file and puts the marker on
  // it, so a position relative to a region would have to be un-relativised by
  // every caller that has one.
  [[nodiscard]] std::uint64_t audition_playhead() const noexcept;

  static constexpr std::uint64_t kNoAuditionPlayhead = std::numeric_limits<std::uint64_t>::max();

  // CONTROL THREAD. Replaces one pad's mixer settings, keeping everything else
  // about the pad.
  //
  // A fader move goes through the same handoff protocol as loading a sample,
  // because a StripConfig lives inside the PadConfig and a published PadConfig
  // is immutable (rt/pad_config.hpp). That is not ceremony: it is what makes
  // moving a fader while sixteen voices are sounding a pointer swap rather than
  // a race.
  //
  // Returns false if the handoff ring is full, exactly like publish_pad_config,
  // in which case the strip is unchanged.
  [[nodiscard]] bool set_strip(std::uint8_t pad, const rt::StripConfig& strip) noexcept;

  // CONTROL THREAD. What this thread last published for that pad's strip, with
  // the same one-block-ahead caveat as pad_config().
  [[nodiscard]] rt::StripConfig strip(std::uint8_t pad) const noexcept;

  // CONTROL THREAD. Replaces the master limiter's settings.
  //
  // Returns false if the ring is full, in which case nothing changed.
  [[nodiscard]] bool set_limiter(const rt::LimiterConfig& limiter) noexcept;

  // CONTROL THREAD. Sets one bus's output gain, linear.
  //
  // Travels in the SAME message as the limiter, because they are one thing --
  // the master section -- and two rings would let the audio thread render a
  // block with a new bus gain and an old limiter, a state nobody asked for.
  [[nodiscard]] bool set_bus_gain(std::uint8_t bus, float gain) noexcept;

  // CONTROL THREAD. What this thread last published.
  [[nodiscard]] rt::LimiterConfig limiter() const noexcept;
  [[nodiscard]] float bus_gain(std::uint8_t bus) const noexcept;

  // CONTROL THREAD. Queues a pad hit for the next block.
  //
  // Returns false if the ring is full, in which case the event is dropped and
  // dropped_events() counts it. Blocking here would push the caller's latency
  // into the UI; silently succeeding would hide a real overload.
  [[nodiscard]] bool trigger_pad(const rt::PadEvent& event) noexcept;

  // MIDI THREAD ONLY. The same event, arriving from a different thread.
  //
  // Separate from trigger_pad() because it goes into a separate ring -- see
  // kMidiRingCapacity. Calling this from the control thread, or trigger_pad()
  // from the MIDI thread, reintroduces exactly the race the split exists to
  // prevent.
  //
  // Returns false if the ring is full, in which case the event is dropped and
  // dropped_midi_events() counts it.
  [[nodiscard]] bool submit_midi_event(const rt::PadEvent& event) noexcept;

  // CONTROL THREAD, AT ANY TIME. Replaces the whole sequencer state.
  //
  // The same protocol as publish_pad_config() and for the same reason: the state
  // is immutable once the audio thread can see it, so editing one step means
  // building a new state. Returns false if the ring is full, in which case THE
  // CALLER STILL OWNS `state` and the sequencer is unchanged.
  [[nodiscard]] bool publish_sequencer(std::shared_ptr<const rt::SequencerState> state) noexcept;

  // CONTROL THREAD. What this thread has most recently published -- not
  // necessarily what the audio thread is using yet. Same one-block-ahead
  // distinction as pad_config(), and honest for the same reason.
  [[nodiscard]] std::shared_ptr<const rt::SequencerState> sequencer_state() const noexcept;

  // CONTROL THREAD. Start, stop or seek the transport.
  //
  // Returns false if the ring is full, in which case the command did not happen.
  [[nodiscard]] bool send_transport(const rt::TransportCommand& command) noexcept;

  // REAL-TIME. Called from the audio callback; opens its own RT_SCOPE.
  //
  // channels is a span of per-channel pointers (the CLAUDE.md `float** out`
  // reconciled with the std::span rule) — channels.size() must equal
  // config.num_channels, and each pointer must address at least num_frames
  // floats. num_frames must not exceed config.max_block_frames. All three are
  // debug-asserted; in release they are the caller's contract.
  void render(std::span<float* const> channels, std::size_t num_frames) noexcept;

  // REAL-TIME. The duplex form: the same block, plus the stream's input.
  //
  // `input` is planar like everything else, and may be EMPTY -- which is the
  // normal state for an offline bounce, for a test, and for an output-only
  // device. The recorder is the only thing that reads it, so an empty span costs
  // nothing and changes no sample of the output.
  //
  // A SEPARATE OVERLOAD rather than a defaulted parameter, so that the
  // output-only form above stays the one every existing caller compiles against
  // unchanged -- and so that "this render has an input" is visible at the call
  // site, which is exactly one place: io::AudioDevice's callback.
  void render(std::span<float* const> channels, std::span<const float* const> input,
              std::size_t num_frames) noexcept;

  // CONTROL THREAD. Arm the recorder: listen, meter, keep nothing yet.
  //
  // With `threshold` above zero the take starts by itself when the source
  // reaches it, keeping `preroll_frames` of what came before (rt/recorder.hpp
  // explains why that is not a luxury). With `threshold` at zero it waits for
  // start_recording().
  //
  // Returns false if the ring is full, OR if a take is still outstanding --
  // recording over one that has not been collected would splice two takes into
  // one, so it is refused here rather than discovered afterwards. Call
  // discard_take() or take the audio first.
  [[nodiscard]] bool arm_recording(rt::RecordSource source, float threshold,
                                   std::size_t preroll_frames) noexcept;

  // CONTROL THREAD. Punch in now. Same refusals as arm_recording().
  [[nodiscard]] bool start_recording(rt::RecordSource source) noexcept;

  // CONTROL THREAD. Stop keeping. The partial chunk in the audio thread's hand
  // comes back with it, so the tail of the take is not lost.
  [[nodiscard]] bool stop_recording() noexcept;

  // CONTROL THREAD, on the run loop's tick, like collect_garbage().
  //
  // Appends whatever the audio thread has filled to the take being assembled and
  // hands the chunks back. Returns the frames appended.
  //
  // MUST BE CALLED WHILE RECORDING, not only at the end: the pool is finite, and
  // a caller that waits until the take is over gets kRecordPoolChunks worth of
  // audio and a drop count for the rest.
  std::size_t collect_take() noexcept;

  // CONTROL THREAD. Whether the take is over AND fully collected.
  //
  // Both halves. The state alone would be trusting the audio thread to have
  // pushed its last chunk before publishing kIdle -- see rt::Recorder, which
  // documents why counting is the half that cannot be broken by an edit.
  [[nodiscard]] bool take_complete() const noexcept;

  [[nodiscard]] std::size_t take_frames() const noexcept { return m_take_frames; }

  // CONTROL THREAD. One channel of the assembled take.
  //
  // A SPAN rather than a built rt::Sample, so that the engine does not decide
  // what a take becomes. The caller may want a pad, a pool entry with a peak
  // pyramid, or a file; all three start from these frames, and only the caller
  // knows which. Valid until the next collect_take() or discard_take().
  [[nodiscard]] std::span<const float> take_channel(std::uint16_t channel) const noexcept;

  [[nodiscard]] std::uint16_t take_channels() const noexcept { return m_record_channels; }

  // CONTROL THREAD. The collected take as a Sample, or null when there is none.
  //
  // A CONVENIENCE OVER take_channel(), not a replacement for it: the spans stay
  // because a caller may want to write a file or compute something without a
  // second copy of the audio. This exists because the copy loop is otherwise
  // written out in the interface, where no test can reach it -- and a loop that
  // fills a Sample one channel at a time is exactly the kind of thing that is
  // right until somebody edits it.
  //
  // Allocates, on the control thread, where that is legal. Does NOT clear the
  // take; call discard_take() when you have what you want.
  [[nodiscard]] std::shared_ptr<rt::Sample> build_take() const;

  // CONTROL THREAD. Throws the assembled take away and drains anything still in
  // flight, so the recorder can be armed again.
  void discard_take() noexcept;

  [[nodiscard]] rt::RecordState record_state() const noexcept { return m_recorder.state(); }

  // CONTROL THREAD. The next live pad hit the audio thread reported, oldest
  // first; false when there are none left.
  //
  // "Live" means a person played it -- keyboard or MIDI -- while the transport
  // was running. The sequencer's own hits are NOT reported, because writing them
  // back into the pattern they came from is how an overdub fills a pattern with
  // copies of itself.
  //
  // Drain this on the run loop's tick whether or not anything is recording. A
  // producer with no consumer fills, and a ring that fills silently is how the
  // audition path broke twice in M4.5.
  [[nodiscard]] bool next_hit(rt::PadHit& out) noexcept;

  // Hits lost because the ring was full. Non-zero means the control thread
  // stopped draining, not that somebody played too fast -- see kHitRingCapacity.
  [[nodiscard]] std::uint64_t dropped_hits() const noexcept {
    return m_dropped_hits.load(std::memory_order_relaxed);
  }

  // JANITOR THREAD. Destroys everything the audio thread has retired, and
  // returns how many references were released.
  std::size_t collect_garbage() noexcept;

  // CONTROL THREAD, and ONLY when no audio thread exists.
  //
  // Adopts whatever has been published, as the top of render() would. With no
  // device open nothing calls render(), so nothing ever drains the handoff
  // rings -- they fill, and every later edit is refused for the rest of the
  // session. That is not a theoretical leak: `:chop` publishes sixteen configs
  // at once and `:slot assign 1-8 1` publishes eight, so four commands exhaust
  // thirty-two slots and pad assignment stops working entirely.
  //
  // Safe precisely BECAUSE there is no second thread in that mode, which is why
  // this names the condition instead of being called "flush". Calling it while a
  // stream is running is a data race with the audio thread, and the same
  // discipline collect_garbage() already relies on is what keeps it honest.
  //
  // Deliberately NOT the transport or the event rings. Those carry state and
  // moments that only mean something while frames are being produced -- adopting
  // a play command with nothing to advance would light `play` on the mode line
  // for a transport that cannot move. Reconfigurations are edits, and an edit
  // must land whether or not anything is listening.
  void adopt_offline() noexcept;

  // Atomic because the audio thread writes it and the UI reads it live. Relaxed
  // on both sides: it orders nothing, it is a progress counter. Making it a
  // plain uint64_t was a real data race, and it did not present as a torn value
  // -- the compiler simply hoisted the load out of a poll loop, so a caller
  // watching for progress saw zero forever while the callback ran normally.
  [[nodiscard]] std::uint64_t frames_rendered() const noexcept {
    return m_published.frames_rendered.load(std::memory_order_relaxed);
  }

  // ANY THREAD. What the audio thread published at the end of its last block.
  //
  // Every field is a relaxed atomic: the UI wants a recent value, not a
  // synchronised one, and acquiring here would put a barrier in the audio
  // thread's hot path to make a meter one frame fresher.
  [[nodiscard]] Telemetry telemetry() const noexcept;

  // Full scale to silence in this long, when nothing louder arrives. Fast enough
  // to follow a drum pattern, slow enough that a 30 Hz UI never samples the gap
  // between two hits and reports silence.
  static constexpr float kPeakFallSeconds = 0.4F;

  [[nodiscard]] const Config& config() const noexcept { return m_config; }

  [[nodiscard]] std::size_t active_voices() const noexcept { return m_voices.active_count(); }

  // Diagnostics. Any of these being non-zero after a normal session is a bug
  // somewhere, so they are exposed rather than merely counted.
  [[nodiscard]] std::uint64_t dropped_events() const noexcept { return m_dropped_events; }

  // MIDI events dropped because the ring was full. Non-zero means the audio
  // thread stopped draining, not that a player was too fast: 256 events between
  // two blocks is not something hands can do.
  [[nodiscard]] std::uint64_t dropped_midi_events() const noexcept {
    return m_dropped_midi_events.load(std::memory_order_relaxed);
  }

  // Also audio-thread-written and UI-read; same reasoning as frames_rendered().
  [[nodiscard]] std::uint64_t dropped_triggers() const noexcept {
    return m_published.dropped_triggers.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t garbage_overflows() const noexcept {
    return m_garbage.overflow_count();
  }

  // publish_pad_config() calls refused because the handoff ring was full. A
  // non-zero value means pad edits were rejected — either nothing is rendering,
  // so the ring never drains, or something is republishing in a loop.
  // publish_sequencer() calls refused because the ring was full. Same meaning as
  // rejected_pad_configs(): either nothing is rendering, or something is
  // republishing in a loop.
  [[nodiscard]] std::uint64_t rejected_sequencer_states() const noexcept {
    return m_sequencer_handoff.rejected_count();
  }

  [[nodiscard]] std::uint64_t rejected_pad_configs() const noexcept {
    return m_pad_handoff.rejected_count();
  }

 private:
  // The playhead is pad and frame packed into ONE 64-bit word rather than two
  // atomics, so the UI can never read a frame position from one voice with the
  // pad label of another. Low 56 bits are the frame -- 2.3e16 of them, about
  // 15 000 years at 48 kHz -- and the top 8 are the pad.
  static constexpr std::uint64_t kNothingPlaying = std::numeric_limits<std::uint64_t>::max();
  static constexpr std::uint32_t kPlayheadPadShift = 56;
  static constexpr std::uint64_t kPlayheadFrameMask = (std::uint64_t{1} << kPlayheadPadShift) - 1;

  // Glow is likewise ONE packed word per pad rather than two atomics: age in the
  // low 23 bits, a source flag above it, quantised velocity in the top 8. Same
  // reasoning as the playhead -- separate atomics would let the UI pair one hit's
  // age with another's velocity, and a pad that flashes at the wrong brightness
  // is a visible wrong answer rather than a rounding.
  //
  // M4 took a bit off the age rather than off the velocity. 23 bits of frames is
  // still 174 seconds at 48 kHz, which is five hundred times the longest glow;
  // velocity's 8 bits are what the ramp is actually drawn from, and narrowing
  // them would have changed the M3 pad-lighting behaviour to make room for an M4
  // feature. Shrink the field with headroom to spare, not the one in use.
  //
  // The count saturates one short of the mask so the all-ones sentinel below
  // stays unreachable however long the program runs.
  static constexpr std::uint32_t kGlowVelocityShift = 24;
  static constexpr std::uint32_t kGlowSequencedBit = std::uint32_t{1} << 23U;
  static constexpr std::uint32_t kGlowFrameMask = kGlowSequencedBit - 1;
  static constexpr std::uint32_t kGlowFrameMax = kGlowFrameMask - 1;
  static constexpr std::uint32_t kNeverTriggered = 0xFFFF'FFFFU;

  // Everything the audio thread writes and the UI reads, on its own cache lines.
  //
  // Grouped rather than scattered through the class because these are written
  // every block and the rings below are touched by the control thread: left
  // interleaved, a UI poll would keep invalidating the line the event ring lives
  // on. Same reason CLAUDE.md asks for alignas(kCacheLine) on shared state.
  struct alignas(rt::kCacheLine) Published {
    std::atomic<std::uint64_t> frames_rendered{0};
    std::atomic<std::uint64_t> dropped_triggers{0};

    std::atomic<std::uint64_t> playhead{kNothingPlaying};

    // Where the preview has got to, in frames from the start of its sample, or
    // kNothingPlaying.
    //
    // SEPARATE FROM `playhead`, which packs a pad number alongside the frame and
    // walks only the pad voices. A preview has no pad -- that is the whole point
    // of the audition lane -- so it cannot be folded into a word whose top bits
    // name one. A plain frame count is all it needs.
    std::atomic<std::uint64_t> audition_playhead{kNothingPlaying};

    std::atomic<float> master_peak{0.0F};
    std::array<std::atomic<float>, rt::kNumPads> pad_peak{};
    std::array<std::atomic<float>, rt::kNumPads> strip_peak{};
    std::array<std::atomic<float>, rt::kNumBuses> bus_peak{};
    std::array<std::atomic<float>, rt::kNumPads> strip_reduction{};  // filled with 1.0 in the ctor
    std::atomic<float> limiter_gain{1.0F};

    // Written on trigger and aged once per block. Initialised in the Engine
    // constructor rather than here, because a default-constructed
    // std::atomic<uint32_t> is zero and zero means "hit just now at velocity 0".
    std::array<std::atomic<std::uint32_t>, rt::kNumPads> pad_glow{};

    // Transport, as one packed word for the same reason the playhead is one:
    // the UI must never pair a position from one block with a playing flag from
    // another and draw a stopped transport at a moving position. Playing in the
    // top bit, frames in the low 63 -- 6 million years at 48 kHz.
    std::atomic<std::uint64_t> transport{0};

    // Step, song slot and pattern, packed together for the same reason again:
    // they are read as one description -- "step 7 of pattern 2, slot 3 of the
    // song" -- and a torn read would name a step that pattern does not have, or
    // a pattern that slot does not play. Step in the low 8 bits, slot in the
    // next 8, pattern above that; each field's maximum (32, 64, 16) fits with
    // room to spare.
    std::atomic<std::uint32_t> transport_step{0};
  };

  static constexpr std::uint64_t kTransportPlayingBit = std::uint64_t{1} << 63U;
  static constexpr std::uint64_t kTransportFrameMask = kTransportPlayingBit - 1;
  static constexpr std::uint32_t kTransportStepShift = 0;
  static constexpr std::uint32_t kTransportSlotShift = 8;
  static constexpr std::uint32_t kTransportPatternShift = 16;
  static constexpr std::uint32_t kTransportFieldMask = 0xFFU;

  // AUDIO THREAD, at the top of every block. Adopts whatever the control thread
  // has published and retires what it displaced.
  void adopt_pad_configs() noexcept;
  void adopt_sequencer() noexcept;

  // AUDIO THREAD, at the top of every block, before the position advances.
  void drain_transport() noexcept;

  // AUDIO THREAD, at the top of every block. Applies whatever the control thread
  // has asked the recorder to do, so a punch-in lands on the block it arrived
  // in rather than one block late.
  void drain_record_commands() noexcept;

  // CONTROL THREAD. Whether a new take may be started without splicing it onto
  // the last one.
  [[nodiscard]] bool can_begin_take() const noexcept;

  // AUDIO THREAD, once the master is final. Meters the recorder's source and
  // captures it if a take is running.
  //
  // AFTER THE LIMITER, which is what makes recording the master mean "what you
  // heard" rather than "what you would have heard without the master section".
  void capture_recording(std::span<float* const> channels, std::span<const float* const> input,
                         std::size_t num_frames) noexcept;

  // AUDIO THREAD. Starts one voice on `pad`, and publishes the glow for it.
  //
  // The single path every producer goes through -- the keyboard and MIDI via the
  // event ring, and the sequencer directly. Shared so that a pad lights the same
  // way whatever triggered it.
  //
  // `sequenced` decides which glow the pad shows and whether the listener-time
  // delay applies -- it is the one thing that differs between the three
  // producers, so it is a parameter rather than three copies of this function.
  void start_voice(std::uint8_t pad, float velocity, std::size_t frame_offset,
                   bool sequenced) noexcept;

  // AUDIO THREAD, from start_voice(). Hands a live hit back to the control
  // thread with the transport position it landed on.
  void report_live_hit(std::uint8_t pad, float velocity, std::size_t frame_offset,
                       bool sequenced) noexcept;

  // AUDIO THREAD, at the top of every block. Takes whatever has been published
  // for audition and starts it.
  void adopt_auditions() noexcept;

  // AUDIO THREAD, once per block, per ring. Turns queued PadEvents into voices.
  //
  // Templated on the ring so the keyboard's and MIDI's differ only in capacity
  // and producer thread, not in what happens to an event once it arrives -- a
  // second copy of this loop is how the two paths would drift into behaving
  // differently.
  template <typename Ring>
  void drain_pad_events(Ring& ring, std::size_t num_frames) noexcept;

  // AUDIO THREAD, once per block. Fires every step that falls inside it, at its
  // exact frame.
  void fire_sequencer_steps(std::size_t num_frames) noexcept;

  // The mixer graph's nodes, in the order they are laid out in m_graph_samples:
  // sixteen strips and then four buses. The master is not among them -- it is
  // the caller's output buffer, which is already zeroed and already the thing
  // everything sums into.
  static constexpr std::size_t kNumGraphNodes = rt::kNumPads + rt::kNumBuses;

  // AUDIO THREAD. The channel pointers for one graph node, as render() and
  // VoicePool want them.
  //
  // Returns an empty span when the graph was not allocated (zero channels or a
  // zero block ceiling), which every consumer already treats as "nothing to do"
  // -- rather than a span over a null pointer, which is undefined behaviour
  // before anybody has a chance to check it.
  [[nodiscard]] std::span<float* const> node_channels(std::size_t node) noexcept;

  // AUDIO THREAD, at the top of every block. Zeroes the first num_frames of
  // every strip and bus.
  //
  // EVERY node, including ones no voice will touch. Skipping the silent ones
  // looks free and is not: from M5's EQ onwards a strip fed silence still has a
  // decaying tail, and a skipped strip would truncate it -- a determinism bug
  // wearing the costume of an optimisation. The perf pass is M8, and it should
  // measure before it decides.
  void clear_graph(std::size_t num_frames) noexcept;

  // AUDIO THREAD, after the voices. Sums strips into buses and buses into
  // `channels`, at unity.
  void mix_graph(std::span<float* const> channels, std::size_t num_frames) noexcept;

  // AUDIO THREAD, once per strip per block. Runs the four EQ bands in order.
  //
  // `pad` selects the filter state, which lives here rather than in the config:
  // a PadConfig is immutable and shared, and filter history is neither.
  void apply_eq(std::span<float* const> buffer, const rt::EqConfig& eq, std::size_t pad,
                std::size_t num_frames) noexcept;

  // AUDIO THREAD, once per strip per block, after the EQ. Applies one gain to
  // every channel of each frame, from a detector that is the maximum across
  // them.
  void apply_compressor(std::span<float* const> buffer, const rt::CompressorConfig& config,
                        std::size_t pad, std::size_t num_frames) noexcept;

  // AUDIO THREAD. This pad's mixer settings, or the defaults if nothing has been
  // published to it. An unloaded pad has a strip like any other -- it is simply
  // one with nothing running through it.
  [[nodiscard]] rt::StripConfig strip_config(std::size_t pad) const noexcept;

  // AUDIO THREAD, once per block. Whether ANY strip is soloed.
  //
  // Derived rather than stored, because solo is a property of the set: the
  // question every strip has to ask is not "am I soloed" but "is anything".
  [[nodiscard]] bool any_soloed() const noexcept;

  // AUDIO THREAD, after the voices. Adds the metronome click, if it is on.
  //
  // Additive into the same buffers, so it is part of the output and therefore
  // part of the master peak -- it is audio a listener hears, not an overlay.
  void mix_metronome(std::span<float* const> channels, std::size_t num_frames) noexcept;

  // AUDIO THREAD. Pattern and step packed for publication. Zero when no
  // sequencer state has been published, which reads as step 0 of pattern 0 and
  // is what an untouched sequencer should look like.
  [[nodiscard]] std::uint32_t current_step_word() const noexcept;

  // AUDIO THREAD, once per block, after rendering.
  void publish_telemetry(std::span<float* const> channels, std::size_t num_frames) noexcept;

  Config m_config;
  Published m_published;

  // AUDIO THREAD ONLY. Read every block, written only by adopt_pad_configs().
  // No synchronisation is needed on it precisely because the control thread
  // never touches it — that is the whole point of the handoff ring.
  std::array<std::shared_ptr<const rt::PadConfig>, rt::kNumPads> m_pads{};

  // AUDIO THREAD ONLY, and a MEMBER rather than a local in adopt_pad_configs()
  // on purpose.
  //
  // A displaced config that the garbage ring refuses has to survive to the next
  // block: it cannot stay on the audio thread's stack, because letting a local
  // shared_ptr go out of scope there would run the destructor this whole
  // mechanism exists to avoid. Holding it here means the retry is free and the
  // failure mode is "one block late", not "freed in the callback".
  std::shared_ptr<const rt::PadConfig> m_retiring;

  // AUDIO THREAD ONLY. What the sequencer is playing, and where the transport
  // is. Null until something is published, which is the normal state for a
  // session that never touches the sequencer.
  std::shared_ptr<const rt::SequencerState> m_sequencer;
  std::shared_ptr<const rt::SequencerState> m_retiring_sequencer;
  rt::Transport m_transport;

  rt::SpscRing<rt::PadEvent, kEventRingCapacity> m_events;
  rt::SpscRing<rt::PadEvent, kMidiRingCapacity> m_midi_events;
  rt::SpscRing<rt::TransportCommand, kTransportRingCapacity> m_transport_commands;
  rt::SpscRing<rt::MasterConfig, kMasterRingCapacity> m_master_commands;
  rt::HandoffRing<rt::PadConfig, kPadHandoffCapacity> m_pad_handoff;
  rt::HandoffRing<rt::SequencerState, kSequencerHandoffCapacity> m_sequencer_handoff;
  rt::VoicePool<kMaxVoices> m_voices;

  // The audition lane: its own ring, its own voices, its own retiring slot.
  // Nothing here is reachable from the pad path and that is the point.
  rt::HandoffRing<rt::PadConfig, kAuditionHandoffCapacity> m_audition_handoff;
  rt::VoicePool<kAuditionVoices> m_audition_voices;
  std::shared_ptr<const rt::PadConfig> m_retiring_audition;

  rt::GarbageRing<kGarbageRingCapacity> m_garbage;

  // The capture lane. The recorder itself is shared between the threads and says
  // in its own header which half owns what; everything below it here is storage
  // allocated ONCE in the constructor, exactly like the mixer graph.
  // AUDIO -> CONTROL, the only ring in this class pointing that way. Live pad
  // hits waiting to be written into a pattern.
  rt::SpscRing<rt::PadHit, kHitRingCapacity> m_hits;
  std::atomic<std::uint64_t> m_dropped_hits{0};

  rt::SpscRing<rt::RecordCommand, kRecordRingCapacity> m_record_commands;
  rt::Recorder m_recorder;
  std::vector<float> m_record_storage;
  std::vector<float> m_record_preroll;
  std::vector<rt::RecordChunk> m_record_chunks;

  // AUDIO THREAD ONLY. Channel pointers for the master tap.
  //
  // Preallocated because the alternative is building the array in the callback:
  // render() is handed std::span<float* const> and rt::Recorder wants
  // std::span<const float* const>, which is not a conversion, so the pointers
  // have to be copied somewhere. Somewhere is here, once.
  std::vector<const float*> m_record_heads;

  // CONTROL THREAD ONLY. The take being assembled, one vector per channel --
  // the layout rt::Sample wants, so whoever finishes the take copies rather
  // than de-interleaves.
  //
  // THIS is what grows without bound, on the control thread where growing is
  // legal. Four minutes of stereo at 48 kHz is 92 MB, which is the honest cost
  // of an unbounded take and is why the pool below it can stay small.
  std::vector<std::vector<float>> m_take;
  std::size_t m_take_frames = 0;

  // Frames drained from the recorder since this take began, INCLUDING ones
  // discard_take() threw away. Separate from m_take_frames above because it
  // answers a different question -- "has everything the audio thread captured
  // been accounted for", which is what take_complete() needs and which emptying
  // the take must not change the answer to.
  std::uint64_t m_collected_frames = 0;

  // How many channels the recorder was given, which is the engine's channel
  // count clamped to rt::kMaxRecordChannels. Wider engines record their first
  // eight channels; nothing in this project builds one.
  std::uint16_t m_record_channels = 0;

  // AUDIO THREAD ONLY after construction. The mixer graph's sample storage:
  // kNumGraphNodes nodes, each num_channels wide and max_block_frames long, in
  // ONE allocation made on the control thread in the constructor.
  //
  // A vector rather than a std::array because the size depends on Config, and
  // one flat vector rather than a vector per node because the audio thread must
  // never see a container it could be tempted to resize. Nothing after the
  // constructor touches its size; render() only writes through the pointers
  // below, which is why this is safe to reach from the callback at all.
  //
  // 320 KB at the defaults (20 nodes x 2 channels x 2048 frames x 4 bytes).
  std::vector<float> m_graph_samples;

  // Channel pointers into m_graph_samples, node-major: node n's channels are at
  // [n * num_channels, (n + 1) * num_channels). Built once, so producing the
  // span a node needs is a multiply rather than anything that could allocate.
  std::vector<float*> m_graph_channels;

  // AUDIO THREAD ONLY. This block's post-fader levels, measured in mix_graph()
  // where audibility is already known and read by publish_telemetry() a few
  // lines later. Plain floats: one thread writes them and the same thread reads
  // them, so an atomic would buy nothing.
  std::array<float, rt::kNumPads> m_strip_peak{};
  std::array<float, rt::kNumBuses> m_bus_peak{};

  // AUDIO THREAD ONLY. EQ filter state, one biquad per pad per band per channel,
  // laid out as ((pad * kEqBands) + band) * num_channels + channel and allocated
  // once in the constructor.
  //
  // Here rather than in rt::StripConfig on purpose: a published config is
  // immutable and shared, and filter history belongs to one strip in one engine.
  // Putting state in the config would mean two engines rendering the same
  // project shared a filter, which is not a thing filters can do.
  std::vector<rt::Biquad> m_eq_state;

  // AUDIO THREAD ONLY. One compressor per strip -- not per channel, because the
  // detector is the maximum across channels and two envelopes would be two
  // different gains. A plain array rather than a vector: the size does not
  // depend on the channel count.
  std::array<rt::Compressor, rt::kNumPads> m_strip_compressor{};

  // AUDIO THREAD ONLY. The master limiter, and the settings it is running with.
  // The delay line is allocated in the constructor.
  rt::Limiter m_limiter;
  rt::MasterConfig m_master{};

  // This block's worst gain reduction per strip, as a LINEAR gain with the
  // makeup divided back out. 1.0 means none. Converted to dB at the interface
  // boundary like every other level in the program.
  std::array<float, rt::kNumPads> m_strip_reduction{};

  // CONTROL THREAD ONLY: what this thread has published, so pad_config() can
  // answer without reading the audio thread's table. Not a cache of m_pads — it
  // is deliberately one block *ahead* of it.
  std::array<std::shared_ptr<const rt::PadConfig>, rt::kNumPads> m_published_pads{};
  std::shared_ptr<const rt::SequencerState> m_published_sequencer;

  // CONTROL THREAD ONLY: what this thread has published, so limiter() can answer
  // without reading the audio thread's copy.
  rt::MasterConfig m_published_master{};

  // Written by the control thread only, and read by it -- no cross-thread access,
  // so no atomic.
  std::uint64_t m_dropped_events = 0;

  // The MIDI equivalent, and ATOMIC where the one above is not: it is written by
  // the MIDI thread and read by the UI. Making it a plain uint64_t would be a
  // data race for the sake of saving nothing -- the same mistake frames_rendered
  // was fixed for in M1. Relaxed: it is a progress counter and orders nothing.
  std::atomic<std::uint64_t> m_dropped_midi_events{0};
};

}  // namespace engine

#endif  // CRATEDIG_ENGINE_ENGINE_HPP
