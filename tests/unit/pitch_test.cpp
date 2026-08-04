#include "rt/pitch.hpp"

#include "rt/garbage_ring.hpp"
#include "rt/pad_config.hpp"
#include "rt/sample.hpp"
#include "rt/voice_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Playback speed: the units, and the price of using it.
//
// The DSP has been in VoicePool::step_for() since M3. What is new is a control
// that can set it, and the honest number for what it costs -- which
// docs/ROADMAP.md asks be MEASURED AND WRITTEN DOWN rather than discovered by
// somebody wondering why their chipmunk vocal sounds like a fax machine.

namespace {

constexpr std::uint32_t kRate = 48'000;

// Two tones at once: a musical one and "air" high enough to fold when pitched.
[[nodiscard]] std::vector<float> render_at_two_tones(float low_hz, float air_hz, float ratio,
                                                     std::size_t frames) {
  const std::size_t source_frames = frames * 8;
  auto sample = std::make_shared<rt::Sample>(kRate, std::uint16_t{1}, source_frames);
  const std::span<float> data = sample->mutable_channel(0);
  for (std::size_t frame = 0; frame < source_frames; ++frame) {
    const auto turn =
        2.0 * std::numbers::pi * static_cast<double>(frame) / static_cast<double>(kRate);
    data[frame] = 0.4F * static_cast<float>(std::sin(turn * static_cast<double>(low_hz))) +
                  0.4F * static_cast<float>(std::sin(turn * static_cast<double>(air_hz)));
  }

  rt::VoicePool<4> pool;
  rt::GarbageRing<8> garbage;
  auto config = std::make_shared<const rt::PadConfig>(
      rt::PadConfig{.sample = std::move(sample), .pitch_ratio = ratio});
  REQUIRE(pool.trigger(config, 1.0F, kRate, garbage, 0));

  std::vector<float> out(frames, 0.0F);
  std::array<float*, 1> channels{out.data()};
  pool.render_add(std::span<float* const>{channels}, frames);
  return out;
}

// Hann-windowed magnitude at one frequency. See the note in the alias test for
// why the window is load-bearing rather than tidy.
[[nodiscard]] double magnitude_of(const std::vector<float>& signal, double hz) {
  constexpr std::size_t kSkip = 512;
  const std::size_t count = signal.size() > kSkip ? signal.size() - kSkip : signal.size();
  double real = 0.0;
  double imaginary = 0.0;
  for (std::size_t frame = 0; frame < count; ++frame) {
    const double turn =
        2.0 * std::numbers::pi * static_cast<double>(frame) / static_cast<double>(count);
    const double window = 0.5 - (0.5 * std::cos(turn));
    const double value = static_cast<double>(signal[frame + (signal.size() - count)]) * window;
    const double angle =
        2.0 * std::numbers::pi * hz * static_cast<double>(frame) / static_cast<double>(kRate);
    real += value * std::cos(angle);
    imaginary -= value * std::sin(angle);
  }
  return std::sqrt((real * real) + (imaginary * imaginary)) / static_cast<double>(count);
}

}  // namespace

TEST_CASE("semitones and ratios are the same number in two units", "[unit]") {
  CHECK(rt::ratio_from_semitones(0.0F) == Catch::Approx(1.0F));
  CHECK(rt::ratio_from_semitones(12.0F) == Catch::Approx(2.0F));
  CHECK(rt::ratio_from_semitones(-12.0F) == Catch::Approx(0.5F));
  CHECK(rt::ratio_from_semitones(7.0F) == Catch::Approx(1.4983F).margin(0.001));

  // And back, for every semitone across the range: the conversion is what the
  // interface shows, so a round trip that drifted would make the readout lie.
  for (int semitone = -72; semitone <= 72; ++semitone) {
    const auto asked = static_cast<float>(semitone);
    INFO("semitone: " << semitone);
    CHECK(rt::semitones_from_ratio(rt::ratio_from_semitones(asked)) ==
          Catch::Approx(asked).margin(0.001));
  }
}

TEST_CASE("a pitch out of range is clamped rather than ignored", "[unit]") {
  // VoicePool::step_for() substitutes 1.0 for anything outside its bounds, which
  // is the right thing on the audio thread and the WRONG thing to let a person
  // reach: asking for 200x and silently getting 1x is a control that lies.
  CHECK(rt::clamp_pitch_ratio(200.0F) == rt::kMaxPitchRatio);
  CHECK(rt::clamp_pitch_ratio(0.0F) == 1.0F);
  CHECK(rt::clamp_pitch_ratio(-3.0F) == 1.0F);
  CHECK(rt::clamp_pitch_ratio(std::nanf("")) == 1.0F);
  CHECK(rt::clamp_pitch_ratio(1.0F / 1'000.0F) == rt::kMinPitchRatio);

  // The clamp is inside what the audio thread accepts, so nothing that survives
  // it is quietly replaced downstream.
  CHECK(rt::kMaxPitchRatio <= 64.0F);
  CHECK(rt::kMinPitchRatio > 0.0F);
}

TEST_CASE("pitching up folds content above Nyquist back down", "[unit]") {
  // THE NUMBER docs/ROADMAP.md ASKS FOR, and it took two wrong measurements to
  // get to. Reading the source r times faster puts a component at f on f*r; once
  // f*r passes Nyquist it folds back to |sample_rate - f*r| and lands in the
  // audible band as a tone that was never played. A 4-point Hermite
  // INTERPOLATES; it does not decimate, so it cannot prevent that.
  //
  // WHAT THE FIRST TWO ATTEMPTS GOT WRONG, because both are easy to repeat:
  //
  //   1. An unwindowed DFT leaks at about -46 dB, so the first version reported
  //      -46 dB at UNITY -- where the output is a bit-exact copy of a pure sine
  //      and there is nothing to find. It was measuring its own window.
  //
  //   2. A single tone BELOW Nyquist/r cannot alias at all, however fast you
  //      play it. Measured with a 6 kHz tone: unity came out at -161 dB and 2x
  //      at -259 dB, both exact, because an integer step makes the phase
  //      fraction zero and hermite4() collapses to a copy. Aliasing needs
  //      content above sample_rate/(2r) to fold, which a low tone does not have.
  //
  // So the source carries a 15 kHz component -- "air", which real material has
  // and a test tone does not. At 1.5x it lands at 22.5 kHz, still under Nyquist,
  // and nothing folds. At 2x it lands at 30 kHz and folds to 18 kHz. At 4x it
  // lands at 60 kHz and folds to 12 kHz, right in the middle of the music.
  constexpr float kLow = 1'000.0F;
  constexpr float kAir = 15'000.0F;
  constexpr std::size_t kFrames = 8'192;

  const auto alias_of = [](double ratio) {
    const double played = static_cast<double>(kAir) * ratio;
    const double nyquist = static_cast<double>(kRate) / 2.0;
    if (played <= nyquist) {
      return 0.0;  // nothing folds
    }
    return std::abs(static_cast<double>(kRate) - played);
  };

  struct Case {
    float ratio;
    const char* label;
  };

  static constexpr Case kCases[]{{1.0F, "1.0x"}, {1.5F, "1.5x"}, {2.0F, "2.0x"}, {4.0F, "4.0x"}};

  for (const Case& one : kCases) {
    const std::vector<float> rendered = render_at_two_tones(kLow, kAir, one.ratio, kFrames);
    const double fold = alias_of(static_cast<double>(one.ratio));

    // Referenced to the musical tone at its new position, which is what a
    // listener hears the rubbish underneath.
    const double reference =
        magnitude_of(rendered, static_cast<double>(kLow) * static_cast<double>(one.ratio));
    const double alias = fold > 0.0 ? magnitude_of(rendered, fold) : 0.0;
    const double floor_db =
        (alias > 0.0 && reference > 0.0) ? 20.0 * std::log10(alias / reference) : -200.0;

    WARN(one.label << ": air lands at "
                   << (static_cast<double>(kAir) * static_cast<double>(one.ratio))
                   << " Hz, folds to " << fold << " Hz, alias " << floor_db << " dB");

    if (fold == 0.0) {
      // BELOW 2x NOTHING FOLDS, which is the useful half of this result: a
      // chipmunk of a fifth or so is clean, and it is the octave-and-up that is
      // not.
      CHECK(one.ratio < 2.0F);
    } else {
      // AND THE ALIAS IS NOT ATTENUATED. Folding relocates energy, it does not
      // reduce it: both source tones are the same amplitude, and the folded one
      // comes back level with the music it is now sitting under. That is the
      // whole caveat in one number -- at 2x and beyond, a sample's air arrives
      // in the middle of the band at full strength.
      CHECK(floor_db == Catch::Approx(0.0).margin(1.0));
    }
  }
}
