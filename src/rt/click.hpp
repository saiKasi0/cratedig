#ifndef CRATEDIG_RT_CLICK_HPP
#define CRATEDIG_RT_CLICK_HPP

#include <array>
#include <cstddef>

namespace rt {

// The metronome click, as a compile-time table.
//
// CLAUDE.md: "constexpr tables for windows, sinc kernels, dB curves. No runtime
// table builds." A metronome is exactly that -- two short fixed sounds that
// never change -- so it is built by the compiler and read at playback.
//
// WHY THIS IS NOT A VOICE, AND NOT A PAD
// --------------------------------------
// The obvious implementation is a PadConfig triggered into the voice pool, and
// it is wrong on three counts. A click would take a pad index it has no business
// owning, a glow slot that would light a pad nobody pressed, and a telemetry
// entry describing something that is not a pad. Worse, a dense pattern could
// STEAL the click's voice -- the metronome going quiet exactly when the music
// gets busy is the opposite of what a metronome is for.
//
// So the click is mixed straight into the output buffer from the table below.
// It costs one multiply-add per frame while it sounds and interacts with nothing.
//
// WHY THERE IS TRIGONOMETRY HERE AND NOT AN #include
// ---------------------------------------------------
// src/ingest/window.hpp already has a constexpr cosine, and src/rt/ may not
// depend on src/ingest/ (the module map in CLAUDE.md: src/rt/ includes nothing
// outside itself). Copying a general-purpose cosine in would be the wrong fix
// for a file that needs exactly two angles.
//
// Instead the table is built by a two-pole recurrence -- s[n] = 2cos(w)s[n-1] -
// s[n-2], which generates sin(n*w) exactly -- seeded with cos(w) and sin(w) as
// literals. Two constants instead of an algorithm, and click_test.cpp pins both
// against std::cos and std::sin so a typo in the tenth decimal cannot survive.

// 25 ms at 48 kHz. Long enough to hear as a tone rather than a tick, short
// enough that it never runs into the next beat -- see the invariant note below.
inline constexpr std::size_t kClickFrames = 1'200;

// 4/4. The bar line is where the accent goes.
inline constexpr std::uint64_t kBeatsPerBar = 4;

// THE INVARIANT that keeps the mixer simple: a click is always shorter than a
// beat, so at most one click is ever sounding. At the clamped maximum of 300 bpm
// a beat is 9600 frames at 48 kHz against this table's 1200 -- eight times the
// margin. The mixer finds the earliest beat whose click could still be sounding
// rather than assuming this, so an absurd sample rate degrades to overlapping
// clicks rather than to a missing one.

struct ClickTable {
  std::array<float, kClickFrames> samples{};
};

// A decaying sine, built at compile time.
//
// The oscillator stays pure and the envelope is applied on the way out: folding
// the decay into the recurrence would change its pole and detune the tone as it
// faded, which is audible as a downward chirp on every beat.
//
// Computed in double and stored as float. It is constexpr, so the wider
// arithmetic is free, and 1200 iterations of a marginally-stable recurrence is
// long enough for float error to be visible in the tail.
[[nodiscard]] constexpr ClickTable make_click(double cos_w, double sin_w, double decay) noexcept {
  ClickTable table{};
  double previous = 0.0;  // sin(0)
  double current = sin_w;
  double envelope = 1.0;

  for (std::size_t n = 0; n < kClickFrames; ++n) {
    // A linear taper alongside the exponential, so the last sample is EXACTLY
    // zero. An exponential alone leaves a small step at the end of the table,
    // and a step is a click -- which on a metronome is a click on a click.
    // Denominator is kClickFrames - 1, so the taper reaches zero ON the last
    // index rather than one past it. With kClickFrames the final sample came out
    // at about -1.3e-7 instead of 0 -- inaudible, but the comment above claims
    // exactness and a claim that is nearly true is worse than one that is not
    // made.
    const double taper =
        static_cast<double>(kClickFrames - 1 - n) / static_cast<double>(kClickFrames - 1);
    table.samples[n] = static_cast<float>(previous * envelope * taper);

    const double next = (2.0 * cos_w * current) - previous;
    previous = current;
    current = next;
    envelope *= decay;
  }
  return table;
}

// cos and sin of 2*pi*f/48000, as literals. Pinned against std::cos/std::sin in
// click_test.cpp -- the test is what makes hard-coding them safe.
inline constexpr double kAccentCos = 0.9781476007338057;  // 1600 Hz
inline constexpr double kAccentSin = 0.20791169081775934;
inline constexpr double kBeatCos = 0.9914448613738104;  // 1000 Hz
inline constexpr double kBeatSin = 0.13052619222005157;
inline constexpr double kClickDecay = 0.994;

// The table is generated for 48 kHz and played back frame for frame, so at
// 44.1 kHz the click is about 8% lower. That is deliberate: resampling it would
// mean either a runtime table build or an interpolator in the metronome, and a
// tick being a tone lower is not a thing anyone can hear as wrong.
inline constexpr ClickTable kClickAccent = make_click(kAccentCos, kAccentSin, kClickDecay);
inline constexpr ClickTable kClickBeat = make_click(kBeatCos, kBeatSin, kClickDecay);

// Loud enough to hear over a pattern, quiet enough not to be the loudest thing
// in the mix. The accent carries more because it is doing more work -- it is
// the only thing telling you where the bar is.
inline constexpr float kAccentGain = 0.50F;
inline constexpr float kBeatGain = 0.32F;

}  // namespace rt

#endif  // CRATEDIG_RT_CLICK_HPP
