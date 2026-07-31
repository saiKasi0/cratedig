#include "tui/app.hpp"

#include "engine/engine.hpp"
#include "ingest/decoder.hpp"
#include "ingest/peak_pyramid.hpp"
#include "ingest/slices.hpp"
#include "io/audio_device.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/sample.hpp"
#include "tui/command.hpp"
#include "tui/keys.hpp"
#include "tui/render.hpp"
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

// The map the mockups print in the caption row, in grid order.
//
// The mockups contradict themselves about the bottom row: the PERFORM caption
// says `qwer asdf zxcv 1234`, while the onboarding screen says number keys set
// velocity. The caption row wins -- it is on the screen being built, and it is
// what the pad grid's own legend already tells the player. Velocity arrives
// with MIDI in M4, where it comes from a controller that actually has it.
constexpr std::array<char, rt::kNumPads> kPadKeys{'q', 'w', 'e', 'r', 'a', 's', 'd', 'f',
                                                  'z', 'x', 'c', 'v', '1', '2', '3', '4'};

// The keys that are not characters, named. Their codes are the legacy control
// bytes, which is what the Kitty protocol reports for them and what FTXUI's own
// events are built from -- so one set of names covers both paths.
constexpr std::uint32_t kTab = 9;
constexpr std::uint32_t kReturn = 13;
constexpr std::uint32_t kEscape = 27;
constexpr std::uint32_t kSpace = 32;
constexpr std::uint32_t kBackspace = 127;

// Which pad a keystroke plays, or -1. Space is pad 1 alongside `q`.
[[nodiscard]] int pad_for_code(std::uint32_t code) noexcept {
  if (code == kSpace) {
    return kPad;
  }
  if (code > 0x7F) {
    return -1;
  }
  for (std::size_t index = 0; index < kPadKeys.size(); ++index) {
    if (static_cast<std::uint32_t>(kPadKeys[index]) == code) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

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
    state.slices.push_back(
        SliceMark{.start_frame = slice.start_frame, .end_frame = slice.end_frame});
  }
  state.chop_algorithm = set.algorithm == ingest::ChopAlgorithm::kTransient
                             ? std::string{"transient"}
                             : "grid " + std::to_string(set.size());
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
        if (command.slice > slices.size()) {
          set_message("no slice " + std::to_string(command.slice) + " (have " +
                          std::to_string(slices.size()) + ")",
                      true);
          break;
        }
        if (command.pad > rt::kNumPads) {
          set_message("no pad " + std::to_string(command.pad) + " (have " +
                          std::to_string(rt::kNumPads) + ")",
                      true);
          break;
        }
        const ingest::Slice& slice = slices.slices[command.slice - 1];
        const auto pad = static_cast<std::uint8_t>(command.pad - 1);
        rt::PadConfig config{};
        config.sample = sample;
        config.pad = pad;
        config.start_frame = slice.start_frame;
        config.end_frame = slice.end_frame;
        if (!engine.publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(config)))) {
          set_message("pad " + std::to_string(command.pad) + " is busy, try again", true);
          break;
        }
        state.pads[pad].loaded = true;
        state.pads[pad].has_slice = true;
        state.pads[pad].slice_index = command.slice - 1;
        state.pads[pad].name = slice_pad_name(command.slice);
        set_message(
            "slice " + std::to_string(command.slice) + " → pad " + std::to_string(command.pad),
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

      case CommandKind::kQuit:
        quit();
        break;
    }
  };

  auto frame = ftxui::Renderer([&] {
    const ftxui::Dimensions size = ftxui::Terminal::Size();
    const auto columns = static_cast<std::size_t>(std::max(size.dimx, 1));
    const auto rows = static_cast<std::size_t>(std::max(size.dimy, 1));

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
    }
    state.active_voices = engine.active_voices();
    state.xruns = device.xrun_count();
    state.dropped = engine.dropped_events() + engine.dropped_triggers();

    // Re-summarised every frame against the CURRENT width, so a resize is a
    // correct redraw rather than a stretched one. At any zoom this reads about
    // five pyramid bins per column, which is what makes it affordable at 30 Hz
    // on a five-minute file.
    if (sample != nullptr && !pyramid.empty()) {
      const std::size_t wave_columns = wave_columns_for(columns);
      state.bins.assign(bins_for_columns(wave_columns), ingest::PeakBin{});
      pyramid.summarize(*sample, 0, state.view.first_frame, state.view.frames_visible, state.bins);
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

    // The pad map, before the view keys: `s` and `d` and `f` are pads, and a
    // player hitting them expects a sound rather than a scroll. The view keys
    // that survive are the ones the map does not claim -- except `f`, which the
    // map does claim, so `fit` moved to `0`.
    //
    // Space is pad 1 as well. It is what M1 and M2 documented, it is what the
    // mode line has always said, and a sampler where the biggest key on the
    // keyboard does nothing would be a strange thing to ship.
    if (const int pad = pad_for_code(code); pad >= 0) {
      const auto index = static_cast<std::uint8_t>(pad);
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
    // ESCAPE, not `q`. The QWERTY pad map claims `q` for pad 1, and a sampler
    // where the top-left pad quits instead of making a sound would be a strange
    // thing to ship. `:q` works too, for the muscle memory that expects it.
    if (code == kEscape) {
      quit();
      return true;
    }
    if (code == kTab) {
      state.tab = state.tab == PanelTab::kSample ? PanelTab::kPattern : PanelTab::kSample;
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
