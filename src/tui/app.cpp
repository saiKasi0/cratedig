#include "tui/app.hpp"

#include "engine/engine.hpp"
#include "engine/take.hpp"
#include "ingest/decoder.hpp"
#include "ingest/peak_pyramid.hpp"
#include "ingest/slices.hpp"
#include "ingest/tempo.hpp"
#include "io/audio_device.hpp"
#include "rt/pad_config.hpp"
#include "rt/pad_event.hpp"
#include "rt/pitch.hpp"
#include "rt/sample.hpp"
#include "rt/sequencer.hpp"
#include "tui/command.hpp"
#include "tui/completion.hpp"
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
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace tui {
namespace {

// Above this a detected tempo is stated plainly; below it, with a question mark.
//
// Measured rather than picked: a clean synthetic loop scores 0.96-0.99, and
// eighteen seconds of real rock -- guitar over drums -- scores 0.34. Half is
// between the two and on the honest side of it.
constexpr float kTempoSureEnough = 0.6F;

// What the browser lists.
//
// FILTERED, and the list is deliberately broad rather than exact. FFmpeg decodes
// far more than this and `:load` will happily try anything, so the filter is
// about keeping a music directory readable -- not about what the decoder can
// manage. A file it refuses is reported when the load fails, which is the honest
// place to find out.
[[nodiscard]] bool looks_like_audio(const std::filesystem::path& path) {
  static constexpr std::array<std::string_view, 10> kExtensions{
      ".wav", ".flac", ".mp3", ".aif", ".aiff", ".ogg", ".opus", ".m4a", ".aac", ".wv"};
  std::string extension = path.extension().string();
  for (char& letter : extension) {
    letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
  }
  return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
}

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

// How much of what came BEFORE a threshold crossing a capture keeps.
//
// 250 ms, which is a quarter of the buffer rt::Recorder is given, so asking for
// it can never be the thing that limits it. It exists because a threshold
// cannot begin before the threshold: without pre-roll every triggered take
// opens on a step from silence to the trigger level, which is a click.
constexpr std::size_t kCapturePreRollMs = 250;

// ~30 Hz. The design is dense but calm (docs/design/DESIGN_BRIEF.md) and nothing
// on screen changes faster than the eye reads it, so a higher rate would only
// spend CPU that the audio thread would rather have.
constexpr auto kFrameInterval = std::chrono::milliseconds(33);

constexpr std::uint8_t kPad = 0;

// The keys that are not characters, named. Their codes are the legacy control
// bytes, which is what the Kitty protocol reports for them and what FTXUI's own
// events are built from -- so one set of names covers both paths.
constexpr std::uint32_t kTab = 9;

// Shift-Tab has no character; FTXUI reports it as its own event and this is the
// code the rest of the program knows it by. Above the functional-key floor for
// the reason every other synthetic code is: printable() must never emit it.
constexpr std::uint32_t kTabReverse = kFirstFunctionalKey + 16;
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
  if (event == ftxui::Event::TabReverse) {
    return KeyEvent{.key = kTabReverse};
  }
  if (event == ftxui::Event::ArrowLeft) {
    return KeyEvent{.key = kKeyArrowLeft};
  }
  if (event == ftxui::Event::ArrowRight) {
    return KeyEvent{.key = kKeyArrowRight};
  }
  // UP AND DOWN WERE MISSING UNTIL M5.5 T7, which is why every arrow binding on
  // a vertical list -- BROWSE's listing, MIX's strips, the completion menu --
  // did nothing on a terminal that does not speak the Kitty protocol. The
  // horizontal pair had been here since M2 and the vertical pair simply was not,
  // so `j`/`k` worked and the arrows beside them in the hint line did not.
  // Measured with a probe that reports the code of every key: left and right
  // arrived, up and down produced nothing at all.
  if (event == ftxui::Event::ArrowUp) {
    return KeyEvent{.key = kKeyArrowUp};
  }
  if (event == ftxui::Event::ArrowDown) {
    return KeyEvent{.key = kKeyArrowDown};
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
// `file` is the pool entry the slices belong to: a pad records WHICH FILE as
// well as which slice, because with a crate a slice index alone no longer names
// a sound. `set` is passed separately so `:chop reset` can publish a one-slice
// set without first writing it into the entry.
[[nodiscard]] std::size_t apply_slices(engine::Engine& engine, const ingest::PoolEntry& file,
                                       const ingest::SliceSet& set, UiState& state) {
  std::size_t published = 0;
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    const bool has_slice = pad < set.size() && file.sample != nullptr;

    // Built fresh and published whole. A PadConfig is immutable once the audio
    // thread can see it, so "clear this pad" is a new config with no sample --
    // which the engine treats as silent rather than as an error.
    rt::PadConfig config{};
    config.pad = static_cast<std::uint8_t>(pad);
    if (has_slice) {
      config.sample = file.sample;
      config.start_frame = set.slices[pad].start_frame;
      config.end_frame = set.slices[pad].end_frame;
    }

    if (!engine.publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(config)))) {
      return published;
    }
    ++published;

    state.pads[pad].loaded = has_slice;
    state.pads[pad].has_slice = has_slice;
    state.pads[pad].file = has_slice ? file.id : ingest::kNoFile;
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

// The pad that plays a given slice OF A GIVEN FILE, or rt::kNumPads.
//
// The file half is not decoration. With a crate, slice 3 of the break and slice
// 3 of the vocal are different sounds; a lookup on the index alone would answer
// "pad 3 plays that" about whichever file happened to be assigned there, and
// EDIT would then audition the wrong material while showing the right waveform.
//
// UNASSERTED AS OF THIS COMMIT, and said so rather than assumed: deleting the
// file comparison passes the whole suite, because nothing can load a second file
// yet -- the CLI puts exactly one in the pool. The :load verb makes it reachable
// and is where the test belongs.
[[nodiscard]] std::uint8_t pad_for_slice(const UiState& state, ingest::FileId file,
                                         std::size_t slice) noexcept {
  if (file == ingest::kNoFile) {
    return rt::kNumPads;
  }
  for (std::size_t pad = 0; pad < rt::kNumPads; ++pad) {
    if (state.pads[pad].has_slice && state.pads[pad].file == file &&
        state.pads[pad].slice_index == slice) {
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
  // THE CRATE. Until M5.5 these were three locals -- one Sample, one name, one
  // pyramid -- and that was the one-file assumption: a session held exactly one
  // file for its whole life and every pad was a slice of it.
  //
  // The pool holds as many as are loaded, and `current` says which one the wave
  // panel and EDIT are looking at. A PAD IS NOT LIMITED TO `current`: each pad's
  // rt::PadConfig carries its own shared_ptr, so pad 3 can be playing a slice of
  // a file that is not on screen and pad 4 a slice of another.
  ingest::SamplePool pool;
  ingest::FileId current = ingest::kNoFile;

  // The entry being looked at, or nullptr when the crate is empty. A function
  // rather than a reference because the pool's storage moves when it grows --
  // holding a PoolEntry& across a `:load` would be holding a dangling one.
  const auto entry = [&pool, &current]() -> ingest::PoolEntry* { return pool.find(current); };
  const auto current_sample = [&entry]() -> std::shared_ptr<const rt::Sample> {
    ingest::PoolEntry* found = entry();
    return found == nullptr ? nullptr : found->sample;
  };

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
    // Built here, on the control thread, before anything real-time exists.
    // ~100-200 ms for a five-minute file; moving it onto a worker so the
    // interface can come up first is M6's ingest job, not M2's.
    ingest::PeakPyramid pyramid = ingest::PeakPyramid::build(*load.sample);
    current = pool.add(load.sample, options.sample_path, ingest::SliceSet{}, std::move(pyramid));
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
  if (current_sample() != nullptr && !engine.set_pad_sample(kPad, current_sample())) {
    // Only reachable if the handoff ring is full, which cannot happen on the
    // very first publish. Checked anyway rather than discarded: a pad that
    // silently failed to load would present as "the spacebar does nothing".
    std::cerr << "error: could not assign " << options.sample_path.filename().string()
              << " to pad 1\n";
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

  // What the pattern was before the current take, so `rec undo` can put it back.
  //
  // A WHOLE STATE rather than a list of the steps written, because a take does
  // more than turn steps on: overdubbing changes the velocity of a step that was
  // already there, and replace throws the pattern away outright. Neither is
  // undoable from a record of what was added. ~16 KB, copied once per take
  // rather than once per note.
  std::optional<rt::SequencerState> take_undo;

  // Live hits drained from the engine this tick. A member rather than a local so
  // the allocation happens once instead of thirty times a second.
  std::vector<rt::PadHit> live_hits;

  // Audio capture, control side.
  //
  // WHAT TO DO WHEN THE TAKE FINISHES, rather than doing it at the moment the
  // verb is typed. Stopping is a message: the audio thread applies it at the top
  // of a block and only then hands back the last chunk, so `capture stop` cannot
  // build a sample there and then. It records an intention, and the frame tick
  // acts on it once Engine::take_complete() says everything has arrived.
  enum class CaptureFinish : std::uint8_t { kNone, kKeep, kDrop };
  CaptureFinish capture_finish = CaptureFinish::kNone;
  rt::RecordSource capture_source = rt::RecordSource::kMaster;

  // Numbered rather than named after a clock. The pool keys on path, so two
  // takes need two distinct ones -- and "take 3" is what a person would call it
  // anyway.
  std::size_t takes_made = 0;

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
    // HOW MANY CHANNELS THE INPUT ACTUALLY HAS, capped at stereo, rather than a
    // fixed two. A laptop microphone is mono, and asking a mono device for two
    // channels fails at open() with "the device does not support the requested
    // channel count" -- which is a confusing way to be told the machine has a
    // built-in mic. Measured on exactly that machine.
    std::uint16_t input_channels = 0;
    if (options.want_input) {
      const std::vector<io::DeviceInfo> inputs = device.input_devices();
      const io::DeviceInfo* chosen = nullptr;
      for (const io::DeviceInfo& info : inputs) {
        const bool wanted = options.input_device_id == 0 ? info.is_default_input
                                                         : info.id == options.input_device_id;
        if (wanted) {
          chosen = &info;
          break;
        }
      }
      if (chosen == nullptr && options.input_device_id == 0 && !inputs.empty()) {
        chosen = &inputs.front();  // no device claims to be the default
      }
      if (chosen == nullptr) {
        std::cerr << "error: " << io::describe(io::DeviceError::kNoInputDeviceAvailable) << '\n';
        std::cerr << "try: cratedig --list-devices, or drop --input\n";
        return 1;
      }
      input_channels = static_cast<std::uint16_t>(std::min(2U, chosen->input_channels));
    }

    const io::AudioDevice::Config device_config{.sample_rate = options.sample_rate,
                                                .num_channels = 2,
                                                .block_frames = options.block_frames,
                                                .device_id = options.device_id,
                                                .input_channels = input_channels,
                                                .input_device_id = options.input_device_id};
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

  const std::size_t total_frames = current_sample() != nullptr ? current_sample()->num_frames() : 0;

  UiState state;
  state.version = CRATEDIG_VERSION;
  state.engine_rate = options.sample_rate;
  state.max_voices = engine::Engine::kMaxVoices;
  if (const ingest::PoolEntry* loaded = entry(); loaded != nullptr) {
    state.current_file = loaded->id;
    state.sample_name = loaded->name;
    state.sample_rate = loaded->sample->sample_rate();
    state.sample_channels = loaded->sample->num_channels();
    state.sample_frames = total_frames;
    state.pads[kPad] = PadState{.name = options.sample_path.stem().string(),
                                .level = 0.0F,
                                .loaded = true,
                                .file = loaded->id};
    state.view.fit(total_frames);
  }
  if (!options.no_audio) {
    state.audio_api = device.api_name();
    state.block_frames = device.actual_block_frames();
  }

  // The chops live in the pool entry now -- each file carries its own, which is
  // the other half of the assumption M5.5 removes: `:chop` used to mean "the
  // chop" and means "this file's chop". Every site reaches them through
  // `entry()`, so there is no second copy to keep in step.

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
  // Returns whether the edit actually landed. Ignored by everything that only
  // wants to say something afterwards; the take recorder needs it, because a
  // counter that went up on an edit the audio thread refused would be reporting
  // notes that are not in the pattern.
  auto edit_sequencer = [&](const auto& mutate, std::string said) -> bool {
    rt::SequencerState next = sequencer;
    mutate(next);

    // Published unconditionally, including with no device open. Nothing drains
    // the handoff rings without a stream, so this used to fill them and then
    // refuse every later edit for the rest of the session -- the frame tick now
    // calls Engine::adopt_offline() in that mode, which is the one place that
    // has to know, rather than every publisher guarding itself.
    if (!engine.publish_sequencer(std::make_shared<const rt::SequencerState>(next))) {
      set_message("sequencer busy — the edit did not happen, try again", true);
      return false;
    }
    sequencer = next;
    if (!said.empty()) {
      set_message(std::move(said), false);
    }
    return true;
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

  // Arm or disarm the take.
  //
  // ARMING IS WHAT TAKES THE UNDO SNAPSHOT, not the first note. The moment the
  // pattern becomes worth keeping a copy of is the moment somebody says they are
  // about to play over it -- waiting for the first hit would mean a `replace`
  // that cleared the pattern and was then played into silence had already thrown
  // it away with nothing recorded to justify it.
  auto set_armed = [&](bool on) {
    if (on == state.take.armed) {
      set_message(on ? "already recording" : "not recording", false);
      return;
    }

    if (!on) {
      state.take.armed = false;
      set_message("recording off — " + std::to_string(state.take.recorded) +
                      (state.take.recorded == 1 ? " note kept" : " notes kept"),
                  false);
      return;
    }

    take_undo = sequencer;
    state.take.recorded = 0;

    // NOT UNDOABLE YET, and the distinction matters. The snapshot is taken at
    // arming because that is the last moment the old pattern exists, but there
    // is nothing to put back until the take has actually changed something --
    // and "take undone, the pattern is back as it was" after a take that
    // recorded nothing is a confirmation of work that never happened. Found by
    // the PTY session, which armed, disarmed, and was told it had undone a take.
    //
    // `replace` sets it below, because clearing the pattern IS the change.
    state.take.can_undo = false;

    if (state.take.replace) {
      // Cleared HERE rather than on the first note, so that what you hear on the
      // next pass is what you are playing into rather than the old pattern
      // waiting to be overwritten a note at a time.
      const std::size_t pattern = lane_pattern();
      if (!edit_sequencer(
              [pattern](rt::SequencerState& next) { next.patterns[pattern].steps = {}; }, "")) {
        // The clear did not land, so neither does the arm. Recording into a
        // pattern that was supposed to be empty and is not is the one outcome
        // `replace` must never produce.
        take_undo.reset();
        return;
      }
      state.take.can_undo = true;
    }

    state.take.armed = true;

    // The transport is what gives a hit a position; without it nothing is kept
    // and nothing says why. Named at the moment of arming rather than left to be
    // discovered after a bar of playing that went nowhere.
    if (!transport_asked) {
      set_message("recording armed — press space to roll", false);
      return;
    }
    set_message(
        state.take.replace
            ? "recording — pattern " + std::to_string(lane_pattern() + 1) + " cleared, playing in"
            : "recording — playing in over pattern " + std::to_string(lane_pattern() + 1),
        false);
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
    state.edit.undo_depth = undo.size();
    if (current_sample() == nullptr || state.edit.slice >= state.slices.size()) {
      return;
    }

    // One tick per two columns is about where a ruler stops being one. Past
    // that, ingest::zero_crossings_in returns nothing and the row goes blank --
    // which is the honest picture of "zoomed too far out to see crossings", and
    // the row fills in again as you zoom toward sample resolution.
    const std::size_t limit = std::max<std::size_t>(edit_wave_columns_for(columns) / 2, 1);
    state.edit.zero_crossings = ingest::zero_crossings_in(
        *current_sample(), state.edit.view.first_frame, state.edit.view.frames_visible, limit);
    if (state.edit.zero_crossings.size() == limit) {
      state.edit.zero_crossings.clear();
    }

    const std::uint8_t pad = pad_for_slice(state, state.current_file, state.edit.slice);
    if (pad >= rt::kNumPads) {
      return;
    }
    const std::shared_ptr<const rt::PadConfig> config = engine.pad_config(pad);
    if (config == nullptr) {
      return;
    }
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
    const std::uint8_t pad = pad_for_slice(state, state.current_file, index);
    ingest::PoolEntry* file = entry();
    if (pad >= rt::kNumPads || file == nullptr || index >= file->slices.size()) {
      return;
    }
    const std::shared_ptr<const rt::PadConfig> current = engine.pad_config(pad);
    rt::PadConfig next = current != nullptr ? *current : rt::PadConfig{};
    next.pad = pad;
    next.sample = file->sample;
    next.start_frame = file->slices.slices[index].start_frame;
    next.end_frame = file->slices.slices[index].end_frame;
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
    ingest::PoolEntry* file = entry();
    if (file == nullptr || state.edit.slice >= file->slices.size()) {
      return;
    }
    ingest::Slice& slice = file->slices.slices[state.edit.slice];
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

    show_slices(file->slices, state);
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
    ingest::PoolEntry* file = entry();
    if (file != nullptr && last.index < file->slices.size()) {
      file->slices.slices[last.index].start_frame = last.start_frame;
      file->slices.slices[last.index].end_frame = last.end_frame;
      show_slices(file->slices, state);
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

  // Points the interface at whatever `current` now names.
  //
  // One place, because "which file am I looking at" changes for three different
  // reasons -- a load, a switch, an unload -- and three copies of this would be
  // three chances to leave the wave panel drawing one file and EDIT editing
  // another.
  auto show_current = [&](std::size_t columns) {
    const ingest::PoolEntry* file = entry();
    state.current_file = current;
    if (file == nullptr) {
      state.sample_name.clear();
      state.sample_rate = 0;
      state.sample_channels = 0;
      state.sample_frames = 0;
      state.slices.clear();
      state.bins.clear();
      state.view = WaveView{};
      state.edit.slice = 0;
      return;
    }
    state.sample_name = file->name;
    state.sample_rate = file->sample->sample_rate();
    state.sample_channels = file->sample->num_channels();
    state.sample_frames = file->sample->num_frames();

    // The view is reset to the whole file rather than carried across. Two files
    // rarely share a length, and a window from a five-minute break landing on a
    // one-bar loop would open on nothing at all.
    state.view.fit(file->sample->num_frames());
    show_slices(file->slices, state);
    refresh_edit(columns);
  };

  // Reads a directory into the browser's listing.
  //
  // ON THE CONTROL THREAD, which is where I/O belongs -- the renderer never
  // touches the filesystem, so render() stays a pure function of UiState and
  // every snapshot test can build a listing by hand.
  //
  // Directories first, then files, each sorted by name: a listing whose order
  // depended on the filesystem's would be a different screen on every machine,
  // and the snapshots would be untestable.
  auto read_directory = [&](const std::filesystem::path& where) {
    BrowserState& browser = state.browser;
    browser.path = where.string();
    browser.entries.clear();
    browser.note.clear();
    browser.cursor = 0;
    browser.first_visible = 0;

    std::error_code failed;
    const std::filesystem::directory_iterator listing{where, failed};
    if (failed) {
      // Said rather than shown as empty: an unreadable directory and an empty
      // one look identical, and only one of them is a mistake to fix.
      browser.note = "cannot read " + where.filename().string() + " — " + failed.message();
      return;
    }

    std::vector<BrowserEntry> directories;
    std::vector<BrowserEntry> files;
    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator{where, failed}) {
      std::error_code ignored;
      const std::string name = item.path().filename().string();
      if (name.empty() || name.front() == '.') {
        continue;  // dotfiles are noise in a music directory
      }
      if (item.is_directory(ignored)) {
        directories.push_back(BrowserEntry{.name = name, .is_directory = true});
        continue;
      }
      if (!looks_like_audio(item.path())) {
        continue;
      }
      files.push_back(BrowserEntry{
          .name = name,
          .is_directory = false,
          .bytes = static_cast<std::uint64_t>(std::filesystem::file_size(item.path(), ignored)),
          .loaded = pool.find_path(item.path()) != ingest::kNoFile,
      });
    }

    const auto by_name = [](const BrowserEntry& left, const BrowserEntry& right) {
      return left.name < right.name;
    };
    std::sort(directories.begin(), directories.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);

    // `..` first, always, and only when there is somewhere to go. A browser you
    // cannot climb out of is one you have to restart to escape.
    if (where.has_parent_path() && where.parent_path() != where) {
      browser.entries.push_back(BrowserEntry{.name = "..", .is_directory = true});
    }
    browser.entries.insert(browser.entries.end(), directories.begin(), directories.end());
    browser.entries.insert(browser.entries.end(), files.begin(), files.end());

    if (browser.entries.empty()) {
      browser.note = "no audio files here";
    }
  };

  // Puts the cursor on a named entry after a re-read, or leaves it at the top.
  //
  // WHY RE-READING NEEDS THIS AT ALL: read_directory() resets the cursor, which
  // is right when you have arrived somewhere new and wrong in the two cases that
  // are not new. Loading a file re-reads so the `loaded` marker updates, and
  // without this you would be thrown back to the top of the listing every time
  // -- which is precisely while you are working down a directory loading
  // several. Climbing out with `h` is the other: landing on `..` rather than on
  // the directory you just left makes `l` then `h` a round trip that does not
  // come back.
  auto focus_entry = [&state](std::string_view name) {
    BrowserState& browser = state.browser;
    for (std::size_t index = 0; index < browser.entries.size(); ++index) {
      if (browser.entries[index].name == name) {
        browser.cursor = index;
        return;
      }
    }
  };

  // Runs a parsed command. Everything it touches -- the engine's handoff ring,
  // the slice set, the UiState -- belongs to this thread, which is what makes a
  // command a plain function call rather than a message.
  // The expensive half of onset detection, kept while CHOP is open.
  //
  // Recomputed only when the low cut moves, because that is the only adjustable
  // that reaches the FFT. Measured on three minutes of stereo: analysing is
  // 82.8 ms and picking is 3.9 ms, so re-analysing per keystroke would put a
  // tenth of a second between a key and the screen on ordinary material.
  ingest::OnsetAnalysis chop_analysis;

  // What the last pick produced, at analysis resolution. Summarised to the
  // terminal's width in the renderer, where the width is known -- the same split
  // the BROWSE preview uses, and the reason UiState carries pictures rather than
  // the data they are drawn from.
  ingest::OnsetResult chop_result;

  // The parameters CHOP is showing, as OnsetParams.
  auto chop_params = [&state]() {
    ingest::OnsetParams params;
    params.threshold_lambda = state.chop.lambda;

    // THE FLOOR MOVES WITH IT, and that is what makes this a control at all.
    //
    // The threshold is `delta + lambda * median(flux)`. On sparse material --
    // hits with silence between them, which is most of what anyone chops -- the
    // local median is near zero, so the whole threshold IS delta and lambda
    // scales nothing. Measured on a twelve-hit fixture: sweeping lambda from 1.3
    // to 2.2 gave 24 slices at every step.
    //
    // So the field drives both, in proportion. At the detector's own default of
    // 1.6 this is exactly its own default delta, so nothing about `:chop
    // transient` moves; away from it, the control works on dense and sparse
    // material alike.
    constexpr float kDefaultLambda = 1.6F;
    constexpr float kDefaultDelta = 0.04F;
    params.threshold_delta = kDefaultDelta * (state.chop.lambda / kDefaultLambda);

    params.min_gap_seconds = state.chop.gap_seconds;
    params.hf_emphasis = state.chop.low_cut;
    return params;
  };

  // Re-picks. Cheap by construction -- see OnsetAnalysis.
  auto repick_chop = [&]() {
    chop_result = ingest::pick_onsets(chop_analysis, chop_params());
    state.chop.boundaries = chop_result.frames;
  };

  // "pad 3 · 1.50x · +7.02 st", the two units the same number goes by.
  auto pitch_summary = [](std::size_t pad_number, float ratio) {
    return "pad " + std::to_string(pad_number) + " · " +
           detail::with_precision(static_cast<double>(ratio), 2) + "x · " +
           (rt::semitones_from_ratio(ratio) >= 0.0F ? "+" : "") +
           detail::with_precision(static_cast<double>(rt::semitones_from_ratio(ratio)), 2) + " st";
  };

  auto execute = [&](const Command& command) {
    switch (command.kind) {
      case CommandKind::kNone:
        break;

      case CommandKind::kError:
        set_message(command.message, true);
        break;

      case CommandKind::kChopTune: {
        ingest::PoolEntry* file = entry();
        if (file == nullptr) {
          set_message("nothing loaded to chop", true);
          break;
        }

        // The analysis runs ONCE, here, on the control thread. It is the
        // expensive half and the screen's whole reason for existing is that
        // nothing after it is.
        state.chop.lambda = 1.6F;
        state.chop.gap_seconds = 0.030;
        state.chop.low_cut = 0.0F;
        state.chop.field = ChopState::Field::kSensitivity;
        state.chop.frames = file->sample->num_frames();
        state.chop.rate = file->sample->sample_rate();
        state.chop.name = file->name;
        state.chop.needs_analysis = false;

        chop_analysis = ingest::analyse_onsets(*file->sample, chop_params());
        repick_chop();

        state.screen = Screen::kChop;
        set_message("tuning the chop — enter applies it, esc leaves it alone", false);
        break;
      }

      case CommandKind::kChopTransient:
      case CommandKind::kChopGrid: {
        ingest::PoolEntry* file = entry();
        if (file == nullptr) {
          set_message("nothing loaded to chop", true);
          break;
        }
        const bool transient = command.kind == CommandKind::kChopTransient;
        // Analysis runs HERE, on the control thread, so the interface stops
        // redrawing until it finishes -- a few tens of milliseconds per minute
        // of audio. Putting it on the worker lane is M6's ingest job; doing it
        // now would mean building most of that lane to earn a progress bar
        // nobody can see yet.
        // The DENSITY, resolved against the session tempo. `beat` and `bar` are
        // defined in beats, so the tempo on the transport is what turns them
        // into a minimum gap -- and when M5.7's tempo detection lands it feeds
        // this rather than replacing it.
        file->slices = transient ? ingest::chop_transient(
                                       *file->sample,
                                       ingest::params_for(command.density, state.pattern.bpm_x100))
                                 : ingest::chop_grid(*file->sample, command.count);
        show_slices(file->slices, state);

        // The density is named only when it is not the default. `:chop transient`
        // answering "chop transient strum" would be quoting back a word nobody
        // typed, and the finest cut is what the verb has always meant.
        const std::string what =
            transient ? (command.density == ingest::ChopDensity::kStrum
                             ? std::string{"chop transient"}
                             : "chop transient " + std::string{ingest::name_of(command.density)})
                      : std::string{"chop grid"};
        const std::size_t count = file->slices.size();
        if (count == 0) {
          set_message(what + ": no transients found", true);
          break;
        }
        const std::size_t published = apply_slices(engine, *file, file->slices, state);
        const std::size_t on_pads = std::min(count, static_cast<std::size_t>(rt::kNumPads));
        if (published < rt::kNumPads) {
          set_message(what + ": " + std::to_string(count) + " slices, but only " +
                          std::to_string(published) + " pads took it",
                      true);
          break;
        }
        set_message(
            what + ": " + std::to_string(count) + " slices on " + std::to_string(on_pads) + " pads",
            false);
        break;
      }

      case CommandKind::kChopReset: {
        ingest::PoolEntry* file = entry();
        if (file == nullptr) {
          set_message("nothing loaded", true);
          break;
        }
        file->slices = ingest::SliceSet{};
        show_slices(file->slices, state);
        // Back to how the file arrived: the whole thing on pad 1, every other
        // pad empty. Expressed as a one-slice chop so there is a single code
        // path that publishes pads, then the NAME is put back -- pad 1 is the
        // file again, not slice one of it.
        ingest::SliceSet whole;
        whole.slices.push_back(
            ingest::Slice{.start_frame = 0, .end_frame = file->sample->num_frames()});
        static_cast<void>(apply_slices(engine, *file, whole, state));
        state.pads[kPad].has_slice = false;
        state.pads[kPad].name = options.sample_path.stem().string();
        set_message("chop reset", false);
        break;
      }

      case CommandKind::kSlotAssign: {
        const ingest::PoolEntry* file = entry();
        if (file == nullptr || file->slices.empty()) {
          set_message("nothing chopped yet — try :chop transient", true);
          break;
        }

        // A RANGE, always -- a single assignment is the one-element case, so
        // there is no second code path for it to disagree with. The parser has
        // already refused a reversed range and a length mismatch; what is left
        // is what only this thread can know, which is how many slices and pads
        // actually exist.
        const std::size_t count = command.slice_last - command.slice + 1;
        if (command.slice_last > file->slices.size()) {
          set_message("no slice " + std::to_string(command.slice_last) + " (have " +
                          std::to_string(file->slices.size()) + ")",
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
          const ingest::Slice& slice = file->slices.slices[command.slice - 1 + done];
          const auto pad = static_cast<std::uint8_t>(command.pad - 1 + done);
          rt::PadConfig config{};
          config.sample = file->sample;
          config.pad = pad;
          config.start_frame = slice.start_frame;
          config.end_frame = slice.end_frame;
          if (!engine.publish_pad_config(
                  std::make_shared<const rt::PadConfig>(std::move(config)))) {
            break;
          }
          state.pads[pad].loaded = true;
          state.pads[pad].has_slice = true;
          state.pads[pad].file = file->id;
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
          const std::shared_ptr<const rt::PadConfig> held =
              engine.pad_config(static_cast<std::uint8_t>(pad));
          if (held == nullptr || held->sample == nullptr) {
            continue;
          }
          rt::PadConfig next = *held;
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

      case CommandKind::kBpmDetect: {
        const ingest::PoolEntry* file = entry();
        if (file == nullptr) {
          set_message("nothing loaded to read a tempo from", true);
          break;
        }
        const ingest::TempoEstimate estimate =
            ingest::detect_tempo(ingest::analyse_onsets(*file->sample));
        if (!estimate.found()) {
          set_message(file->name + ": no tempo in it — too short, or nothing periodic", true);
          break;
        }

        // SAID WITH A QUALIFIER WHEN IT IS A GUESS. The confidence is a
        // normalised autocorrelation, so a clean loop scores near 1 and real
        // music with a guitar over it scores a third of that. Reporting both the
        // same way would make the honest case look like the sure one.
        const bool sure = estimate.confidence >= kTempoSureEnough;
        edit_sequencer([&](rt::SequencerState& next) { next.bpm_x100 = estimate.bpm_x100; },
                       file->name + ": " + format_bpm(estimate.bpm_x100) + " bpm" +
                           (sure ? "" : "?  (a rough read — double or halve it if it is wrong)"));
        break;
      }

      case CommandKind::kTapeSpeed: {
        // TAPE SPEED SCALES THE TEMPO and says so. See parse_tape() for why it
        // is not a resampler.
        const std::uint32_t now = state.pattern.bpm_x100;
        const auto scaled = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(now) * static_cast<double>(command.decibels)));
        const std::uint32_t next_bpm = std::clamp(scaled, rt::kMinBpmX100, rt::kMaxBpmX100);
        if (next_bpm != scaled) {
          set_message("tape: " + format_bpm(scaled) + " bpm is outside " +
                          format_bpm(rt::kMinBpmX100) + " to " + format_bpm(rt::kMaxBpmX100),
                      true);
          break;
        }
        edit_sequencer([&](rt::SequencerState& next) { next.bpm_x100 = next_bpm; },
                       "tape " + detail::with_precision(static_cast<double>(command.decibels), 2) +
                           "x — " + format_bpm(next_bpm) + " bpm (the pattern, not the pitch)");
        break;
      }

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

      case CommandKind::kRecordArm: {
        const bool on =
            command.toggle == Switch::kToggle ? !state.take.armed : command.toggle == Switch::kOn;
        set_armed(on);
        break;
      }

      case CommandKind::kRecordQuantise: {
        // The parser already refused anything that does not divide a bar, so
        // this cannot come back zero -- but it is checked rather than assumed,
        // because a zero here would be a quantiser dividing by it.
        const auto steps =
            engine::quantise_from_denominator(static_cast<std::uint8_t>(command.count));
        if (steps == 0) {
          set_message("rec quant: " + std::to_string(command.count) + " does not divide a bar",
                      true);
          break;
        }
        state.take.quantise_steps = steps;
        set_message("recording snaps to 1/" + std::to_string(command.count), false);
        break;
      }

      case CommandKind::kRecordReplace: {
        const bool on =
            command.toggle == Switch::kToggle ? !state.take.replace : command.toggle == Switch::kOn;
        state.take.replace = on;
        // Named for what happens NEXT TIME, because changing the mode mid-take
        // does not retroactively clear anything -- the clear happens at arming.
        set_message(on ? "replace — arming clears the pattern first"
                       : "overdub — a take adds to the pattern",
                    false);
        break;
      }

      case CommandKind::kRecordUndo: {
        if (!state.take.can_undo || !take_undo.has_value()) {
          set_message("nothing to put back — no take has been recorded", true);
          break;
        }
        // Disarmed by the undo, deliberately. Putting the pattern back while
        // still recording would start overwriting it again on the next bar,
        // which is not what anybody means by undo.
        state.take.armed = false;
        const rt::SequencerState before = *take_undo;
        if (edit_sequencer([&before](rt::SequencerState& next) { next = before; },
                           "take undone — the pattern is back as it was")) {
          take_undo.reset();
          state.take.can_undo = false;
          state.take.recorded = 0;
        }
        break;
      }

      case CommandKind::kCaptureSource: {
        if (engine.record_state() != rt::RecordState::kIdle) {
          set_message("capture source: stop the take first", true);
          break;
        }
        // REFUSED RATHER THAN ACCEPTED AND SILENT. With no capture side on the
        // stream, `source input` would set a mode that records nothing at all,
        // and the only symptom would be an empty take with no explanation. The
        // input is opt-in (see AppOptions::want_input), so the fix is a restart
        // and the message says so.
        if (command.text == "input" && device.input_channels() == 0) {
          set_message(audio_running
                          ? "capture source: this stream has no input — restart with --input"
                          : "capture source: no audio device, so there is no input",
                      true);
          break;
        }
        capture_source =
            command.text == "input" ? rt::RecordSource::kInput : rt::RecordSource::kMaster;
        state.capture.master = capture_source == rt::RecordSource::kMaster;
        set_message(state.capture.master
                        ? "capture records the master — what you hear, mixer and all"
                        : "capture records the input",
                    false);
        break;
      }

      case CommandKind::kCaptureArm: {
        const float threshold = tui::detail::db_to_linear(command.decibels);
        const std::size_t preroll =
            (static_cast<std::size_t>(engine.config().sample_rate) * kCapturePreRollMs) / 1000;
        if (!engine.arm_recording(capture_source, threshold, preroll)) {
          set_message("capture: there is a take waiting — :capture drop it first", true);
          break;
        }
        capture_finish = CaptureFinish::kNone;
        set_message("capture armed at " +
                        detail::with_precision(static_cast<double>(command.decibels), 1) +
                        " dB — it starts itself",
                    false);
        break;
      }

      case CommandKind::kCaptureStart: {
        const bool running = engine.record_state() != rt::RecordState::kIdle;
        const bool start =
            command.toggle == Switch::kToggle ? !running : command.toggle == Switch::kOn;
        if (start) {
          if (!engine.start_recording(capture_source)) {
            set_message("capture: there is a take waiting — :capture drop it first", true);
            break;
          }
          capture_finish = CaptureFinish::kNone;
          if (!audio_running) {
            set_message("capture: no audio device, so nothing renders and nothing is captured",
                        true);
            break;
          }
          set_message(state.capture.master ? "capturing the master" : "capturing the input", false);
          break;
        }

        if (!running) {
          set_message("nothing is being captured", false);
          break;
        }
        // The take is not finished HERE. See CaptureFinish.
        static_cast<void>(engine.stop_recording());
        capture_finish = CaptureFinish::kKeep;
        break;
      }

      case CommandKind::kCaptureDrop:
        if (engine.record_state() != rt::RecordState::kIdle) {
          static_cast<void>(engine.stop_recording());
        }
        capture_finish = CaptureFinish::kDrop;
        break;

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

      case CommandKind::kLoadFile: {
        const std::filesystem::path path{command.text};

        // Already in the crate: bring it up rather than decoding a second copy.
        // The pool would return the same id anyway; saying so is the difference
        // between "nothing happened" and "that one is already here".
        if (const ingest::FileId known = pool.find_path(path); known != ingest::kNoFile) {
          current = known;
          show_current(last_columns);
          set_message(path.filename().string() + " is already loaded", false);
          break;
        }

        // Decode on this thread, which is where it belongs and where it blocks:
        // about 4 s for a five-minute file, single-digit milliseconds for a
        // loop. The worker lane stays in M6 -- docs/ROADMAP.md records the
        // measurement rather than the intuition.
        const ingest::SampleLoad load = ingest::load_sample(path, options.sample_rate);
        if (!load.ok()) {
          std::string said = "load: ";
          said += ingest::describe(load.error);
          if (!load.detail.empty()) {
            said += " (" + load.detail + ")";
          }
          set_message(said, true);
          break;
        }

        ingest::PeakPyramid built = ingest::PeakPyramid::build(*load.sample);
        const ingest::FileId id = pool.add(load.sample, path, ingest::SliceSet{}, std::move(built));
        if (id == ingest::kNoFile) {
          set_message("load: could not add " + path.filename().string(), true);
          break;
        }

        // ADDS RATHER THAN REPLACES: the pads are not touched. Whatever was on
        // them keeps playing, from whichever file it came -- which is the whole
        // reason a pad names a file.
        current = id;
        show_current(last_columns);
        set_message("loaded " + path.filename().string() + " (" + std::to_string(pool.size()) +
                        " in the crate)",
                    false);
        break;
      }

      case CommandKind::kSelectFile: {
        if (command.file > pool.size()) {
          set_message("no file " + std::to_string(command.file) + " (have " +
                          std::to_string(pool.size()) + ")",
                      true);
          break;
        }
        current = pool.entries()[command.file - 1].id;
        show_current(last_columns);
        set_message("showing " + state.sample_name, false);
        break;
      }

      case CommandKind::kUnloadFile: {
        const ingest::FileId target =
            command.file == 0 ? current
                              : (command.file <= pool.size() ? pool.entries()[command.file - 1].id
                                                             : ingest::kNoFile);
        const ingest::PoolEntry* going = pool.find(target);
        if (going == nullptr) {
          set_message(pool.empty() ? "nothing loaded" : "no such file", true);
          break;
        }
        const std::string name = going->name;

        // PADS PLAYING IT KEEP PLAYING, and voices sounding from it keep
        // sounding: each holds its own shared_ptr through its PadConfig, so this
        // drops the crate's reference and no more. Nothing is published and
        // nothing is stopped.
        static_cast<void>(pool.remove(target));
        if (current == target) {
          current = pool.empty() ? ingest::kNoFile : pool.entries().front().id;
        }
        show_current(last_columns);
        set_message("unloaded " + name + " (pads holding it still play)", false);
        break;
      }

      case CommandKind::kListFiles: {
        if (pool.empty()) {
          set_message("the crate is empty — :load <path>", false);
          break;
        }
        std::string said;
        std::size_t index = 1;
        for (const ingest::PoolEntry& file : pool.entries()) {
          if (!said.empty()) {
            said += " · ";
          }
          said += std::to_string(index) + " " + file.name;
          if (file.id == current) {
            said += "*";
          }
          said += " (" + std::to_string(file.slices.size()) + ")";
          ++index;
        }
        set_message(said, false);
        break;
      }

      case CommandKind::kPadPitch: {
        // The DSP has been here since M3: `pitch_ratio` is multiplied into the
        // phase step in VoicePool::step_for() and clamped on arrival. This is
        // the control that was missing, which makes it the fifth thing this
        // project has found the engine able to do and nothing able to ask for.
        const auto pad = static_cast<std::uint8_t>(command.pad - 1);
        const std::shared_ptr<const rt::PadConfig> held = engine.pad_config(pad);
        if (held == nullptr || held->sample == nullptr) {
          set_message("pad " + std::to_string(command.pad) + " has nothing on it", true);
          break;
        }

        rt::PadConfig next = *held;
        next.pitch_ratio = rt::clamp_pitch_ratio(command.decibels);

        if (!engine.publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(next)))) {
          set_message("pads are busy — the edit did not happen, try again", true);
          break;
        }
        refresh_edit(last_columns);

        // BOTH UNITS, because the grammar cannot tell `pitch 3 7` meaning seven
        // semitones from `pitch 3 7` meaning seven times. Saying what was
        // understood is what makes the rule discoverable rather than a trap.
        set_message(pitch_summary(command.pad, rt::clamp_pitch_ratio(command.decibels)), false);
        break;
      }

      case CommandKind::kPadReverse: {
        const auto pad = static_cast<std::uint8_t>(command.pad - 1);
        const std::shared_ptr<const rt::PadConfig> held = engine.pad_config(pad);
        if (held == nullptr || held->sample == nullptr) {
          set_message("pad " + std::to_string(command.pad) + " has nothing on it", true);
          break;
        }

        rt::PadConfig next = *held;
        next.reverse =
            command.toggle == Switch::kToggle ? !held->reverse : command.toggle == Switch::kOn;

        if (!engine.publish_pad_config(std::make_shared<const rt::PadConfig>(next))) {
          set_message("pads are busy — the edit did not happen, try again", true);
          break;
        }
        refresh_edit(last_columns);
        set_message("pad " + std::to_string(command.pad) + " plays " +
                        (next.reverse ? "backwards" : "forwards"),
                    false);
        break;
      }

      case CommandKind::kPadEnvelope: {
        // THE ENGINE HAS HONOURED PadConfig::env SINCE M3 and nothing could set
        // it: EDIT drew the four segments and every one of them was the default,
        // so the panel promised a control that did not exist. Reported as "the
        // ADSR in edit view aren't doing anything", which was exactly right.
        const auto pad = static_cast<std::uint8_t>(command.pad - 1);
        const std::shared_ptr<const rt::PadConfig> held = engine.pad_config(pad);
        if (held == nullptr || held->sample == nullptr) {
          set_message("pad " + std::to_string(command.pad) + " has nothing on it", true);
          break;
        }

        rt::PadConfig next = *held;
        const auto frames = [&](float milliseconds) {
          return static_cast<std::size_t>(static_cast<double>(milliseconds) *
                                          static_cast<double>(options.sample_rate) / 1000.0);
        };

        std::string said = "pad " + std::to_string(command.pad) + " ";
        if (command.text == "a") {
          next.env.attack = frames(command.attack_ms);
          said += "attack " +
                  tui::detail::with_precision(static_cast<double>(command.attack_ms), 2) + " ms";
        } else if (command.text == "d") {
          next.env.decay = frames(command.attack_ms);
          said += "decay " +
                  tui::detail::with_precision(static_cast<double>(command.attack_ms), 2) + " ms";
        } else if (command.text == "r") {
          next.env.release = frames(command.attack_ms);
          said += "release " +
                  tui::detail::with_precision(static_cast<double>(command.attack_ms), 2) + " ms";
        } else {
          next.env.sustain = tui::detail::db_to_linear(command.decibels);
          said += "sustain " +
                  tui::detail::with_precision(static_cast<double>(command.decibels), 1) + " dB";
        }

        if (!engine.publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(next)))) {
          set_message("pads are busy — the edit did not happen, try again", true);
          break;
        }
        refresh_edit(last_columns);
        set_message(said, false);
        break;
      }

      case CommandKind::kBrowse: {
        // Starts where the last load came from when nowhere is named, which is
        // almost always where the next one is too.
        std::filesystem::path where{command.text};
        if (command.text.empty()) {
          const ingest::PoolEntry* file = entry();
          where = file != nullptr && file->path.has_parent_path() ? file->path.parent_path()
                                                                  : std::filesystem::current_path();
        }
        read_directory(where);
        state.screen = Screen::kBrowse;
        break;
      }

      case CommandKind::kQuit:
        quit();
        break;
    }
  };

  // One frame of grace for a just-published audition. See the state sync inside
  // the renderer, and the toggle in BROWSE and EDIT that depends on it.
  bool audition_settling = false;

  // The decoded preview, kept so the waveform can be re-summarised when the
  // terminal is resized without decoding the file again.
  //
  // HERE RATHER THAN IN UiState, which holds only what a renderer needs: the
  // Sample and the pyramid are the inputs the summary is computed FROM, and
  // putting them in the state a pure render function reads would invite that
  // function to compute rather than draw. Same reason `bins` is a summary and
  // the current file's Sample is not in UiState either.
  struct Preview {
    std::shared_ptr<const rt::Sample> sample;
    ingest::PeakPyramid pyramid;
    std::string name;

    // The PATH, not just the name, because grabbing a region has to put the
    // file in the crate and two directories can hold the same filename.
    std::filesystem::path path;
  } preview;

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

    // THE CRATE, listed. Rebuilt each frame rather than maintained: it is at
    // most a handful of entries of a few words each, and a cached copy is one
    // more thing that can disagree with the pool after a load or an unload.
    state.files.clear();
    state.files.reserve(pool.size());
    for (const ingest::PoolEntry& file : pool.entries()) {
      state.files.push_back(UiState::FileEntry{
          .id = file.id,
          .name = file.name,
          .slices = file.slices.size(),
          .frames = file.sample != nullptr ? file.sample->num_frames() : 0,
          .rate = file.sample != nullptr ? file.sample->sample_rate() : 0,
          .channels = file.sample != nullptr ? file.sample->num_channels() : std::uint16_t{0},
      });
    }
    state.current_file = current;

    state.active_voices = engine.active_voices();

    // What the capture lane is doing, straight off the engine's telemetry.
    //
    // `lost` is NOT cleared when the take ends. A hole in a recording cannot be
    // repaired afterwards, so the count stays up until the next take begins,
    // which is the only moment it stops being about the take you have.
    state.capture.recording = telemetry.record_state == rt::RecordState::kRecording;
    state.capture.armed = telemetry.record_state == rt::RecordState::kArmed;
    state.capture.master = capture_source == rt::RecordSource::kMaster;
    state.capture.seconds = static_cast<float>(telemetry.recorded_frames) /
                            static_cast<float>(std::max(engine.config().sample_rate, 1U));
    state.capture.lost = telemetry.record_dropped_frames;

    // A preview that ran to its end is no longer the thing space would stop.
    // Without this the toggle would refuse to replay the last file you heard.
    //
    // ONE FRAME OF GRACE, because publishing an audition and the engine adopting
    // it are not the same moment. This runs on every event; adopt_offline() runs
    // only on the refresh tick, so with no audio device the count is still zero
    // when the frame right after the keystroke asks. Without the grace the
    // label was cleared before it could ever match, and the toggle needed THREE
    // presses to stop -- play, play, stop -- which is what it did when measured.
    if (audition_settling) {
      audition_settling = false;
    } else if (engine.active_auditions() == 0) {
      state.auditioning.clear();
    }
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
    const ingest::PoolEntry* drawn = entry();
    if (drawn != nullptr && !drawn->pyramid.empty()) {
      // Each screen has its own view and its own panel width -- EDIT spends six
      // columns on the amplitude gutter -- so which one is up decides both the
      // span summarised and the number of bins to summarise it into. Using one
      // pair for both would stretch whichever screen lost.
      // CHOP always shows the WHOLE file: it is about where every cut lands,
      // and a zoomed view would hide the ones outside it while still counting
      // them.
      const bool editing = state.screen == Screen::kEdit;
      WaveView whole_file;
      whole_file.first_frame = 0;
      whole_file.frames_visible = drawn->sample->num_frames();
      const WaveView& view = state.screen == Screen::kChop ? whole_file
                             : editing                     ? state.edit.view
                                                           : state.view;
      const std::size_t wave_columns =
          editing ? edit_wave_columns_for(columns) : wave_columns_for(columns);
      state.bins.assign(bins_for_columns(wave_columns), ingest::PeakBin{});
      drawn->pyramid.summarize(*drawn->sample, 0, view.first_frame, view.frames_visible,
                               state.bins);
    }

    // The preview strip. Summarised per frame rather than per audition because
    // its width follows the terminal's, and the pyramid it reads was built once
    // when the sound started.
    //
    // THE WHOLE FILE, always, with no zoom: a preview answers "what is this",
    // and a view you could scroll would be EDIT with fewer keys.
    if (state.screen == Screen::kBrowse && preview.sample != nullptr) {
      state.preview.name = preview.name;
      state.preview.frames = preview.sample->num_frames();
      state.preview.rate = preview.sample->sample_rate();
      state.preview.bins.assign(bins_for_columns(preview_columns_for(columns)), ingest::PeakBin{});
      preview.pyramid.summarize(*preview.sample, 0, 0, state.preview.frames, state.preview.bins);

      const std::uint64_t at = engine.audition_playhead();
      state.preview.playing = at != engine::Engine::kNoAuditionPlayhead;
      if (state.preview.playing) {
        state.preview.playhead = static_cast<std::size_t>(at);
      }
    } else if (state.screen != Screen::kBrowse) {
      state.preview.bins.clear();
      state.preview.frames = 0;
    }

    // The detection function, summarised to one value a column.
    //
    // The MAXIMUM of each column's analysis frames rather than the mean: a peak
    // one frame wide is exactly what an onset looks like, and averaging it away
    // would draw a picture in which the thing being detected is invisible.
    if (state.screen == Screen::kChop && !chop_result.flux.empty()) {
      // TWO LEVELS A CHARACTER COLUMN, because envelope_rows() takes one per
      // BRAILLE dot column and there are two of those in a cell. Sized to the
      // cells alone, the curve drew across half the panel and left the rest
      // blank -- which looked like a file that stopped halfway.
      const std::size_t cells = (columns > 2 ? columns - 2 : 1) * kDotColumnsPerCell;
      state.chop.flux.assign(cells, 0.0F);
      state.chop.threshold.assign(cells, 0.0F);
      for (std::size_t column = 0; column < cells; ++column) {
        const std::size_t first = column * chop_result.flux.size() / cells;
        const std::size_t last =
            std::max(first + 1, (column + 1) * chop_result.flux.size() / cells);
        for (std::size_t at = first; at < last && at < chop_result.flux.size(); ++at) {
          state.chop.flux[column] = std::max(state.chop.flux[column], chop_result.flux[at]);
          state.chop.threshold[column] =
              std::max(state.chop.threshold[column], chop_result.threshold[at]);
        }
      }
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
  // "0.42s — 1.08s · 0.66s" for the mode line, or why there is nothing.
  auto region_summary = [](const PreviewState& mark) {
    if (!mark.has_region || mark.rate == 0) {
      return std::string{"no region — i marks the start, o the end"};
    }
    const auto seconds = [&](std::size_t frame) {
      return detail::with_precision(static_cast<double>(frame) / mark.rate, 2);
    };
    return "region " + seconds(mark.region_start) + "s — " + seconds(mark.region_end) + "s · " +
           seconds(mark.region_end - mark.region_start) + "s · a pad key puts it on a pad";
  };

  // Puts the marked region on a pad.
  //
  // THE FILE JOINS THE CRATE, with that one region as its only slice. It is
  // tempting to keep it out -- "just get a slice" sounds like "without loading
  // the file" -- but the audio is already decoded and in memory the moment you
  // previewed it, so nothing is saved by hiding it, and a pad naming a file the
  // crate does not have could not be drawn, listed or serialised by M6. What
  // "just a slice" buys you is that the file arrives with ONE cut instead of
  // sixteen, which is the part that was actually in the way.
  auto grab_region = [&](std::uint8_t pad) {
    PreviewState& mark = state.preview;
    if (preview.sample == nullptr || !mark.has_region) {
      set_message("mark a region first — space to hear a file, then i and o", true);
      return;
    }

    const ingest::Slice slice{.start_frame = mark.region_start, .end_frame = mark.region_end};

    // Already in the crate: append the cut rather than adding the file twice.
    // pool.add() deliberately leaves an existing entry alone, including its
    // slices, so a second grab from the same file has to go this way.
    ingest::FileId id = pool.find_path(preview.path);
    std::size_t slice_index = 0;
    if (id == ingest::kNoFile) {
      ingest::SliceSet set;
      set.slices.push_back(slice);
      id = pool.add(preview.sample, preview.path, std::move(set), preview.pyramid);
      if (id == ingest::kNoFile) {
        set_message("could not add " + preview.name + " to the crate", true);
        return;
      }
    } else {
      ingest::PoolEntry* held = pool.find(id);
      if (held == nullptr) {
        set_message("could not add " + preview.name + " to the crate", true);
        return;
      }
      held->slices.slices.push_back(slice);
      slice_index = held->slices.size() - 1;
    }

    rt::PadConfig config{};
    config.sample = preview.sample;
    config.pad = pad;
    config.start_frame = slice.start_frame;
    config.end_frame = slice.end_frame;
    if (!engine.publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(config)))) {
      set_message("pads are busy — try again", true);
      return;
    }

    state.pads[pad].loaded = true;
    state.pads[pad].has_slice = true;
    state.pads[pad].file = id;
    state.pads[pad].slice_index = slice_index;
    state.pads[pad].name = slice_pad_name(slice_index + 1);

    const double length =
        mark.rate > 0 ? static_cast<double>(slice.end_frame - slice.start_frame) / mark.rate : 0.0;
    set_message(preview.name + " " + detail::with_precision(length, 2) + "s → pad " +
                    std::to_string(pad + 1),
                false);
  };

  auto close_completion = [&state]() {
    state.completion.entries.clear();
    state.completion.cursor = 0;
    state.completion.active = false;
  };

  // Candidate paths for a partially typed one.
  //
  // ON THE CONTROL THREAD, where read_directory() already is, and NOT in
  // completion.cpp: choosing among names is a pure function and reading a
  // directory is I/O, so they are two functions in two places. Same split as
  // BROWSE, for the same reason -- the pure half is the half worth testing and
  // it should not need a filesystem to test.
  auto path_candidates = [](const PathContext& context) {
    std::vector<std::string> names;
    const std::filesystem::path typed{context.partial};

    // The directory to read, and the stem to match inside it. A partial that
    // ends in a separator is a finished directory name -- `load /crate/` means
    // "what is in /crate" -- while `/crate/br` means "what in /crate starts
    // with br". std::filesystem::path gets this right via parent_path() only
    // when the trailing separator is there, which is why it is tested first.
    const bool whole_directory =
        !context.partial.empty() &&
        context.partial.back() == std::filesystem::path::preferred_separator;
    const std::filesystem::path where = whole_directory           ? typed
                                        : typed.has_parent_path() ? typed.parent_path()
                                                                  : std::filesystem::path{"."};

    std::error_code failed;
    const std::filesystem::directory_iterator listing{where, failed};
    if (failed) {
      return names;  // an unreadable directory offers nothing, and says nothing
    }

    for (const std::filesystem::directory_entry& item : listing) {
      std::error_code ignored;
      const std::string name = item.path().filename().string();
      if (name.empty() || name.front() == '.') {
        continue;  // dotfiles are noise here for the reason they are in BROWSE
      }
      const bool directory = item.is_directory(ignored);
      if (!directory && !looks_like_audio(item.path())) {
        continue;
      }
      // Offered as it would be TYPED, not as the filesystem spells it: the
      // partial the person wrote is the prefix, so `load ./b` must offer
      // `./break.wav` rather than `break.wav`. Rebuilding from `where` keeps
      // the two in step whatever they typed.
      std::string full = (where / name).string();
      if (directory) {
        // A trailing separator, so a second Tab descends rather than stopping.
        full.push_back(static_cast<char>(std::filesystem::path::preferred_separator));
      }
      names.push_back(std::move(full));
    }
    return names;
  };

  // Tab: open the menu if it is closed, move within it if it is open.
  auto step_completion = [&](bool backwards) {
    CompletionState& menu = state.completion;

    if (!menu.showing()) {
      const PathContext context = path_being_typed(state.command_text);
      const CompletionSet set = context.is_path ? complete_paths(context, path_candidates(context))
                                                : complete_verbs(state.command_text);
      if (set.empty()) {
        // Said out loud. A Tab that does nothing at all is indistinguishable
        // from a Tab the terminal ate -- which this program has already spent a
        // milestone believing.
        set_message(context.is_path ? "nothing here to complete" : "no command starts with that",
                    false);
        return;
      }
      menu.entries = set.entries;
      menu.replace_from = set.replace_from;
      menu.cursor = 0;
      menu.active = true;
    } else {
      const std::size_t count = menu.entries.size();
      menu.cursor = backwards ? (menu.cursor + count - 1) % count : (menu.cursor + 1) % count;
    }

    // The line always shows the selection, so that Enter runs what you can see
    // rather than what you last typed. Applied through the SET's own rule,
    // which is why replace_from travels with the entries.
    CompletionSet applied;
    applied.entries = menu.entries;
    applied.cursor = menu.cursor;
    applied.replace_from = menu.replace_from;
    state.command_text = applied.apply(state.command_text);
  };

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
        // ESC CLOSES THE MENU FIRST, then the prompt. Two steps because they
        // are two things: having opened a menu by accident and wanting the
        // whole line gone are different intentions, and one key that always did
        // the second would make Tab a thing you had to be sure about before
        // pressing.
        if (state.completion.showing()) {
          close_completion();
          return true;
        }
        state.command_active = false;
        state.command_text.clear();
        close_completion();
        return true;
      }
      if (code == kReturn) {
        const Command command = parse_command(state.command_text);
        // Closed before running, not after: `:q` exits from inside execute(),
        // and leaving the prompt up until it returns would draw one last frame
        // with a stale `:` line in it.
        state.command_active = false;
        state.command_text.clear();
        close_completion();
        execute(command);
        return true;
      }
      if (code == kTab || code == kTabReverse || code == kKeyArrowDown || code == kKeyArrowUp) {
        const bool backwards = code == kTabReverse || code == kKeyArrowUp;
        step_completion(backwards);
        return true;
      }
      if (code == kBackspace) {
        pop_codepoint(state.command_text);
        // Typing dismisses the menu. The set was captured when Tab was pressed
        // and cycling it against a line that has since changed would offer
        // things that no longer match what is on screen.
        close_completion();
        return true;
      }
      // Anything that is not a printable character -- arrows, function keys,
      // Tab -- is eaten rather than passed through. Nothing below should act on
      // a keystroke aimed at the prompt.
      if (const std::string typed = printable(code); !typed.empty()) {
        state.command_text += typed;
        close_completion();
      }
      return true;
    }

    // BROWSE HAS ITS OWN KEYMAP, for the reason EDIT and MIX do: `a`, `s`, `d`,
    // `f` and `z` are pad keys, and a browser that played a drum every time you
    // moved down a listing would be unusable. Pads are off; space auditions
    // whatever is under the cursor, which is the one thing they were wanted for
    // here.
    if (state.screen == Screen::kBrowse) {
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

      BrowserState& browser = state.browser;
      const auto* under =
          browser.cursor < browser.entries.size() ? &browser.entries[browser.cursor] : nullptr;

      switch (code) {
        case 'j':
        case kKeyArrowDown:
          if (browser.cursor + 1 < browser.entries.size()) {
            browser.cursor++;
          }
          return true;

        case 'k':
        case kKeyArrowUp:
          if (browser.cursor > 0) {
            browser.cursor--;
          }
          return true;

        case 'h':
        case kKeyArrowLeft: {
          const std::filesystem::path here{browser.path};
          if (here.has_parent_path() && here.parent_path() != here) {
            const std::string leaving = here.filename().string();
            read_directory(here.parent_path());
            focus_entry(leaving);
          }
          return true;
        }

        case 'l':
        case kKeyArrowRight:
        case kReturn: {
          if (under == nullptr) {
            return true;
          }
          const std::filesystem::path here{browser.path};
          if (under->is_directory) {
            if (under->name == "..") {
              const std::string leaving = here.filename().string();
              read_directory(here.parent_path());
              focus_entry(leaving);
            } else {
              read_directory(here / under->name);
            }
            return true;
          }
          // A file: load it. `l` and Enter both, because one of them is what
          // every terminal browser uses and it is never the same one.
          const std::string loaded = under->name;
          execute(parse_command(std::string{"load "} + (here / loaded).string()));
          read_directory(here);  // the listing now says which are loaded
          focus_entry(loaded);   // ...and you are still standing where you were
          return true;
        }

        // -- the region, and grabbing it -------------------------------------
        //
        // "Browsing should have an ability to just get a slice from a sample":
        // mark a bit of what you are previewing and put THAT on a pad, rather
        // than loading the whole file and chopping it to get at one hit.
        //
        // `i` and `o` are the tape-op pair and are free here; `x`, `c`, `v` and
        // `z` are NOT available for "clear" because they are pad keys, which is
        // why that is on `-`.
        case 'i':
        case 'o': {
          if (!state.preview.showing()) {
            set_message("nothing to mark — press space to hear a file first", true);
            return true;
          }
          PreviewState& mark = state.preview;
          const bool in = code == 'i';

          // AT THE PLAYHEAD, which is what makes this a tape-op flow: you hear
          // the bit start and press `i`. With no audio device nothing advances,
          // so the marker sits at 0 and `o` takes the end of the file -- which
          // gives the whole file as a region rather than an empty one.
          const std::size_t at = mark.playing ? mark.playhead : (in ? 0 : mark.frames);

          if (in) {
            mark.region_start = at;
            if (!mark.has_region || mark.region_end <= at) {
              mark.region_end = mark.frames;
            }
          } else {
            mark.region_end = at > mark.region_start ? at : mark.frames;
          }
          mark.has_region = mark.region_end > mark.region_start;
          set_message(region_summary(mark), !mark.has_region);
          return true;
        }

        case '[':
        case ']': {
          // Nudge the out point. A sixteenth of the file a press: this is a
          // browser, and frame-accurate work is what EDIT is for.
          if (!state.preview.showing() || !state.preview.has_region) {
            set_message("no region to trim — i and o mark one", true);
            return true;
          }
          PreviewState& mark = state.preview;
          const std::size_t step = mark.frames / 16 + 1;
          if (code == '[') {
            mark.region_end = mark.region_end > mark.region_start + step ? mark.region_end - step
                                                                         : mark.region_start + 1;
          } else {
            mark.region_end = std::min(mark.frames, mark.region_end + step);
          }
          set_message(region_summary(mark), false);
          return true;
        }

        case '-': {
          state.preview.has_region = false;
          set_message("region cleared", false);
          return true;
        }

        case 'E': {
          // LOAD IT AND GO AND CUT IT UP, for when the answer is not one region
          // but the whole file. Everything EDIT does -- boundaries, nudging,
          // auditioning a slice on no pad -- already exists, so this is a door
          // to it rather than a second implementation of it.
          //
          // CAPITAL, because `e` is pad 7 and the pad keys are spent on grabbing
          // a region. Named in the hint line, since a key nobody can find is a
          // key that does not exist.
          if (under == nullptr || under->is_directory) {
            set_message("no file here to edit", true);
            return true;
          }
          const std::filesystem::path here{browser.path};
          execute(parse_command(std::string{"load "} + (here / under->name).string()));
          if (entry() == nullptr) {
            return true;  // the load failed and said why
          }
          state.screen = Screen::kEdit;
          state.edit.slice = 0;
          set_message("editing " + under->name, false);
          return true;
        }

        case kSpace: {
          // AUDITION BEFORE LOAD, which is the reason this screen needed the
          // audition path built first. Previewing a file you have not loaded is
          // the same problem as playing a slice that is on no pad, and it is the
          // same mechanism.
          if (under == nullptr || under->is_directory) {
            return true;
          }

          // SPACE STOPS WHAT SPACE STARTED. Reported as "browsing has no
          // deselection": the key only ever played, so a preview ran to its end
          // whatever you did, and there was no way to silence one -- the panic
          // key did not reach the audition lane either.
          //
          // On the SAME entry, because on a different one the useful thing is to
          // hear that one, not to stop this one and press space again.
          if (state.auditioning == under->name) {
            static_cast<void>(engine.stop_audition());
            state.auditioning.clear();
            set_message("stopped " + under->name, false);
            return true;
          }

          const std::filesystem::path here{browser.path};
          const ingest::SampleLoad load =
              ingest::load_sample(here / under->name, options.sample_rate);
          if (!load.ok()) {
            set_message(
                "cannot play " + under->name + " — " + std::string{ingest::describe(load.error)},
                true);
            return true;
          }
          auto config =
              std::make_shared<const rt::PadConfig>(rt::PadConfig{.sample = load.sample, .pad = 0});
          if (!engine.audition(std::move(config))) {
            set_message("audition is busy — try again", true);
            return true;
          }

          // The picture, built once here rather than per frame. Building a
          // pyramid walks the whole file, which is the cost `:load` already pays
          // and which must not be paid sixty times a second.
          preview.sample = load.sample;
          preview.pyramid = ingest::PeakPyramid::build(*load.sample);
          preview.name = under->name;
          preview.path = here / under->name;
          state.preview.has_region = false;  // a new sound is a new selection
          state.auditioning = under->name;
          audition_settling = true;
          set_message("playing " + under->name + " — space again to stop", false);
          return true;
        }

        default:
          break;
      }

      // A PAD KEY PUTS THE MARKED REGION ON THAT PAD.
      //
      // The pads are otherwise off on this screen -- a browser that played a
      // drum every time you moved down a listing would be unusable -- so the
      // same sixteen keys are free to mean "put it here" instead of "play it".
      // It is the map the whole program already uses, so it needs no new
      // concept and no new row in the hint line beyond saying so.
      //
      // Only with a region marked. Without one the key says what to do rather
      // than silently doing nothing, because a key that does nothing reads as
      // broken -- the lesson `h`/`l` and the ADSR panel both taught.
      if (const std::uint8_t pad = pad_for_key(code); pad < rt::kNumPads) {
        grab_region(pad);
        return true;
      }
      return true;
    }

    // CHOP HAS ITS OWN KEYMAP, for the reason BROWSE and MIX do: the pad keys
    // would play drums over the thing you are listening for.
    if (state.screen == Screen::kChop) {
      if (!typing) {
        return true;
      }
      if (code == kEscape) {
        // LEAVES IT ALONE. Esc is cancel here, not "apply and go" -- the whole
        // screen is a preview, and a preview that committed itself on the way
        // out would be an edit you did not ask for.
        state.screen = Screen::kPerform;
        set_message("chop unchanged", false);
        return true;
      }
      if (code == ':') {
        state.command_active = true;
        state.command_text.clear();
        return true;
      }

      ChopState& chop = state.chop;
      const auto fields = static_cast<std::size_t>(ChopState::Field::kCount);

      switch (code) {
        case 'j':
        case kKeyArrowDown:
          chop.field =
              static_cast<ChopState::Field>((static_cast<std::size_t>(chop.field) + 1) % fields);
          return true;

        case 'k':
        case kKeyArrowUp:
          chop.field = static_cast<ChopState::Field>(
              (static_cast<std::size_t>(chop.field) + fields - 1) % fields);
          return true;

        case 'h':
        case 'l':
        case 'H':
        case 'L':
        case kKeyArrowLeft:
        case kKeyArrowRight: {
          const bool up = code == 'l' || code == 'L' || code == kKeyArrowRight;
          const bool coarse = code == 'H' || code == 'L';
          const float step = coarse ? 5.0F : 1.0F;

          switch (chop.field) {
            case ChopState::Field::kSensitivity:
              // Clamped ABOVE ZERO: a lambda of 0 leaves only the absolute
              // floor, which on quiet material fires on every ripple.
              chop.lambda = std::clamp(chop.lambda + (up ? 0.05F : -0.05F) * step, 0.2F, 6.0F);
              break;

            case ChopState::Field::kGap:
              chop.gap_seconds =
                  std::clamp(chop.gap_seconds + ((up ? 0.005 : -0.005) * static_cast<double>(step)),
                             0.005, 4.0);
              break;

            case ChopState::Field::kLowCut:
              chop.low_cut = std::clamp(chop.low_cut + (up ? 0.02F : -0.02F) * step, 0.0F,
                                        ingest::kMaxHfEmphasis);
              // THE ONLY ADJUSTABLE THAT REACHES THE FFT, so the only one that
              // costs an analysis. Flagged rather than done here, because the
              // keymap has no business running an 80 ms computation inside a
              // keystroke handler.
              chop.needs_analysis = true;
              break;

            case ChopState::Field::kCount:
              break;
          }
          repick_chop();
          return true;
        }

        case kReturn: {
          // APPLY. The preview becomes the file's slices, and the pads follow.
          ingest::PoolEntry* file = entry();
          if (file == nullptr) {
            set_message("nothing loaded to chop", true);
            return true;
          }
          file->slices = ingest::slices_at(*file->sample, chop.boundaries);
          show_slices(file->slices, state);
          static_cast<void>(apply_slices(engine, *file, file->slices, state));
          state.screen = Screen::kPerform;
          set_message("chop: " + std::to_string(file->slices.size()) + " slices", false);
          return true;
        }

        default:
          break;
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
          // PAGING IS ON `[` AND `]`, and the reason recorded here in M5 was
          // WRONG: it said Tab never reaches this handler because FTXUI consumes
          // it before CatchEvent runs. It does reach it. A probe in M5.5 that
          // reports the code of every key showed Tab arriving as 9 on a plain
          // xterm, and the PERFORM panel switch it claimed was dead switches the
          // panel. The M5 probe read a stale screen -- the same mistake its
          // replacement made once before being written correctly.
          //
          // `[` and `]` stay, now on their own merits rather than on a false
          // one: EDIT already uses them to step through slices, and they are
          // bidirectional where Tab cycles one way through three pages. Tab
          // itself is spent on completion at the `:` prompt, which is where a
          // terminal user expects it.
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
        const std::uint8_t pad = pad_for_slice(state, state.current_file, state.edit.slice);
        if (pad >= rt::kNumPads) {
          // A SLICE NOT ON A PAD IS AUDITIONED, as of M5.5.
          //
          // It used to be unreachable: every route to sound was a pad trigger,
          // the audio thread played m_pads[N], and there was nothing to address
          // material no pad held -- so a chop of more than sixteen left the rest
          // editable, drawable and silent. M4.5 made this key SAY that rather
          // than do nothing, which was as far as it could go. The audition path
          // is the mechanism it was waiting for.
          //
          // THROUGH THE AUDITION LANE, not through some pad borrowed for the
          // purpose: a preview must not light a pad it does not belong to, and
          // must not be silenced by that pad's mute.
          const ingest::PoolEntry* file = entry();
          if (file == nullptr || state.edit.slice >= file->slices.size()) {
            set_message("nothing to audition", true);
            return true;
          }

          // A toggle here too, for the reason it is one in BROWSE: a preview you
          // cannot stop is one you have to wait out.
          const std::string label = "slice " + std::to_string(state.edit.slice + 1);
          if (state.auditioning == label) {
            static_cast<void>(engine.stop_audition());
            state.auditioning.clear();
            set_message("stopped " + label, false);
            return true;
          }

          const ingest::Slice& slice = file->slices.slices[state.edit.slice];

          // Built from the file and the slice, with the defaults a pad would
          // have. Not copied from a pad's config: there is no pad, and borrowing
          // a neighbouring one's envelope would make the preview sound like
          // something the slice is not.
          rt::PadConfig config{};
          config.sample = file->sample;
          config.start_frame = slice.start_frame;
          config.end_frame = slice.end_frame;

          if (!engine.audition(std::make_shared<const rt::PadConfig>(std::move(config)))) {
            set_message("audition is busy — try again", true);
            return true;
          }
          state.auditioning = label;
          audition_settling = true;
          set_message("auditioning " + label + " (on no pad — :slot assign " +
                          std::to_string(state.edit.slice + 1) + " <pad> to keep it)",
                      false);
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

        case 'r': {
          // REVERSE, on the slice under the cursor. `r` is pad 8 in PERFORM and
          // free here, because EDIT turns the pad map off -- the same room that
          // makes `z`, `u` and `-`/`=` available.
          const ingest::PoolEntry* file = entry();
          if (file == nullptr || state.edit.slice >= file->slices.size()) {
            set_message("no slice to reverse", true);
            break;
          }
          const std::uint8_t pad = pad_for_slice(state, state.current_file, state.edit.slice);
          if (pad >= rt::kNumPads) {
            set_message("slice " + std::to_string(state.edit.slice + 1) +
                            " is on no pad — :slot assign it first",
                        true);
            break;
          }
          const std::shared_ptr<const rt::PadConfig> held = engine.pad_config(pad);
          if (held == nullptr) {
            break;
          }
          rt::PadConfig next = *held;
          next.reverse = !held->reverse;
          if (!engine.publish_pad_config(std::make_shared<const rt::PadConfig>(next))) {
            set_message("pads are busy — try again", true);
            break;
          }
          refresh_edit(last_columns);
          set_message("pad " + std::to_string(pad + 1) + " plays " +
                          (next.reverse ? "backwards" : "forwards"),
                      false);
          break;
        }

        case '-':
        case '=': {
          // PITCH, ON THE SLICE UNDER THE CURSOR, in semitones -- the unit an
          // ear works in, and the reason the keys move by one rather than by a
          // ratio step. The `:pitch` verb takes either unit; this is the one you
          // reach for while listening.
          //
          // `-` and `=` because they sit together on the keyboard and neither is
          // a pad key. `+` needs shift and would be a different key on half the
          // layouts this runs on.
          const ingest::PoolEntry* file = entry();
          if (file == nullptr || state.edit.slice >= file->slices.size()) {
            set_message("no slice to pitch", true);
            break;
          }
          const std::uint8_t pad = pad_for_slice(state, state.current_file, state.edit.slice);
          if (pad >= rt::kNumPads) {
            set_message("slice " + std::to_string(state.edit.slice + 1) +
                            " is on no pad — :slot assign it first",
                        true);
            break;
          }
          const std::shared_ptr<const rt::PadConfig> held = engine.pad_config(pad);
          if (held == nullptr) {
            break;
          }
          const float semitones =
              rt::semitones_from_ratio(held->pitch_ratio) + (code == '=' ? 1.0F : -1.0F);
          rt::PadConfig next = *held;
          next.pitch_ratio =
              rt::ratio_from_semitones(std::clamp(semitones, rt::kMinSemitones, rt::kMaxSemitones));
          if (!engine.publish_pad_config(std::make_shared<const rt::PadConfig>(std::move(next)))) {
            set_message("pads are busy — try again", true);
            break;
          }
          refresh_edit(last_columns);
          set_message(pitch_summary(pad + 1U, rt::ratio_from_semitones(semitones)), false);
          break;
        }
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
    // The right-hand panel, on Tab and on BACKSLASH.
    //
    // M5 recorded that Tab "has never worked" because FTXUI consumes it before
    // CatchEvent runs. THAT WAS WRONG, and the correction is left here because
    // the claim reached three comments and one design decision before anyone
    // pressed the key. Tab arrives as 9 on a plain xterm; this binding, in this
    // file since M2, switches the panel. The M5 probe read a stale screen.
    //
    // Backslash stays as the alias. It cost nothing to add and it is the one a
    // person reaches for when their terminal has bound Tab to something else.
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

    // RECORD-ARM, on `R`.
    //
    // Capital, because the pad map owns every lower-case letter in the four rows
    // and `r` is pad 8. Shift is no burden for this one: arming happens between
    // takes rather than between notes, unlike everything the pad map claims.
    //
    // Press only, like the transport and the panic. Holding it would arm and
    // disarm at the terminal's repeat rate, and with `replace` set that would
    // clear the pattern on every other repeat.
    if (code == 'R' && press) {
      set_armed(!state.take.armed);
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
      case 'l':
      case kKeyArrowRight: {
        // SAYS SO WHEN THERE IS NOWHERE TO GO.
        //
        // At the default zoom the whole file is on screen, so scrolling is
        // correctly a no-op -- and a key that correctly does nothing is
        // indistinguishable from a key that is broken. "h and l aren't working"
        // was reported against exactly this: they work, and at fit-to-file zoom
        // there is nothing for them to do, and nothing said so.
        //
        // Only when it cannot move. A message on every scroll would be noise on
        // the one screen that is meant to stay quiet while you play.
        const std::size_t before = state.view.first_frame;
        const bool left = code == 'h' || code == kKeyArrowLeft;
        state.view.scroll_by(left ? -step : step, total_frames);
        if (state.view.first_frame == before && state.view.frames_visible >= total_frames) {
          set_message("the whole file is on screen — + to zoom in first", false);
        }
        break;
      }
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

      // The low cut moved, so the FFT half has to run again. HERE rather than in
      // the keystroke handler: it is ~80 ms on ordinary material, and a keymap
      // that stopped to compute would drop the keys pressed behind it.
      if (state.chop.needs_analysis && state.screen == Screen::kChop) {
        state.chop.needs_analysis = false;
        if (const ingest::PoolEntry* file = entry(); file != nullptr) {
          chop_analysis = ingest::analyse_onsets(*file->sample, chop_params());
          repick_chop();
        }
      }
      static_cast<void>(engine.collect_garbage());

      // CAPTURE. Drained every tick while a take is running, for the same
      // reason as the hit ring below and with a harder consequence: the chunk
      // pool holds 2.7 seconds, and a collector that waits until the end gets
      // the first 2.7 seconds of the take and a drop count for the rest.
      static_cast<void>(engine.collect_take());

      if (capture_finish != CaptureFinish::kNone && engine.take_complete()) {
        const std::size_t frames = engine.take_frames();
        const bool keep = capture_finish == CaptureFinish::kKeep;
        capture_finish = CaptureFinish::kNone;

        if (!keep) {
          engine.discard_take();
          set_message("capture dropped", false);
        } else if (frames == 0) {
          // Not an error, and not silently nothing either: a stop that produced
          // no audio has a cause -- no device, or the threshold never crossed --
          // and saying "0 frames" is what points at it.
          engine.discard_take();
          set_message("capture: nothing was recorded", true);
        } else {
          // THE TAKE BECOMES A FILE IN THE CRATE, rather than going straight to
          // a pad. Everything else that arrives as audio does that, so a take
          // gets `:chop`, EDIT, the browser and `:slot assign` for free instead
          // of a second path that would have to grow each of them again.
          std::shared_ptr<rt::Sample> sample = engine.build_take();
          engine.discard_take();

          const std::string name = "take " + std::to_string(++takes_made);
          ingest::PeakPyramid built =
              sample == nullptr ? ingest::PeakPyramid{} : ingest::PeakPyramid::build(*sample);
          const ingest::FileId id = sample == nullptr
                                        ? ingest::kNoFile
                                        : pool.add(std::move(sample), std::filesystem::path{name},
                                                   ingest::SliceSet{}, std::move(built));
          if (id == ingest::kNoFile) {
            set_message("capture: could not add the take to the crate", true);
          } else {
            current = id;
            show_current(last_columns);
            const double seconds = static_cast<double>(frames) /
                                   static_cast<double>(std::max(engine.config().sample_rate, 1U));
            set_message(name + " — " + detail::with_precision(seconds, 2) + "s in the crate",
                        false);
          }
        }
      }

      // THE TAKE. Live hits are drained EVERY tick, armed or not.
      //
      // Draining only while recording would leave a producer with no consumer
      // the rest of the time, and the ring would fill and stay full -- so the
      // first take after a while of ordinary playing would be missing its
      // opening notes, with nothing to suggest why. That failure has been paid
      // for twice already in this project (the audition ring, M4.5), and it
      // costs one loop to avoid.
      live_hits.clear();
      rt::PadHit hit{};
      while (engine.next_hit(hit)) {
        live_hits.push_back(hit);
      }

      if (state.take.armed && !live_hits.empty()) {
        std::size_t written = 0;
        const std::uint32_t rate = engine.config().sample_rate;
        const std::uint8_t quantise = state.take.quantise_steps;

        // One publish for the whole tick rather than one per note. A rolled
        // sixteenth is four hits in a frame's worth of time, and four 16 KB
        // copies through the handoff ring to say what one could.
        if (edit_sequencer(
                [&](rt::SequencerState& next) {
                  written = engine::record_hits(next, rate, quantise, live_hits);
                },
                "")) {
          state.take.recorded += written;
          if (written > 0) {
            // The first note is what makes the take worth putting back.
            state.take.can_undo = true;
          }
        }
      }

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
