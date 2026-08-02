#ifndef CRATEDIG_RT_BIQUAD_HPP
#define CRATEDIG_RT_BIQUAD_HPP

#include "rt/arch.hpp"

#include <span>

namespace rt {

// Normalised biquad coefficients. a0 has already been divided out, so the
// difference equation is exactly:
//
//   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
//
// NOTHING HERE COMPUTES THEM. Derivation needs sin, cos and sqrt, and
// docs/MIXER.md requires that coefficients are derived once on the control
// thread and published -- never recomputed per block. rt/eq.hpp does that.
//
// The default is passthrough, not silence: b0 = 1 with everything else zero
// leaves the signal exactly as it arrived. So a default-constructed band is
// bit-transparent AND bypassed, which are two independent reasons a fresh strip
// cannot colour the signal. Defaulting b0 to 0 would make a bug in the bypass
// check silent rather than obvious, which is the wrong way round.
struct BiquadCoeffs {
  float b0 = 1.0F;
  float b1 = 0.0F;
  float b2 = 0.0F;
  float a1 = 0.0F;
  float a2 = 0.0F;
};

// One Direct Form I biquad section: coefficients in, state here.
//
// DIRECT FORM I because its state is the input and output history rather than an
// internal accumulator, which is the form that behaves best when coefficients
// change between blocks -- and they do change, because the control thread
// republishes them whenever anybody touches a knob (docs/MIXER.md).
//
// The coefficients are NOT a member. State belongs to a strip and a channel and
// lives on the audio thread; coefficients belong to a PadConfig and arrive
// through the handoff ring. Storing them together would mean either copying
// coefficients into the state every block or letting the audio thread hold a
// pointer into a config it does not own.
//
// NOTE FOR ANY COMMITTED HASH: process() contains several a*b+c expressions that
// a compiler is free to contract into an FMA, and different compilers make
// different choices. An engaged EQ is therefore NOT bit-portable across
// toolchains -- see docs/TESTING.md, "When a golden hash may be committed". The
// e2e golden runs the transparent chain for exactly this reason.
class Biquad {
 public:
  [[nodiscard]] float process(const BiquadCoeffs& coeffs, float input) noexcept {
    const float output =
        flush_denormal((coeffs.b0 * input) + (coeffs.b1 * m_x1) + (coeffs.b2 * m_x2) -
                       (coeffs.a1 * m_y1) - (coeffs.a2 * m_y2));
    m_x2 = m_x1;
    m_x1 = input;
    m_y2 = m_y1;
    m_y1 = output;
    return output;
  }

  // In place, because the strip processes its own buffer.
  void process_block(const BiquadCoeffs& coeffs, std::span<float> samples) noexcept {
    for (float& sample : samples) {
      sample = process(coeffs, sample);
    }
  }

  void reset() noexcept {
    m_x1 = 0.0F;
    m_x2 = 0.0F;
    m_y1 = 0.0F;
    m_y2 = 0.0F;
  }

  // Whether every piece of state is exactly zero -- so a settled filter can be
  // told apart from one still ringing. For tests and for nothing else: the audio
  // path must not branch on it, because skipping a filter with a tail is how a
  // decay gets truncated.
  [[nodiscard]] bool settled() const noexcept {
    return m_x1 == 0.0F && m_x2 == 0.0F && m_y1 == 0.0F && m_y2 == 0.0F;
  }

 private:
  float m_x1 = 0.0F;
  float m_x2 = 0.0F;
  float m_y1 = 0.0F;
  float m_y2 = 0.0F;
};

}  // namespace rt

#endif  // CRATEDIG_RT_BIQUAD_HPP
