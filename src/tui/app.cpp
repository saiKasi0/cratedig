#include "tui/app.hpp"

#include "engine/engine.hpp"
#include "ingest/decoder.hpp"
#include "ingest/peak_pyramid.hpp"
#include "ingest/slices.hpp"
#include "io/audio_device.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "rt/sequencer.hpp"
#include "tui/command.hpp"
#include "tui/keys.hpp"
#include "tui/render.hpp"
#include "tui/render_detail.hpp"
#include "tui/ui_state.hpp"
#include "tui/waveform.hpp"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace tui {
namespace {

// What the strip's chain rows say. Short enough for eight columns, and naming
// the BAND COUNT rather than a type, because "2bd" answers "is anything on" at a
// glance and the curve above it answers "doing what".
[[nodiscard]] std::string eq_label_of(const rt::EqConfig& eq) {
  std::size_t on = 0;
  for (const rt::EqBand& band : eq.bands) {
    on += band.enabled ? 1 : 0;
  }
  return on == 0 ? std::string{"--"} : std::to_string(on) + "bd";
}

[[nodiscard]] std::string comp_label_of(const rt::CompressorConfig& compressor) {
  if (!compressor.enabled) {
    return "--";
  }
  // The ratio, which is the one number that says what a compressor is doing.
  const auto whole = static_cast<int>(compressor.ratio);
  return std::to_string(whole) + ":1";
}

// The EQ magnitude response across the audible band, in dB, at the braille
// resolution render_mix draws it at.
//
// Log-spaced from 20 Hz to 20 kHz, because an EQ curve drawn on a linear
// frequency axis spends three quarters of its width above 5 kHz and squeezes
// everything anybody actually adjusts into the first inch.
//
// Evaluated from the SAME published coefficients the audio thread is running,
// so the curve cannot drift from the sound: |H(e^jw)| out of b0..a2, exactly as
// docs/MIXER.md's acceptance defines it.
[[nodiscard]] std::vector<float> eq_response(const rt::EqConfig& eq, std::uint32_t rate) {
  if (!eq.any_enabled() || rate == 0) {
    return {};
  }
  constexpr std::size_t kPoints = 16;
  std::vector<float> curve(kPoints, 0.0F);
  for (std::size_t index = 0; index < kPoints; ++index) {
    const double position = static_cast<double>(index) / static_cast<double>(kPoints - 1);
    const double frequency = 20.0 * std::pow(1000.0, position);  // 20 Hz .. 20 kHz
    const double omega = 2.0 * std::acos(-1.0) * frequency / static_cast<double>(rate);
    const std::complex<double> z = std::polar(1.0, -omega);

    double total_db = 0.0;
    for (const rt::EqBand& band : eq.bands) {
      if (!band.enabled) {
        continue;
      }
      const std::complex<double> numerator = static_cast<double>(band.coeffs.b0) +
                                             (static_cast<double>(band.coeffs.b1) * z) +
                                             (static_cast<double>(band.coeffs.b2) * z * z);
      const std::complex<double> denominator = 1.0 + (static_cast<double>(band.coeffs.a1) * z) +
                                               (static_cast<double>(band.coeffs.a2) * z * z);
      // Cascaded bands MULTIPLY, so their dB contributions add.
      total_db += 20.0 * std::log10(std::abs(numerator / denominator));
    }
    curve[index] = static_cast<float>(total_db);
  }
  return curve;
}

// Comfortably above anything a device will negotiate. The engine's buffers are
// sized from this at construction, before the device has told us what it
// actually granted, so it has to be a ceiling rather than a guess -- and open()
// is checked against it afterwards.
constexpr std::uint32_t kMaxBlockFrames = 8'192;

// ~30 Hz. The design is dense but calm (docs/design/DESIGN_BRIEF.md) and nothing
// on screen changes faster than the eye reads it, so a higher rate would only
// spend CPU that the audio thread would rather have.
constexpr auto kFrameInterval = std::chrono::milliseconds(33);

constexpr std::uint8_t kPad = 0;

// The keys that are not characters, named. Their codes are the legacy control
// bytes, which is what the Kitty protocol reports for them and what FTXUI's own
// events are built from -- so one set of names covers both paths.
constexpr std::uint32_t kTab = 9;
constexpr std::uint32_t kReturn = 13;
constexpr std::uint32_t kEscape = 27;
constexpr std::uint32_t kSpace = 32;
constexpr std::uint32_t kBackspace = 127;

// What a keystroke contributes to the command line, or nothing.
//
// Control codes are excluded by hand rather than by asking a locale: the
// command line has to behave the same in every environment, and a Tab or a
// Backspace appended as a literal byte would be a character the prompt cannot
// draw and Backspace cannot cleanly remove.
[[nodiscard]] std::string printable(std::uint32_t code) {
  if (code < kSpace || code == kBackspace || code >= kFirstFunctionalKey) {
    return {};
  }
  return utf8_encode(code);
}

// FTXUI's own events, in the vocabulary the bindings are written in.
//
// Always kPress, because that is all this path can know -- a terminal without
// the protocol never says a key was released. Note that `key` is the TYPED
// character here rather than the unshifted one, which is why the bindings use
// character() and not key: it is the only field both paths fill in the same
// way.
[[nodiscard]] std::optional<KeyEvent> legacy_key(const ftxui::Event& event) {
  if (event.is_character()) {
    const std::uint32_t code = utf8_decode_first(event.character());
    if (code == 0) {
      return std::nullopt;
    }
    return KeyEvent{.key = code, .text = code};
  }
  if (event == ftxui::Event::Escape) {
    return KeyEvent{.key = kEscape};
  }
  if (event == ftxui::Event::Return) {
    return KeyEvent{.key = kReturn};
  }
  if (event == ftxui::Event::Backspace) {
    return KeyEvent{.key = kBackspace};
  }
  if (event == ftxui::Event::Tab) {
    return KeyEvent{.key = kTab};
  }
  if (event == ftxui::Event::ArrowLeft) {
    return KeyEvent{.key = kKeyArrowLeft};
  }
  if (event == ftxui::Event::ArrowRight) {
    return KeyEvent{.key = kKeyArrowRight};
  }
  return std::nullopt;  // mouse, resize, a sequence we do not bind
}

// Writes straight to the terminal, around FTXUI.
//
// The negotiation is a conversation with the terminal rather than something to
// draw, and FTXUI has no channel for it. Unbuffered and unchecked: a terminal
// that will not take four bytes has already gone away, and there is nothing
// useful to do about it from inside the event loop.
void write_terminal(std::string_view bytes) {
  static_cast<void>(::write(STDOUT_FILENO, bytes.data(), bytes.size()));
}

// A step is an eighth of the view, so scrolling feels the same at every zoom --
// the alternative, a fixed number of frames, is either glacial when zoomed out
// or a jump-cut when zoomed in.
constexpr std::size_t kScrollDivisor = 8;
constexpr double kZoomStep = 0.5;

// What a pad chopped from slice N is called.
//
// Zero-padded and short on purpose: a pad cell is ten columns wide and the
// borders take two, so "chop10" arrives on screen as "chop1" -- a name that
// lies about which slice it plays. "s01".."s16" fits whole at every number.
[[nodiscard]] std::string slice_pad_name(std::size_t slice_number) {
  const std::string digits = std::to_string(slice_number);
  return "s" + (digits.size() < 2 ? "0" + digits : digits);
}

// Slices onto pads, one for one, and every pad past the last slice cleared.
//
// Returns how many pads were actually reconfigured. A short count means the
// handoff ring refused a publish, which the caller reports rather than hides:
// half a chop that looks like a whole one is the kind of thing you discover
// mid-performance.
//
// Sixteen at a time; banks are M5. Slices past the sixteenth still exist in the
// SliceSet and the wave panel draws them, so nothing is lost -- there is just
// nothing to play them with yet.
[[nodiscard]] std::size_t apply_slices(engine::Engine& engine,
                                       const std::shared_ptr<const rt::Sample>& sample,
                                       const ingest::SliceSet& set, UiState& state) {
  std::size_t published = 0;
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    const bool has_slice = pad < set.size() && sample != nullptr;

    // Built fresh and published whole. A PadConfig is immutable once the audio
    // thread can see it, so "clear this pad" is a new config with no sample --
    // which the engine treats as silent rather than as an error.
    rt::PadConfig config{};
    config.pad = static_cast<std::uint8_t>(pad);
    if (has_slice) {
      config.sample = sample;
      config.start_frame = set.slices[pad].start_frame;
      config.end_frame = set.slices[pad].end_frame;
    }

    if (!engine.publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(config)))) {
      return published;
    }
    ++published;

    state.pads[pad].loaded = has_slice;
    state.pads[pad].has_slice = has_slice;
    state.pads[pad].slice_index = pad;
    state.pads[pad].name = has_slice ? slice_pad_name(pad + 1) : std::string{};
  }
  return published;
}

// Deletes one CHARACTER, not one byte.
//
// Backspacing a multi-byte character a byte at a time leaves a truncated
// sequence on the line, which draws as a replacement glyph -- and then the next
// Backspace removes another byte of it rather than the glyph, so the prompt
// appears to have stopped responding.
void pop_codepoint(std::string& text) {
  while (!text.empty()) {
    const auto byte = static_cast<unsigned char>(text.back());
    text.pop_back();
    if ((byte & 0xC0U) != 0x80U) {
      break;  // ASCII, or the lead byte: that was the whole character
    }
  }
}

// What the wave panel draws. Kept in step with the engine by being written in
// the same place the configs are published.
void show_slices(const ingest::SliceSet& set, UiState& state) {
  state.slices.clear();
  state.slices.reserve(set.size());
  for (const ingest::Slice& slice : set.slices) {
    state.slices.push_back(SliceMark{.start_frame = slice.start_frame,
                                     .end_frame = slice.end_frame,
                                     .start_snap = slice.start_snap,
                                     .end_snap = slice.end_snap});
  }
  state.chop_algorithm = set.algorithm == ingest::ChopAlgorithm::kTransient
                             ? std::string{"transient"}
                             : "grid " + std::to_string(set.size());
  if (state.edit.slice >= state.slices.size()) {
    state.edit.slice = 0;
  }
}

// -- EDIT ---------------------------------------------------------------------

// How much room the slice gets around it when EDIT opens on one, as a fraction
// of the slice's own length on each side.
//
// A boundary with nothing on the far side of it cannot be judged: the whole
// question EDIT answers is "did this cut land in the right place", and that is a
// question about both sides.
constexpr double kEditContext = 0.35;

// How far a nudge moves a boundary. ONE FRAME, as the mockup's `h -1 ┻ +1 l`
// says: this is the screen where sample-resolution work happens, and anything
// coarser would need its own unit printed beside it. Holding the key repeats,
// which is what puts a hundred frames within reach.
constexpr std::size_t kNudgeFrames = 1;

// Each `z` multiplies the visible span by this, wrapping back to the whole slice
// once a single frame per column is reached. One key, one direction, no state to
// remember -- which is what the mockup's bare `z zoom` has to mean.
constexpr double kEditZoomStep = 0.25;

// One undoable boundary edit: the whole slice rather than a delta, so undo does
// not have to know which way it moved or replay anything to get back.
struct SliceEdit {
  std::size_t index = 0;
  std::size_t start_frame = 0;
  std::size_t end_frame = 0;
};

// The pad that plays a slice, or rt::kNumPads.
[[nodiscard]] std::uint8_t pad_for_slice(const UiState& state, std::size_t slice) noexcept {
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    if (state.pads[pad].has_slice && state.pads[pad].slice_index == slice) {
      return static_cast<std::uint8_t>(pad);
    }
  }
  return rt::kNumPads;
}

}  // namespace

int run_app(const AppOptions& options) {
  // Checked before anything expensive: refusing after decoding a five-minute
  // file would be a slow way to learn the output is a pipe. FTXUI redirects
  // *input* from /dev/tty when stdin is piped, but it has nowhere to send the
  // interface if stdout is not a terminal.
  if (isatty(STDOUT_FILENO) == 0) {
    std::cerr << "error: stdout is not a terminal — cratedig's interface needs one.\n"
              << "       for scripted use see: cratedig --list-devices, cratedig --version\n";
    return 1;
  }

  // 1. Load. On this thread, before anything real-time exists, which is exactly
  //    where decoding belongs.
  std::shared_ptr<const rt::Sample> sample;
  std::string sample_name;
  ingest::PeakPyramid pyramid;

  if (!options.sample_path.empty()) {
    const ingest::SampleLoad load = ingest::load_sample(options.sample_path, options.sample_rate);
    if (!load.ok()) {
      std::cerr << "error: " << ingest::describe(load.error);
      if (!load.detail.empty()) {
        std::cerr << " (" << load.detail << ")";
      }
      std::cerr << '\n';
      return 1;
    }
    sample = load.sample;
    sample_name = options.sample_path.filename().string();

    // Built here, on the control thread, before anything real-time exists.
    // ~100-200 ms for a five-minute file; moving it onto a worker so the
    // interface can come up first is M6's ingest job, not M2's.
    pyramid = ingest::PeakPyramid::build(*sample);
  }

  // 2. Build the engine and assign the pad.
  //
  //    The assignment goes through the handoff ring like every other pad edit,
  //    so it takes effect on the first render() rather than immediately. Before
  //    the device is even open there is nothing to be one block behind, and
  //    doing it the same way here as at `:chop` time means there is only one
  //    path to get right.
  engine::Engine engine{engine::Engine::Config{.sample_rate = options.sample_rate,
                                               .num_channels = 2,
                                               .max_block_frames = kMaxBlockFrames,
                                               .seed = 0}};
  if (sample != nullptr && !engine.set_pad_sample(kPad, sample)) {
    // Only reachable if the handoff ring is full, which cannot happen on the
    // very first publish. Checked anyway rather than discarded: a pad that
    // silently failed to load would present as "the spacebar does nothing".
    std::cerr << "error: could not assign " << sample_name << " to pad 1\n";
    return 1;
  }

  // The sequencer, control-side.
  //
  // The MUTABLE ORIGINAL. What the audio thread reads is an immutable copy
  // published through the handoff ring, exactly as pad configs are, so every
  // edit is made here and then published whole. ~16 KB copied per keystroke,
  // which is what rt::SequencerState's own comment weighs against a shared
  // mutable structure and rejects.
  rt::SequencerState sequencer;

  // Published before anything renders, so that what the lane draws and what the
  // engine holds are the same object from the first frame. Without it the
  // interface would show "pattern 01, 16 steps" while the engine had no pattern
  // at all -- true enough to look right and wrong in the way that matters, since
  // the first edit would appear to change something that had not existed.
  if (!engine.publish_sequencer(std::make_shared<const rt::SequencerState>(sequencer))) {
    std::cerr << "error: could not publish the initial sequencer state\n";
    return 1;
  }

  // 3. Open the device, unless we were told not to.
  io::AudioDevice device;
  bool audio_running = false;

  if (!options.no_audio) {
    const io::AudioDevice::Config device_config{.sample_rate = options.sample_rate,
                                                .num_channels = 2,
                                                .block_frames = options.block_frames,
                                                .device_id = options.device_id};
    const io::DeviceError opened = device.open(engine, device_config);
    if (opened != io::DeviceError::kNone) {
      std::cerr << "error: " << io::describe(opened);
      if (!device.last_error().empty()) {
        std::cerr << " (" << device.last_error() << ")";
      }
      std::cerr << "\ntry: cratedig --list-devices, or --no-audio to run without one\n";
      return 1;
    }

    // The device decides the block size, not us. If it hands back something
    // larger than the engine was built for, stop -- rendering past the end of
    // the engine's contract is not something to discover as a crackle.
    if (device.actual_block_frames() > engine.config().max_block_frames) {
      std::cerr << "error: device wants " << device.actual_block_frames()
                << "-frame blocks, engine is built for " << engine.config().max_block_frames
                << '\n';
      return 1;
    }

    if (const io::DeviceError started = device.start(); started != io::DeviceError::kNone) {
      std::cerr << "error: " << io::describe(started) << " (" << device.last_error() << ")\n";
      return 1;
    }
    audio_running = true;
  }

  // 4. The interface. render() is a pure function of a UiState, so everything
  //    below is about assembling that struct once per frame; the layout itself
  //    lives in src/tui/render.cpp and is what the snapshot tests exercise.
  ftxui::App screen = ftxui::App::Fullscreen();

  // Nothing in this interface responds to the mouse, and leaving tracking on
  // makes a terminal emit mouse escape sequences into the PTY snapshot tests.
  screen.TrackMouse(false);

  const std::size_t total_frames = sample != nullptr ? sample->num_frames() : 0;

  UiState state;
  state.version = CRATEDIG_VERSION;
  state.engine_rate = options.sample_rate;
  state.max_voices = engine::Engine::kMaxVoices;
  if (sample != nullptr) {
    state.sample_name = sample_name;
    state.sample_rate = sample->sample_rate();
    state.sample_channels = sample->num_channels();
    state.sample_frames = total_frames;
    state.pads[kPad] =
        PadState{.name = options.sample_path.stem().string(), .level = 0.0F, .loaded = true};
    state.view.fit(total_frames);
  }
  if (!options.no_audio) {
    state.audio_api = device.api_name();
    state.block_frames = device.actual_block_frames();
  }

  // The chops, control-side. UiState carries a display copy in frames; this is
  // the one `slot assign` reads, because it is the only place a slice NUMBER
  // still means something.
  ingest::SliceSet slices;

  // The keyboard negotiation, in two bits. `asked` says the query has gone out;
  // `active` says a terminal answered it and the flags are pushed. There is no
  // third state to wait in: a terminal that does not implement the protocol
  // never replies, so silence is the answer and no timeout is needed.
  bool kitty_asked = false;
  bool kitty_active = false;

  auto set_message = [&state](std::string text, bool is_error) {
    state.message = std::move(text);
    state.message_is_error = is_error;
  };

  // Leaves the keyboard as we found it, on the way out.
  //
  // Sent from INSIDE the loop, while the alternate screen is still up. The
  // protocol's flag stack is per-screen, so popping after FTXUI has switched
  // back would pop the MAIN screen's stack -- state belonging to the shell we
  // came from. If this never runs (a crash, a signal), leaving the alternate
  // screen discards our stack anyway; that is what makes the feature safe to
  // ship, and this is only the tidy path.
  auto quit = [&] {
    if (kitty_active) {
      write_terminal(kKittyPop);
      kitty_active = false;
    }
    screen.Exit();
  };

  // The boundary edits that `u` can take back. Control-side, like the slice set
  // itself; nothing on the audio thread has any idea this exists.
  std::vector<SliceEdit> undo;

  // Where the next step edit lands, and whether the transport has been told to
  // run.
  //
  // The transport flag is what THIS thread asked for, and it is what decides
  // which way `p` toggles. The mode line reads the telemetry instead, which is
  // what the audio thread is actually doing -- the two differ for one block
  // with a device, and permanently under --no-audio, where nothing renders and
  // so nothing ever runs. Saying so is the honest reading of that mode.
  std::size_t step_cursor = 0;
  bool transport_asked = false;

  // Applies an edit to the sequencer and publishes it, ATOMICALLY as far as
  // this thread is concerned: the local copy only moves once the audio thread
  // has accepted the new state. A refused publish therefore leaves the
  // interface showing what is actually playing rather than an edit that never
  // landed -- which is the same promise publish_pad_config() makes, and it is
  // worth keeping because a half-applied pattern is not something you would
  // diagnose quickly.
  auto edit_sequencer = [&](const auto& mutate, std::string said) {
    rt::SequencerState next = sequencer;
    mutate(next);

    // Published unconditionally, including with no device open. Nothing drains
    // the handoff rings without a stream, so this used to fill them and then
    // refuse every later edit for the rest of the session -- the frame tick now
    // calls Engine::adopt_offline() in that mode, which is the one place that
    // has to know, rather than every publisher guarding itself.
    if (!engine.publish_sequencer(std::make_shared<const rt::SequencerState>(next))) {
      set_message("sequencer busy — the edit did not happen, try again", true);
      return;
    }
    sequencer = next;
    if (!said.empty()) {
      set_message(std::move(said), false);
    }
  };

  // The pattern the lane is showing, which is the one every edit lands in.
  //
  // ALWAYS THE SELECTED ONE, even while a song is playing something else. The
  // alternative -- following the transport -- would mean `t` wrote into
  // whichever pattern the song had reached, so holding the key through a
  // pattern change would scatter hits across two of them. The playhead marker
  // is suppressed instead when the transport is elsewhere, and the caption's
  // `slot N` is what says the song is still running.
  auto lane_pattern = [&]() -> std::size_t {
    return std::min<std::size_t>(sequencer.selected_pattern, rt::kMaxPatterns - 1);
  };

  // Kept inside the pattern, which `pattern length` can shrink underneath it.
  auto clamp_cursor = [&] {
    const std::size_t length = rt::pattern_length(sequencer.patterns[lane_pattern()]);
    step_cursor = std::min(step_cursor, length - 1);
  };

  // Start or stop, ALWAYS FROM THE TOP.
  //
  // One key toggles the transport, so `p` has to mean the same thing every time
  // it is pressed. Resuming from wherever it stopped would start the pattern at
  // a different step depending on what happened earlier -- and after a tempo
  // change the frame it stopped at is not even the same step any more, so
  // "resume" would not resume.
  //
  // Two commands rather than one with a flag: the seek and the state change are
  // separate kinds precisely so that seeking cannot start playback by accident
  // (rt::sequencer.hpp), and the ring drains in order.
  auto set_transport = [&](bool play) {
    // Nothing drains the transport ring without a stream, for the same reason
    // nothing drains the sequencer handoff (see edit_sequencer above). Two
    // commands per keystroke would fill thirty-two slots in sixteen presses --
    // harmless, since a transport with nothing rendering does nothing either
    // way, but it is a queue of orders to a thread that does not exist.
    if (!audio_running) {
      return;
    }
    static_cast<void>(engine.send_transport(
        rt::TransportCommand{.kind = rt::TransportCommandKind::kSeek, .position_frames = 0}));
    static_cast<void>(engine.send_transport(rt::TransportCommand{
        .kind = play ? rt::TransportCommandKind::kPlay : rt::TransportCommandKind::kStop}));
  };

  // Stop everything that is sounding, and the transport with it.
  //
  // BOTH, because voices alone would not produce silence: a running sequencer
  // retriggers the pads within a step, so the panic would last a fraction of a
  // beat and read as not having worked. `:stop N` is the surgical version and
  // deliberately leaves the transport alone.
  auto panic = [&] {
    static_cast<void>(engine.trigger_pad(rt::PadEvent{.kind = rt::PadEventKind::kStopAll}));
    transport_asked = false;
    set_transport(false);
  };

  // The last width the interface was drawn at.
  //
  // Key handling needs it -- how many zero crossings are worth collecting is a
  // question about columns -- and the event loop is not told the terminal size.
  // Stale by at most one frame, and the only consequence of that is collecting
  // slightly the wrong number of ticks for one redraw.
  std::size_t last_columns = kMinColumns;

  // Everything EDIT shows that is derived rather than stored: the zero crossings
  // in view and the current pad's envelope.
  //
  // Called after anything that moves the view or changes the slice -- NOT once
  // per frame. Scanning a view for sign changes is cheap but not free, and the
  // answer only changes when one of those two does.
  auto refresh_edit = [&](std::size_t columns) {
    state.edit.zero_crossings.clear();
    state.edit.pad_known = false;
    state.edit.undo_depth = undo.size();
    if (sample == nullptr || state.edit.slice >= state.slices.size()) {
      return;
    }

    // One tick per two columns is about where a ruler stops being one. Past
    // that, ingest::zero_crossings_in returns nothing and the row goes blank --
    // which is the honest picture of "zoomed too far out to see crossings", and
    // the row fills in again as you zoom toward sample resolution.
    const std::size_t limit = std::max<std::size_t>(edit_wave_columns_for(columns) / 2, 1);
    state.edit.zero_crossings = ingest::zero_crossings_in(*sample, state.edit.view.first_frame,
                                                          state.edit.view.frames_visible, limit);
    if (state.edit.zero_crossings.size() == limit) {
      state.edit.zero_crossings.clear();
    }

    const std::uint8_t pad = pad_for_slice(state, state.edit.slice);
    if (pad >= rt::kNumPads) {
      return;
    }
    const std::shared_ptr<const rt::PadConfig> config = engine.pad_config(pad);
    if (config == nullptr) {
      return;
    }
    state.edit.pad = pad;
    state.edit.pad_known = true;

    // Frames to milliseconds happens HERE, on the thread that knows the rate.
    // UiState carries times so the renderer -- and therefore every snapshot
    // test -- needs no rate at all.
    const auto rate = static_cast<float>(std::max(options.sample_rate, 1U));
    const auto to_ms = [rate](std::size_t frames) {
      return static_cast<float>(frames) * 1000.0F / rate;
    };
    state.edit.envelope = EnvelopeView{.attack_ms = to_ms(config->env.attack),
                                       .decay_ms = to_ms(config->env.decay),
                                       .sustain = config->env.sustain,
                                       .release_ms = to_ms(config->env.release),
                                       .gate = config->trigger == rt::TriggerMode::kGate,
                                       .choke_group = config->choke_group,
                                       .gain = config->gain,
                                       .pitch_ratio = config->pitch_ratio};
  };

  // Frames the EDIT view, centred on the slice with context on both sides.
  auto frame_slice = [&](std::size_t columns) {
    if (state.edit.slice >= state.slices.size()) {
      return;
    }
    const SliceMark& slice = state.slices[state.edit.slice];
    const std::size_t length =
        slice.end_frame > slice.start_frame ? slice.end_frame - slice.start_frame : 1;
    const auto margin = static_cast<std::size_t>(static_cast<double>(length) * kEditContext);
    state.edit.view.first_frame = slice.start_frame > margin ? slice.start_frame - margin : 0;
    state.edit.view.frames_visible = length + (2 * margin);
    state.edit.view.clamp(total_frames);
    refresh_edit(columns);
  };

  // Republishes the pad that plays a slice, so an edited boundary is audible
  // rather than merely drawn. Built from the current config, so a nudge does not
  // reset the envelope somebody set a moment ago.
  auto republish_slice = [&](std::size_t index) {
    const std::uint8_t pad = pad_for_slice(state, index);
    if (pad >= rt::kNumPads || sample == nullptr || index >= slices.size()) {
      return;
    }
    const std::shared_ptr<const rt::PadConfig> current = engine.pad_config(pad);
    rt::PadConfig next = current != nullptr ? *current : rt::PadConfig{};
    next.pad = pad;
    next.sample = sample;
    next.start_frame = slices.slices[index].start_frame;
    next.end_frame = slices.slices[index].end_frame;
    static_cast<void>(engine.publish_pad_config(std::make_shared<const rt::PadConfig>(next)));
  };

  // Moves one boundary of the current slice by `delta` frames.
  //
  // The slice keeps at least one frame: a zero-length slice is a pad that makes
  // no sound, which looks exactly like a broken one. Boundaries are allowed to
  // pass their neighbours' -- overlapping slices are a legitimate thing to build
  // deliberately, and refusing would make the last frame before a neighbour
  // unreachable.
  auto nudge = [&](bool end_boundary, std::ptrdiff_t delta, std::size_t columns) {
    if (state.edit.slice >= slices.size() || sample == nullptr) {
      return;
    }
    ingest::Slice& slice = slices.slices[state.edit.slice];
    undo.push_back(SliceEdit{
        .index = state.edit.slice, .start_frame = slice.start_frame, .end_frame = slice.end_frame});

    const auto move = [&](std::size_t frame) {
      const auto moved = static_cast<std::ptrdiff_t>(frame) + delta;
      return static_cast<std::size_t>(
          std::clamp<std::ptrdiff_t>(moved, 0, static_cast<std::ptrdiff_t>(total_frames)));
    };
    if (end_boundary) {
      slice.end_frame = std::max(move(slice.end_frame), slice.start_frame + 1);
      // Nudged by hand, so whatever the snap did to it is no longer the story.
      slice.end_snap = 0;
    } else {
      slice.start_frame = std::min(move(slice.start_frame), slice.end_frame - 1);
      slice.start_snap = 0;
    }

    show_slices(slices, state);
    republish_slice(state.edit.slice);
    refresh_edit(columns);
  };

  auto undo_edit = [&](std::size_t columns) {
    if (undo.empty()) {
      set_message("nothing to undo", true);
      return;
    }
    const SliceEdit last = undo.back();
    undo.pop_back();
    if (last.index < slices.size()) {
      slices.slices[last.index].start_frame = last.start_frame;
      slices.slices[last.index].end_frame = last.end_frame;
      show_slices(slices, state);
      republish_slice(last.index);
    }
    state.edit.slice = last.index;
    refresh_edit(columns);
  };

  // Read-modify-publish one strip, the same shape republish_slice() uses.
  //
  // A published PadConfig is immutable, so every mixer edit is a new object --
  // which is the one-pointer rule doing its job rather than getting in the way.
  // Built from what is currently published, so setting a fader does not reset an
  // EQ somebody dialled in a moment ago.
  //
  // Returns false when the handoff ring is full, which the caller reports rather
  // than swallowing: an edit that did not reach the audio thread must not look
  // like one that did.
  auto edit_strip = [&](std::size_t pad_1based, auto&& mutate) -> bool {
    const auto pad = static_cast<std::uint8_t>(pad_1based - 1);
    if (pad >= rt::kNumPads) {
      return false;
    }
    rt::StripConfig strip = engine.strip(pad);
    mutate(strip);
    return engine.set_strip(pad, strip);
  };

  // Says what happened, or says the ring was full. One place, so no mixer verb
  // can quietly forget the failure case.
  auto report_strip = [&](bool ok, const std::string& said) {
    if (ok) {
      set_message(said, false);
    } else {
      set_message("mixer busy — the edit did not happen, try again", true);
    }
  };

  // Milliseconds to frames, at the engine's rate. The interface talks in time
  // and rt::CompressorConfig counts frames, for the reason rt::AdsrFrames does:
  // a block size must not be able to change how long an attack is.
  auto frames_of_ms = [&](float milliseconds) {
    const double frames =
        static_cast<double>(milliseconds) * static_cast<double>(options.sample_rate) / 1000.0;
    return static_cast<std::size_t>(frames < 0.0 ? 0.0 : frames);
  };

  // Runs a parsed command. Everything it touches -- the engine's handoff ring,
  // the slice set, the UiState -- belongs to this thread, which is what makes a
  // command a plain function call rather than a message.
  auto execute = [&](const Command& command) {
    switch (command.kind) {
      case CommandKind::kNone:
        break;

      case CommandKind::kError:
        set_message(command.message, true);
        break;

      case CommandKind::kChopTransient:
      case CommandKind::kChopGrid: {
        if (sample == nullptr) {
          set_message("nothing loaded to chop", true);
          break;
        }
        const bool transient = command.kind == CommandKind::kChopTransient;
        // Analysis runs HERE, on the control thread, so the interface stops
        // redrawing until it finishes -- a few tens of milliseconds per minute
        // of audio. Putting it on the worker lane is M6's ingest job; doing it
        // now would mean building most of that lane to earn a progress bar
        // nobody can see yet.
        slices =
            transient ? ingest::chop_transient(*sample) : ingest::chop_grid(*sample, command.count);
        show_slices(slices, state);

        const std::string what = transient ? "chop transient" : "chop grid";
        if (slices.empty()) {
          set_message(what + ": no transients found", true);
          break;
        }
        const std::size_t published = apply_slices(engine, sample, slices, state);
        const std::size_t on_pads = std::min(slices.size(), static_cast<std::size_t>(rt::kNumPads));
        if (published < rt::kNumPads) {
          set_message(what + ": " + std::to_string(slices.size()) + " slices, but only " +
                          std::to_string(published) + " pads took it",
                      true);
          break;
        }
        set_message(what + ": " + std::to_string(slices.size()) + " slices on " +
                        std::to_string(on_pads) + " pads",
                    false);
        break;
      }

      case CommandKind::kChopReset: {
        slices = ingest::SliceSet{};
        show_slices(slices, state);
        if (sample == nullptr) {
          set_message("nothing loaded", true);
          break;
        }
        // Back to how the file arrived: the whole thing on pad 1, every other
        // pad empty. Expressed as a one-slice chop so there is a single code
        // path that publishes pads, then the NAME is put back -- pad 1 is the
        // file again, not slice one of it.
        ingest::SliceSet whole;
        whole.slices.push_back(ingest::Slice{.start_frame = 0, .end_frame = sample->num_frames()});
        static_cast<void>(apply_slices(engine, sample, whole, state));
        state.pads[kPad].has_slice = false;
        state.pads[kPad].name = options.sample_path.stem().string();
        set_message("chop reset", false);
        break;
      }

      case CommandKind::kSlotAssign: {
        if (slices.empty()) {
          set_message("nothing chopped yet — try :chop transient", true);
          break;
        }

        // A RANGE, always -- a single assignment is the one-element case, so
        // there is no second code path for it to disagree with. The parser has
        // already refused a reversed range and a length mismatch; what is left
        // is what only this thread can know, which is how many slices and pads
        // actually exist.
        const std::size_t count = command.slice_last - command.slice + 1;
        if (command.slice_last > slices.size()) {
          set_message("no slice " + std::to_string(command.slice_last) + " (have " +
                          std::to_string(slices.size()) + ")",
                      true);
          break;
        }
        if (command.pad + count - 1 > rt::kNumPads) {
          // Named as PADS, because pads are what ran out. The first phrasing
          // said "runs past pad 16" and was read as a limit on CHOPS -- which it
          // is not: there can be any number of slices, and only sixteen pads to
          // hold them at once. Banks are M6; until then the answer is a
          // different destination range, so the message names the one asked for.
          set_message("needs pads " + std::to_string(command.pad) + "-" +
                          std::to_string(command.pad + count - 1) + ", but there are only " +
                          std::to_string(rt::kNumPads) + " pads",
                      true);
          break;
        }

        // Published one pad at a time, and a refusal stops the run rather than
        // carrying on: the handoff ring being full means the rest would be
        // refused too, and reporting how far it got is more use than sixteen
        // identical failures.
        std::size_t done = 0;
        for (; done < count; ++done) {
          const ingest::Slice& slice = slices.slices[command.slice - 1 + done];
          const auto pad = static_cast<std::uint8_t>(command.pad - 1 + done);
          rt::PadConfig config{};
          config.sample = sample;
          config.pad = pad;
          config.start_frame = slice.start_frame;
          config.end_frame = slice.end_frame;
          if (!engine.publish_pad_config(
                  std::make_shared<const rt::PadConfig>(std::move(config)))) {
            break;
          }
          state.pads[pad].loaded = true;
          state.pads[pad].has_slice = true;
          state.pads[pad].slice_index = command.slice - 1 + done;
          state.pads[pad].name = slice_pad_name(command.slice + done);
        }

        if (done < count) {
          set_message("pads are busy — " + std::to_string(done) + " of " + std::to_string(count) +
                          " assigned, try again",
                      true);
          break;
        }
        if (count == 1) {
          set_message(
              "slice " + std::to_string(command.slice) + " → pad " + std::to_string(command.pad),
              false);
          break;
        }
        set_message("slices " + std::to_string(command.slice) + "-" +
                        std::to_string(command.slice_last) + " → pads " +
                        std::to_string(command.pad) + "-" + std::to_string(command.pad + count - 1),
                    false);
        break;
      }

      case CommandKind::kPadGate:
      case CommandKind::kPadOneShot: {
        const bool gate = command.kind == CommandKind::kPadGate;
        const rt::TriggerMode mode = gate ? rt::TriggerMode::kGate : rt::TriggerMode::kOneShot;
        if (command.pad > rt::kNumPads) {
          set_message("no pad " + std::to_string(command.pad) + " (have " +
                          std::to_string(rt::kNumPads) + ")",
                      true);
          break;
        }

        // Rebuilt from the CURRENT config rather than from scratch, so changing
        // the trigger mode does not quietly discard a slice range someone spent
        // a minute nudging. A pad with nothing on it is skipped: publishing a
        // gate config for an empty pad would be a change with no effect and one
        // more thing in the ring.
        const std::size_t first = command.pad > 0 ? command.pad - 1 : 0;
        const std::size_t last = command.pad > 0 ? command.pad : rt::kNumPads;
        std::size_t changed = 0;
        for (std::size_t pad = first; pad < last; ++pad) {
          const std::shared_ptr<const rt::PadConfig> current =
              engine.pad_config(static_cast<std::uint8_t>(pad));
          if (current == nullptr || current->sample == nullptr) {
            continue;
          }
          rt::PadConfig next = *current;
          next.trigger = mode;
          if (engine.publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(next)))) {
            ++changed;
          }
        }

        const std::string what = gate ? "gate" : "one-shot";
        if (changed == 0) {
          set_message(what + ": no pads loaded", true);
          break;
        }
        std::string said =
            what + ": " + std::to_string(changed) + " pad" + (changed == 1 ? "" : "s");
        // Said out loud rather than left to be discovered. A gate pad on a
        // terminal with no key release plays through to the end like a
        // one-shot, which is the right behaviour and a baffling one to meet
        // without warning.
        if (gate && !kitty_active) {
          said += " — but this terminal reports no key release, so they play through";
        }
        set_message(said, gate && !kitty_active);
        break;
      }

      case CommandKind::kEdit: {
        if (state.slices.empty()) {
          set_message("nothing chopped yet — try :chop transient", true);
          break;
        }
        if (command.slice > state.slices.size()) {
          set_message("no slice " + std::to_string(command.slice) + " (have " +
                          std::to_string(state.slices.size()) + ")",
                      true);
          break;
        }
        if (command.slice > 0) {
          state.edit.slice = command.slice - 1;
        }
        state.screen = Screen::kEdit;
        frame_slice(last_columns);
        break;
      }

      case CommandKind::kPerform:
        state.screen = Screen::kPerform;
        break;

      case CommandKind::kBpm:
        edit_sequencer([&](rt::SequencerState& next) { next.bpm_x100 = command.bpm_x100; },
                       "bpm " + format_bpm(command.bpm_x100));
        break;

      case CommandKind::kSwing: {
        const std::size_t pattern = lane_pattern();
        edit_sequencer(
            [&](rt::SequencerState& next) { next.patterns[pattern].swing = command.swing; },
            "swing " + std::to_string(command.swing) + "% on pattern " +
                std::to_string(pattern + 1));
        break;
      }

      case CommandKind::kPatternSelect: {
        const auto selected = static_cast<std::uint8_t>(command.pattern - 1);
        edit_sequencer([&](rt::SequencerState& next) { next.selected_pattern = selected; },
                       "pattern " + std::to_string(command.pattern));
        // AFTER the edit, not before: if the publish was refused the selection
        // did not move, and a cursor clamped to the pattern that was not
        // selected would be clamped to the wrong length.
        clamp_cursor();
        break;
      }

      case CommandKind::kPatternLength: {
        const std::size_t pattern = lane_pattern();
        const auto length = static_cast<std::uint8_t>(command.count);
        edit_sequencer([&](rt::SequencerState& next) { next.patterns[pattern].length = length; },
                       "pattern " + std::to_string(pattern + 1) + ": " +
                           std::to_string(command.count) + " steps");
        clamp_cursor();
        break;
      }

      case CommandKind::kPatternClear: {
        const std::size_t pattern = lane_pattern();
        // The STEPS only. Length and swing are how the pattern is set up rather
        // than what is written in it, and clearing them too would make `pattern
        // clear` undo work nobody asked it to touch.
        edit_sequencer([&](rt::SequencerState& next) { next.patterns[pattern].steps = {}; },
                       "pattern " + std::to_string(pattern + 1) + " cleared");
        break;
      }

      case CommandKind::kSong: {
        const std::size_t slots = std::min(command.song.size(), rt::kMaxSongSlots);
        std::string order;
        for (std::size_t slot = 0; slot < slots; ++slot) {
          order += (slot > 0 ? " " : "") + std::to_string(command.song[slot]);
        }
        edit_sequencer(
            [&](rt::SequencerState& next) {
              next.song = rt::Song{};  // built whole, so a shorter song leaves no tail behind
              for (std::size_t slot = 0; slot < slots; ++slot) {
                next.song.order[slot] = static_cast<std::uint8_t>(command.song[slot] - 1);
              }
              next.song.length = static_cast<std::uint8_t>(slots);
            },
            "song: " + order);
        break;
      }

      case CommandKind::kSongClear:
        edit_sequencer([](rt::SequencerState& next) { next.song = rt::Song{}; },
                       "song cleared — the selected pattern repeats");
        break;

      case CommandKind::kMetronome: {
        const bool on = command.toggle == Switch::kToggle ? !sequencer.metronome
                                                          : command.toggle == Switch::kOn;
        edit_sequencer([on](rt::SequencerState& next) { next.metronome = on; },
                       on ? "metronome on" : "metronome off");
        break;
      }

      case CommandKind::kStop:
        if (command.pad == 0) {
          panic();
          set_message("stopped everything", false);
          break;
        }
        static_cast<void>(engine.trigger_pad(rt::PadEvent{
            .pad = static_cast<std::uint8_t>(command.pad - 1), .kind = rt::PadEventKind::kStop}));
        set_message("stopped pad " + std::to_string(command.pad), false);
        break;

      case CommandKind::kMix:
        state.screen = Screen::kMix;
        state.mix.page = command.pattern == 1 ? MixPage::kBuses : MixPage::kChannelsLow;
        state.mix.cursor = 0;
        break;

      case CommandKind::kStripGain:
        if (command.bus_target) {
          const float linear = tui::detail::db_to_linear(command.decibels);
          const bool ok = engine.set_bus_gain(command.bus, linear);
          report_strip(ok,
                       "bus " + std::string(1, static_cast<char>('a' + command.bus)) + " " +
                           tui::detail::with_precision(static_cast<double>(command.decibels), 1) +
                           " dB");
          break;
        }
        report_strip(edit_strip(command.pad,
                                [&](rt::StripConfig& strip) {
                                  strip.gain = tui::detail::db_to_linear(command.decibels);
                                }),
                     "pad " + std::to_string(command.pad) + " gain " +
                         tui::detail::with_precision(static_cast<double>(command.decibels), 1) +
                         " dB");
        break;

      case CommandKind::kStripPan:
        report_strip(edit_strip(command.pad,
                                [&](rt::StripConfig& strip) {
                                  strip.balance = static_cast<float>(command.pan_percent) / 100.0F;
                                }),
                     "pad " + std::to_string(command.pad) + " pan " +
                         (command.pan_percent == 0 ? std::string{"centre"}
                                                   : std::to_string(command.pan_percent)));
        break;

      case CommandKind::kStripMute: {
        bool muted = false;
        const bool ok = edit_strip(command.pad, [&](rt::StripConfig& strip) {
          strip.mute =
              command.toggle == Switch::kToggle ? !strip.mute : command.toggle == Switch::kOn;
          muted = strip.mute;
        });
        report_strip(ok, "pad " + std::to_string(command.pad) + (muted ? " muted" : " unmuted"));
        break;
      }

      case CommandKind::kStripSolo: {
        bool soloed = false;
        const bool ok = edit_strip(command.pad, [&](rt::StripConfig& strip) {
          strip.solo =
              command.toggle == Switch::kToggle ? !strip.solo : command.toggle == Switch::kOn;
          soloed = strip.solo;
        });
        report_strip(ok, "pad " + std::to_string(command.pad) + (soloed ? " soloed" : " unsoloed"));
        break;
      }

      case CommandKind::kStripBus:
        report_strip(
            edit_strip(command.pad, [&](rt::StripConfig& strip) { strip.bus = command.bus; }),
            "pad " + std::to_string(command.pad) + " -> bus " +
                std::string(1, static_cast<char>('a' + command.bus)));
        break;

      case CommandKind::kStripEq: {
        const auto index = static_cast<std::size_t>(command.band - 1);
        const bool ok = edit_strip(command.pad, [&](rt::StripConfig& strip) {
          if (command.toggle == Switch::kOff) {
            strip.eq.bands[index].enabled = false;
            return;
          }
          // The TYPE stays with the band -- band 1 is the low shelf whatever it
          // is tuned to. Letting a verb change a band's type would mean four
          // bands that are only nominally in the fixed order docs/MIXER.md
          // specifies.
          strip.eq.bands[index] =
              rt::make_eq_band(strip.eq.bands[index].type, command.frequency, command.decibels,
                               command.shape, options.sample_rate);
        });
        report_strip(
            ok, command.toggle == Switch::kOff
                    ? "pad " + std::to_string(command.pad) + " eq " + std::to_string(command.band) +
                          " off"
                    : "pad " + std::to_string(command.pad) + " eq " + std::to_string(command.band) +
                          " " +
                          tui::detail::with_precision(static_cast<double>(command.frequency), 0) +
                          " Hz " +
                          tui::detail::with_precision(static_cast<double>(command.decibels), 1) +
                          " dB");
        break;
      }

      case CommandKind::kStripComp: {
        const bool ok = edit_strip(command.pad, [&](rt::StripConfig& strip) {
          if (command.toggle == Switch::kOff) {
            strip.compressor.enabled = false;
            return;
          }
          strip.compressor = rt::make_compressor(command.decibels, command.ratio, command.knee_db,
                                                 command.makeup_db, frames_of_ms(command.attack_ms),
                                                 frames_of_ms(command.release_ms));
        });
        report_strip(
            ok, command.toggle == Switch::kOff
                    ? "pad " + std::to_string(command.pad) + " comp off"
                    : "pad " + std::to_string(command.pad) + " comp " +
                          tui::detail::with_precision(static_cast<double>(command.decibels), 1) +
                          " dB " +
                          tui::detail::with_precision(static_cast<double>(command.ratio), 1) +
                          ":1");
        break;
      }

      case CommandKind::kLimiter: {
        const bool on = command.toggle == Switch::kToggle ? !engine.limiter().enabled
                                                          : command.toggle == Switch::kOn;
        rt::LimiterConfig limiter = on ? rt::make_limiter(command.decibels) : rt::LimiterConfig{};
        const bool ok = engine.set_limiter(limiter);
        report_strip(
            ok, on ? "limiter on at " +
                         tui::detail::with_precision(static_cast<double>(command.decibels), 1) +
                         " dB"
                   : "limiter off");
        break;
      }

      case CommandKind::kQuit:
        quit();
        break;
    }
  };

  auto frame = ftxui::Renderer([&] {
    const ftxui::Dimensions size = ftxui::Terminal::Size();
    const auto columns = static_cast<std::size_t>(std::max(size.dimx, 1));
    const auto rows = static_cast<std::size_t>(std::max(size.dimy, 1));
    last_columns = columns;

    const engine::Telemetry telemetry = engine.telemetry();
    state.playing = telemetry.playing;
    state.playhead_frame = telemetry.playhead_frame;
    state.playhead_pad = telemetry.playhead_pad;
    state.master_peak = telemetry.master_peak;
    for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
      state.pads[pad].level = telemetry.pad_peak[pad];
      state.pads[pad].triggered = telemetry.pad_glow[pad].triggered;
      state.pads[pad].glow_seconds = telemetry.pad_glow[pad].seconds_since_trigger;
      state.pads[pad].glow_velocity = telemetry.pad_glow[pad].velocity;
      state.pads[pad].glow_sequenced = telemetry.pad_glow[pad].sequenced;
    }
    // THE MIXER'S VIEW, refreshed only when MIX is up.
    //
    // Sixteen strips of EQ response is a few hundred evaluations of a transfer
    // function, and PERFORM does not draw any of it. Building it every frame
    // regardless would be paying for a screen nobody is looking at -- the same
    // reason the waveform is only summarised for the view that is showing.
    if (state.screen == Screen::kMix) {
      state.mix.any_solo = false;
      for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
        const rt::StripConfig strip = engine.strip(static_cast<std::uint8_t>(pad));
        StripView& view = state.mix.strips[pad];
        view.name = state.pads[pad].name;
        view.gain_db = tui::detail::linear_to_db(strip.gain);
        view.balance = strip.balance;
        view.peak = telemetry.strip_peak[pad];
        view.reduction = telemetry.strip_reduction[pad];
        view.mute = strip.mute;
        view.solo = strip.solo;
        view.bus = strip.bus;
        state.mix.any_solo = state.mix.any_solo || strip.solo;

        view.eq_label = eq_label_of(strip.eq);
        view.comp_label = comp_label_of(strip.compressor);
        view.eq_curve = eq_response(strip.eq, options.sample_rate);
      }

      for (std::size_t bus = 0; bus < rt::kNumBuses; ++bus) {
        StripView& view = state.mix.buses[bus];
        view.name = "bus";
        view.gain_db = tui::detail::linear_to_db(engine.bus_gain(static_cast<std::uint8_t>(bus)));
        view.peak = telemetry.bus_peak[bus];
        view.has_balance = false;
        view.has_bus = false;
        view.routed = 0;
        for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
          view.routed += engine.strip(static_cast<std::uint8_t>(pad)).bus == bus ? 1 : 0;
        }
      }

      state.mix.master.name = "master";
      state.mix.master.gain_db = 0.0F;
      state.mix.master.peak = telemetry.master_peak;
      state.mix.master.has_balance = false;
      state.mix.master.has_bus = false;
      state.mix.limiter_enabled = engine.limiter().enabled;
      state.mix.limiter_gain = telemetry.limiter_gain;
    }

    state.active_voices = engine.active_voices();
    state.xruns = device.xrun_count();
    state.dropped =
        engine.dropped_events() + engine.dropped_triggers() + engine.dropped_midi_events();

    // The pattern lane, copied flat out of what the control thread published.
    //
    // The STEP and the PATTERN come from telemetry rather than being recomputed
    // here: the audio thread is the only place that knows both the transport
    // position and the tempo those frames were actually rendered at, so a UI
    // that derived the step itself would disagree after every tempo change.
    // The grid comes from the published state, which is what this thread owns.
    state.pattern.transport_running = telemetry.transport_playing;
    state.pattern.step = telemetry.transport_step;
    state.pattern.slot = telemetry.transport_slot;
    state.pattern.cursor_step = step_cursor;
    // Drawn from THIS THREAD'S copy rather than read back out of the engine.
    //
    // They are the same object whenever there is a stream, because a published
    // edit is what moves the local copy. Without one there is nothing to publish
    // to (see edit_sequencer above), so reading it back would draw a sequencer
    // frozen at whatever was published before the ring filled -- which is how
    // this was found.
    {
      const auto index = static_cast<std::uint8_t>(lane_pattern());
      const rt::Pattern& source = sequencer.patterns[index];

      state.pattern.has_pattern = true;
      state.pattern.pattern = index;
      state.pattern.length = rt::pattern_length(source);
      state.pattern.swing = source.swing;
      state.pattern.bpm_x100 = sequencer.bpm_x100;
      state.pattern.metronome = sequencer.metronome;
      state.pattern.song = rt::song_slots(sequencer.song) > 0;

      // The marker is drawn only when the transport is on the pattern being
      // shown. With a song running that is often some other pattern, and a
      // marker moving across a grid the audio is not playing would be a
      // confident wrong answer.
      state.pattern.playhead = telemetry.transport_playing && telemetry.transport_pattern == index;

      for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
        for (std::size_t step = 0; step < rt::kMaxSteps; ++step) {
          state.pattern.rows[pad].on[step] = source.steps[step][pad].on;
        }
      }
    }

    // Re-summarised every frame against the CURRENT width, so a resize is a
    // correct redraw rather than a stretched one. At any zoom this reads about
    // five pyramid bins per column, which is what makes it affordable at 30 Hz
    // on a five-minute file.
    if (sample != nullptr && !pyramid.empty()) {
      // Each screen has its own view and its own panel width -- EDIT spends six
      // columns on the amplitude gutter -- so which one is up decides both the
      // span summarised and the number of bins to summarise it into. Using one
      // pair for both would stretch whichever screen lost.
      const bool editing = state.screen == Screen::kEdit;
      const WaveView& view = editing ? state.edit.view : state.view;
      const std::size_t wave_columns =
          editing ? edit_wave_columns_for(columns) : wave_columns_for(columns);
      state.bins.assign(bins_for_columns(wave_columns), ingest::PeakBin{});
      pyramid.summarize(*sample, 0, view.first_frame, view.frames_visible, state.bins);
    }

    return render(state, columns, rows);
  });

  // THE BINDINGS, WRITTEN ONCE.
  //
  // Both input paths -- FTXUI's own events and the CSI-u decoder -- turn a
  // keystroke into a KeyEvent and end up here. That is the whole reason
  // tui::KeyEvent exists: with the Kitty protocol enabled, Event::Character
  // never fires again, so a second copy of the bindings would be a second copy
  // that drifts.
  //
  // Bindings compare key.character(), not key.key. The protocol reports the
  // UNSHIFTED key with the typed text alongside; FTXUI reports the text as the
  // key. character() is the one value both paths agree on.
  auto handle_key = [&](const KeyEvent& key) -> bool {
    const std::uint32_t code = key.character();
    const bool press = key.action == KeyAction::kPress;
    const bool typing = press || key.action == KeyAction::kRepeat;

    // A message is cleared by the next PRESS. Not by a release: in Kitty mode
    // the release of the Return that ran a command arrives a moment after it,
    // and clearing on that would make every answer flash and vanish.
    if (press) {
      state.message.clear();
      state.message_is_error = false;
    }

    // The prompt swallows EVERYTHING while it is up, pad keys included. There
    // is a `c` in `:chop`, and a prompt that plays a drum as you type it is not
    // a prompt.
    if (state.command_active) {
      if (!typing) {
        return true;
      }
      if (code == kEscape) {
        state.command_active = false;
        state.command_text.clear();
        return true;
      }
      if (code == kReturn) {
        const Command command = parse_command(state.command_text);
        // Closed before running, not after: `:q` exits from inside execute(),
        // and leaving the prompt up until it returns would draw one last frame
        // with a stale `:` line in it.
        state.command_active = false;
        state.command_text.clear();
        execute(command);
        return true;
      }
      if (code == kBackspace) {
        pop_codepoint(state.command_text);
        return true;
      }
      // Anything that is not a printable character -- arrows, function keys,
      // Tab -- is eaten rather than passed through. Nothing below should act on
      // a keystroke aimed at the prompt.
      if (const std::string typed = printable(code); !typed.empty()) {
        state.command_text += typed;
      }
      return true;
    }

    // MIX HAS ITS OWN KEYMAP TOO, for the same reason EDIT does: `s` is pad 10
    // and `d` is pad 11, so a mixer that bound them to solo and something else
    // would make two strips unreachable exactly while you were setting their
    // levels. The pads are off here and SPACE is the transport -- which is the
    // right thing on this screen anyway, since mixing is done while the pattern
    // runs rather than by poking individual pads.
    if (state.screen == Screen::kMix) {
      if (!typing) {
        return true;
      }
      if (code == kEscape) {
        state.screen = Screen::kPerform;
        return true;
      }
      if (code == ':') {
        state.command_active = true;
        state.command_text.clear();
        return true;
      }

      // How many strips this page holds -- the cursor is an index into the PAGE,
      // not into the pads, so it cannot point past the four buses.
      const std::size_t page_size =
          state.mix.page == MixPage::kBuses ? rt::kNumBuses : std::size_t{8};

      // Which pad the cursor is on, or kNumPads on the bus page. Every key below
      // that edits a strip goes through this, so none of them can act on a pad
      // number the screen is not showing.
      const auto cursor_pad = [&]() -> std::size_t {
        if (state.mix.page == MixPage::kBuses) {
          return rt::kNumPads;
        }
        const std::size_t first = state.mix.page == MixPage::kChannelsHigh ? 8 : 0;
        return first + state.mix.cursor;
      };

      switch (code) {
        case '[':
        case ']': {
          // PAGING IS ON `[` AND `]`, NOT ON TAB, and that is a measurement
          // rather than a preference. Tab never reaches this handler: FTXUI
          // consumes it before CatchEvent runs, which a probe confirmed by
          // reporting the code of every other key and nothing for Tab. PERFORM's
          // own Tab binding -- the sample/pattern panel switch, in this file
          // since M2 -- is dead for the same reason and was never noticed
          // because the PTY session only presses it conditionally.
          //
          // `[` and `]` also read better here: EDIT already uses them to step
          // through slices, and they are bidirectional where Tab only cycles one
          // way through three pages.
          //
          // The cursor resets rather than being carried: strip 6 of a channel
          // page has no counterpart among four buses, and leaving it at 6 would
          // put it off the end.
          const bool forward = code == ']';
          state.mix.page = state.mix.page == MixPage::kChannelsLow
                               ? (forward ? MixPage::kChannelsHigh : MixPage::kBuses)
                           : state.mix.page == MixPage::kChannelsHigh
                               ? (forward ? MixPage::kBuses : MixPage::kChannelsLow)
                               : (forward ? MixPage::kChannelsLow : MixPage::kChannelsHigh);
          state.mix.cursor = 0;
          return true;
        }

        case 'h':
          // Clamped, not wrapped -- the same rule EDIT's `[` and `]` follow.
          if (state.mix.cursor > 0) {
            state.mix.cursor--;
          }
          return true;

        case 'l':
          if (state.mix.cursor + 1 < page_size) {
            state.mix.cursor++;
          }
          return true;

        case 'k':
        case 'j': {
          const float step = code == 'k' ? 1.0F : -1.0F;
          if (state.mix.page == MixPage::kBuses) {
            const auto bus = static_cast<std::uint8_t>(state.mix.cursor);
            const float now = tui::detail::linear_to_db(engine.bus_gain(bus));
            const float next = std::clamp(now + step, -60.0F, 12.0F);
            report_strip(engine.set_bus_gain(bus, tui::detail::db_to_linear(next)),
                         "bus " + std::string(1, static_cast<char>('a' + bus)) + " " +
                             tui::detail::with_precision(static_cast<double>(next), 1) + " dB");
            return true;
          }
          const std::size_t pad = cursor_pad();
          float shown = 0.0F;
          const bool ok = edit_strip(pad + 1, [&](rt::StripConfig& strip) {
            shown = std::clamp(tui::detail::linear_to_db(strip.gain) + step, -60.0F, 12.0F);
            strip.gain = tui::detail::db_to_linear(shown);
          });
          report_strip(ok, "pad " + std::to_string(pad + 1) + " gain " +
                               tui::detail::with_precision(static_cast<double>(shown), 1) + " dB");
          return true;
        }

        case 'm': {
          if (state.mix.page == MixPage::kBuses) {
            set_message("a bus has no mute — mute the strips feeding it", true);
            return true;
          }
          const std::size_t pad = cursor_pad();
          bool muted = false;
          const bool ok = edit_strip(pad + 1, [&](rt::StripConfig& strip) {
            strip.mute = !strip.mute;
            muted = strip.mute;
          });
          report_strip(ok, "pad " + std::to_string(pad + 1) + (muted ? " muted" : " unmuted"));
          return true;
        }

        case 's': {
          if (state.mix.page == MixPage::kBuses) {
            set_message("a bus has no solo — solo the strips feeding it", true);
            return true;
          }
          const std::size_t pad = cursor_pad();
          bool soloed = false;
          const bool ok = edit_strip(pad + 1, [&](rt::StripConfig& strip) {
            strip.solo = !strip.solo;
            soloed = strip.solo;
          });
          report_strip(ok, "pad " + std::to_string(pad + 1) + (soloed ? " soloed" : " unsoloed"));
          return true;
        }

        case 'b': {
          if (state.mix.page == MixPage::kBuses) {
            return true;  // a bus does not route to a bus
          }
          const std::size_t pad = cursor_pad();
          std::uint8_t bus = 0;
          const bool ok = edit_strip(pad + 1, [&](rt::StripConfig& strip) {
            strip.bus = static_cast<std::uint8_t>((strip.bus + 1) % rt::kNumBuses);
            bus = strip.bus;
          });
          report_strip(ok, "pad " + std::to_string(pad + 1) + " -> bus " +
                               std::string(1, static_cast<char>('a' + bus)));
          return true;
        }

        default:
          break;
      }
      return true;
    }

    // EDIT HAS ITS OWN KEYMAP, and the QWERTY pad map is not part of it.
    //
    // It cannot be: `z` is pad 9 and the mockup binds `z` to zoom, `h` and `l`
    // are pads-adjacent view keys in PERFORM and boundary nudges here. Rather
    // than contort one map to fit both screens, the pads are off in EDIT and
    // SPACE auditions the slice being edited -- which is the one thing the pads
    // were wanted for here, since you nudge a boundary and then listen to it.
    if (state.screen == Screen::kEdit) {
      if (!typing) {
        return true;
      }
      if (code == kSpace) {
        const std::uint8_t pad = pad_for_slice(state, state.edit.slice);
        if (pad >= rt::kNumPads) {
          // A SLICE NOT ON A PAD CANNOT BE HEARD, because every route to sound
          // here is a pad trigger: the audio thread plays m_pads[N], so there is
          // nothing to address a slice that no pad holds. A chop of more than
          // sixteen leaves the rest exactly there -- editable, drawable, and
          // silent.
          //
          // Said rather than ignored. The key did nothing at all before, which
          // reads as EDIT being broken on those slices rather than as the slice
          // being unreachable. Auditioning without a pad is M5.5's, alongside
          // the browser that needs the same mechanism to preview a file it has
          // not loaded.
          set_message("slice " + std::to_string(state.edit.slice + 1) +
                          " is not on a pad — :slot assign " +
                          std::to_string(state.edit.slice + 1) + " <pad> to hear it",
                      true);
          return true;
        }
        static_cast<void>(
            engine.trigger_pad(rt::PadEvent{.pad = pad, .velocity = 1.0F, .frame_offset = 0}));
        state.selected_pad = pad;
        return true;
      }
      if (code == kEscape) {
        // Back to PERFORM, NOT out of the program. Escape means "leave the thing
        // I am in", and in PERFORM the thing you are in is cratedig.
        state.screen = Screen::kPerform;
        return true;
      }
      if (code == ':') {
        state.command_active = true;
        state.command_text.clear();
        return true;
      }
      if (state.slices.empty()) {
        return true;  // nothing below means anything without a slice
      }

      switch (code) {
        case '[':
          // Clamped, not wrapped. Wrapping from slice 1 to slice 16 while
          // holding the key down would be a jump you did not ask for, in the
          // middle of an edit.
          if (state.edit.slice > 0) {
            --state.edit.slice;
          }
          frame_slice(last_columns);
          break;
        case ']':
          if (state.edit.slice + 1 < state.slices.size()) {
            ++state.edit.slice;
          }
          frame_slice(last_columns);
          break;
        case 'h':
          nudge(false, -static_cast<std::ptrdiff_t>(kNudgeFrames), last_columns);
          break;
        case 'l':
          nudge(false, static_cast<std::ptrdiff_t>(kNudgeFrames), last_columns);
          break;
        case 'H':
          nudge(true, -static_cast<std::ptrdiff_t>(kNudgeFrames), last_columns);
          break;
        case 'L':
          nudge(true, static_cast<std::ptrdiff_t>(kNudgeFrames), last_columns);
          break;
        case 'u':
          undo_edit(last_columns);
          break;
        case 'z': {
          // One key, one direction, wrapping back to the whole slice when there
          // is no more to see -- which is what a bare `z zoom` in the mockup's
          // mode line has to mean.
          const std::size_t before = state.edit.view.frames_visible;
          state.edit.view.zoom_by(kEditZoomStep, total_frames);
          if (state.edit.view.frames_visible == before) {
            frame_slice(last_columns);
          } else {
            refresh_edit(last_columns);
          }
          break;
        }
        case kKeyArrowLeft:
        case kKeyArrowRight: {
          const auto step = static_cast<std::ptrdiff_t>(
              std::max<std::size_t>(state.edit.view.frames_visible / kScrollDivisor, 1));
          state.edit.view.scroll_by(code == kKeyArrowLeft ? -step : step, total_frames);
          refresh_edit(last_columns);
          break;
        }
        default:
          return false;
      }
      return true;
    }

    // The pad map, before the view keys: `s` and `d` and `f` are pads, and a
    // player hitting them expects a sound rather than a scroll. The view keys
    // that survive are the ones the map does not claim -- except `f`, which the
    // map does claim, so `fit` moved to `0`. The number row is claimed too, as
    // of M4.5: `1234` are pads 1-4 rather than 13-16, which is where they sit on
    // the keyboard relative to the rest of the grid.
    //
    // SPACE IS NOT A PAD, as of M4.5. It was pad 1 from M1, on the argument that
    // the biggest key on the keyboard should do something -- and the something
    // it should do turns out to be the transport, which is what it does in every
    // other machine that has one. Pad 1 is still two keys away, on `1` and `q`.
    if (const std::uint8_t index = pad_for_key(code); index < rt::kNumPads) {
      if (key.action == KeyAction::kRelease) {
        // Addressed to the pad, not to a voice: a one-shot ignores it and a
        // gate pad lets go. Which is why this can be sent unconditionally.
        static_cast<void>(engine.trigger_pad(
            rt::PadEvent{.pad = index, .kind = rt::PadEventKind::kNoteOff, .velocity = 0.0F}));
        return true;
      }
      if (!press) {
        // AUTO-REPEAT IS NOT A HIT. Holding a pad down must sustain it, not
        // machine-gun it at the terminal's repeat rate -- and in one-shot mode
        // retriggering would steal a voice from itself thirty times a second.
        return true;
      }
      static_cast<void>(
          engine.trigger_pad(rt::PadEvent{.pad = index, .velocity = 1.0F, .frame_offset = 0}));
      state.selected_pad = index;
      return true;
    }

    if (!typing) {
      return true;  // nothing below here has a meaning for a release
    }

    if (code == ':') {
      state.command_active = true;
      state.command_text.clear();
      return true;
    }
    // ESCAPE, not `q`. The pad map claims `q`, and a sampler where a pad quits
    // instead of making a sound would be a strange thing to ship. `:q` works
    // too, for the muscle memory that expects it.
    if (code == kEscape) {
      quit();
      return true;
    }
    // The right-hand panel, on BACKSLASH as well as on Tab.
    //
    // Tab has been the binding since M2 and has never worked: FTXUI consumes it
    // before CatchEvent runs, so it does not reach this function at all. Found
    // in M5 while giving MIX a paging key -- a probe that reported the code of
    // every unbound key showed `z` and `n` arriving and nothing for Tab. The
    // PTY session only ever pressed Tab inside an `if`, so the assertion after
    // it had been passing without exercising the key.
    //
    // Tab is KEPT rather than deleted: it costs nothing, it is what the mockup's
    // caption row says, and it will start working on its own the day the Kitty
    // protocol is negotiated (where Tab arrives as a CSI-u sequence this
    // program decodes itself -- see keys.cpp). Backslash is the one that works
    // today, on every terminal.
    if (code == kTab || code == '\\') {
      state.tab = state.tab == PanelTab::kSample ? PanelTab::kPattern : PanelTab::kSample;
      return true;
    }

    // THE TRANSPORT, on space and on `p`.
    //
    // Space is the binding; `p` is an alias, and stays one because M6 wants `p`
    // for a pad in the right-hand mirror of the grid. Naming space as the
    // primary is what makes that possible without taking the transport with it.
    //
    // PRESS ONLY, not repeat: holding it would start and stop the transport at
    // the terminal's repeat rate, which is the same reason a held pad does not
    // retrigger.
    if ((code == kSpace || code == 'p') && press) {
      transport_asked = !transport_asked;
      set_transport(transport_asked);
      // Said out loud rather than left to be discovered, exactly as `pad gate`
      // says it on a terminal with no key release. With no device nothing calls
      // render(), so the sequencer never advances and the transport genuinely
      // does nothing -- a key that silently does nothing reads as broken.
      if (!audio_running) {
        set_message("transport: no audio device, so nothing renders and nothing runs", true);
      }
      return true;
    }

    // THE PANIC. `.` reads as a full stop, is under the right hand, and is one
    // of the few keys the pad map does not claim on either screen.
    //
    // Press only, like the transport: holding it would re-panic at the repeat
    // rate, which is harmless and pointless.
    if (code == '.' && press) {
      panic();
      set_message("stopped everything", false);
      return true;
    }

    // The step keys BRING THE LANE UP rather than acting invisibly. Toggling a
    // step you cannot see is how you end up with a pattern you did not write,
    // and the cursor only exists on the lane -- so pressing one of these is
    // taken as asking to see it.
    if (code == '[' || code == ']' || code == 't') {
      state.tab = PanelTab::kPattern;
      clamp_cursor();
      const std::size_t length = rt::pattern_length(sequencer.patterns[lane_pattern()]);

      if (code == '[') {
        // Clamped, not wrapped, for the reason EDIT's slice keys are: a held
        // key that jumps from step 1 to step 16 is a jump nobody asked for.
        step_cursor = step_cursor > 0 ? step_cursor - 1 : 0;
        return true;
      }
      if (code == ']') {
        step_cursor = step_cursor + 1 < length ? step_cursor + 1 : length - 1;
        return true;
      }

      // `[` and `]` repeat happily; `t` does NOT. A held toggle would flip the
      // step at the terminal's repeat rate and leave it in whichever state the
      // key happened to be released in, which is a coin toss rather than an
      // edit.
      if (!press) {
        return true;
      }

      const std::size_t pattern = lane_pattern();
      const std::size_t step = step_cursor;
      const std::size_t pad = state.selected_pad;
      const bool on = !sequencer.patterns[pattern].steps[step][pad].on;
      // No message: the lane shows the result, and a line of text for every
      // toggle would spend the mode line on something already on screen.
      edit_sequencer(
          [&](rt::SequencerState& next) {
            rt::Step& cell = next.patterns[pattern].steps[step][pad];
            cell.on = on;
            // Velocity is left alone, so turning a step off and on again keeps
            // whatever it was set to. Nothing writes it yet -- M4 records from
            // MIDI, not into the grid -- but zeroing it here would be a silent
            // loss the moment something does.
          },
          std::string{});
      return true;
    }
    // Enter opens EDIT on the selected pad's slice. `:edit N` reaches any slice
    // by number; this is the version that needs no number, because the pad you
    // just hit is almost always the one you want to look at.
    if (code == kReturn) {
      if (!state.pads[state.selected_pad].has_slice) {
        set_message("pad " + std::to_string(state.selected_pad + 1) + " has no slice to edit",
                    true);
        return true;
      }
      state.edit.slice = state.pads[state.selected_pad].slice_index;
      state.screen = Screen::kEdit;
      frame_slice(last_columns);
      return true;
    }

    // Everything below moves the view, which only means something with a sample
    // loaded.
    if (total_frames == 0) {
      return false;
    }
    const auto step = static_cast<std::ptrdiff_t>(
        std::max<std::size_t>(state.view.frames_visible / kScrollDivisor, 1));

    switch (code) {
      case 'h':
      case kKeyArrowLeft:
        state.view.scroll_by(-step, total_frames);
        break;
      case 'l':
      case kKeyArrowRight:
        state.view.scroll_by(step, total_frames);
        break;
      case 'H':
        state.view.scroll_by(-static_cast<std::ptrdiff_t>(state.view.frames_visible), total_frames);
        break;
      case 'L':
        state.view.scroll_by(static_cast<std::ptrdiff_t>(state.view.frames_visible), total_frames);
        break;
      case '+':
      case '=':
        state.view.zoom_by(kZoomStep, total_frames);
        break;
      case '-':
      case '_':
        state.view.zoom_by(1.0 / kZoomStep, total_frames);
        break;
      case '0':
        // `0` rather than `f`, which pad 8 now owns. Zero reads as "show
        // everything" and is the one digit the 4x4 map does not claim.
        state.view.fit(total_frames);
        break;
      case 'g':
        state.view.first_frame = 0;
        state.view.clamp(total_frames);
        break;
      case 'G':
        state.view.first_frame = total_frames;
        state.view.clamp(total_frames);
        break;
      default:
        return false;
    }
    return true;
  };

  auto root = ftxui::CatchEvent(frame, [&](const ftxui::Event& event) {
    // The janitor tick. The engine spawns no threads of its own, so somebody has
    // to call collect_garbage(); doing it on the frame tick keeps the whole
    // design single-writer and costs nothing when there is nothing to collect.
    if (event == ftxui::Event::Custom) {
      // With no device nothing calls render(), so nothing adopts what has been
      // published and the handoff rings fill until every edit is refused. Four
      // commands were enough: `:chop` publishes sixteen configs and
      // `:slot assign 1-8 1` publishes eight. Safe here because in that mode
      // there is no audio thread to race with -- see Engine::adopt_offline().
      if (!audio_running) {
        engine.adopt_offline();
      }
      static_cast<void>(engine.collect_garbage());

      // The capability query goes out on the first tick rather than before
      // Loop(), because by now FTXUI has switched to the ALTERNATE SCREEN --
      // and the protocol's flag stack is per-screen. Pushing flags onto the
      // main screen's stack would change the state of the shell we came from,
      // and leaving it changed is exactly what must not happen.
      if (!kitty_asked && !options.legacy_keys) {
        kitty_asked = true;
        write_terminal(kKittyQuery);
      }
      return false;  // let the loop redraw
    }

    // The capability reply, before anything else looks at this event. A
    // terminal that does not implement the protocol never sends one, so there
    // is nothing to time out on and nothing is ever enabled on a guess.
    if (kitty_asked && !kitty_active && parse_kitty_flags(event.input()).has_value()) {
      kitty_active = true;
      write_terminal(kKittyPush);
      return true;
    }

    if (kitty_active) {
      // Every keystroke now arrives as CSI-u, including the ones FTXUI would
      // otherwise have turned into Event::Character. Anything the decoder does
      // not recognise is not something we bind, so it is dropped rather than
      // handed to a parser that no longer sees the whole keyboard.
      if (const std::optional<KeyEvent> key = parse_key(event.input()); key.has_value()) {
        return handle_key(*key);
      }
      return false;
    }

    if (const std::optional<KeyEvent> key = legacy_key(event); key.has_value()) {
      return handle_key(*key);
    }
    return false;
  });

  // The refresh thread exists because Loop() is blocking and the counters change
  // without any key being pressed. Declared after `screen` so it is joined
  // before `screen` is destroyed.
  //
  // Post(), NOT PostEvent(). FTXUI's own documentation recommends
  // PostEvent(Event::Custom) from exactly this kind of thread, and in 7.0.1
  // that is a data race: PostEvent goes to MultiReceiverBuffer::Push, which
  // does an unsynchronised values_.push_back() and next_index_++ on a
  // std::deque shared with the main loop. Post() takes the same Event through
  // TaskRunner::PostTask, whose queue is mutex-guarded. TSan found this on the
  // Linux run of the PTY session test; it is a real race, not an artifact of
  // FTXUI being built without instrumentation.
  std::atomic<bool> ticking{true};
  std::thread refresh{[&] {
    while (ticking.load(std::memory_order_relaxed)) {
      screen.Post(ftxui::Event::Custom);
      std::this_thread::sleep_for(kFrameInterval);
    }
  }};

  screen.Loop(root);

  ticking.store(false, std::memory_order_relaxed);
  refresh.join();

  // 5. Stop before collecting: once the stream is stopped nothing else can
  //    retire, so this last sweep is guaranteed to drain the ring.
  if (audio_running) {
    device.stop();
    device.close();
  }
  static_cast<void>(engine.collect_garbage());

  if (device.xrun_count() > 0) {
    std::cout << device.xrun_count() << " xrun(s) during this session\n";
  }
  return 0;
}

}  // namespace tui
