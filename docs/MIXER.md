# Mixer & DSP specification

Signal flow and the definition of every processor, precise enough to test against.

This file exists because M5's acceptance is *"EQ ±0.1 dB vs analytic"*, and a test cannot
compare a filter to an analytic response without being told which response. Everything here
is a decision the tests cite; if an implementation and this file disagree, one of them is a
bug and the same PR fixes both (CLAUDE.md).

**Scope.** M5 builds the graph, the strip essentials, the EQ, the compressor and the master
limiter. Saturation, sends, reverb and delay are M5.2 and are specified here only far enough
to say where they attach, so that the graph is not re-cut when they land.

## Signal flow

```
  pad 1..16          strip 1..16                bus a..d            master
  ─────────          ───────────                ────────            ──────
  voices ──► [ gain ► EQ ► comp ► balance ] ──► [ sum ► gain ] ──► [ sum ► gain ► limiter ] ──► out
                                    │                                  ▲
                          (M5.2: sends tap here) ──► [ reverb ]────────►│
                                                     [ delay  ]────────►│
                                                                        │
                                      metronome ────────────────────────┘
```

Sixteen strips, four buses, one master. The shape is **fixed**: no node is created or
destroyed while running, so there is no graph topology to allocate, branch on, or get wrong
at 3 a.m. Every buffer is preallocated at `Engine` construction against
`Config::max_block_frames`.

**Order within a strip is gain → EQ → compressor → balance, and it does not move.** Gain
first so the compressor sees what the fader is actually sending it; balance last so it cannot
change what the compressor detected. The mockup draws two free-form insert slots per strip;
this is fixed instead, and `docs/design/README.md` records the departure. Arbitrary
processors in arbitrary order is M8's per-pad plugin chain, which has a swap protocol this
does not need.

**The metronome enters at master, after the bus sum and before the limiter.** It is a
monitoring signal that belongs to the machine rather than to the music: routing it through a
strip would give it a fader, a mute and an EQ it has no business having, and putting it after
the limiter would let it be the one thing that can clip the output.

### Where the pad's own gain lives, and why it is not the fader

`PadConfig::gain` is multiplied into `voice.gain` **at trigger time** (`rt::VoicePool::trigger`),
along with the note's velocity. It is a property of the material — how loud this chop is —
and changing it deliberately does not disturb voices already sounding.

`StripConfig::gain` is the fader, applied per block on the strip. It moves what is sounding,
because that is what a fader is. Two numbers, two meanings, and the reason they are not one:
a mixer whose fader only affected the next hit would be broken in a way that is very hard to
describe and very easy to ship.

## Levels, units and conventions

- **Linear gain everywhere in the audio path.** dB appears only at the interface boundary and
  in this document. A dB field in an RT struct means a `pow()` on the audio thread or a
  conversion somebody forgets.
- **dBFS is `20·log10(|x|)`**, matching `tui::detail::format_dbfs`. 0 dBFS is a sample
  magnitude of 1.0. There is no headroom convention above that: the format is float, so 1.0
  is a reference rather than a ceiling, and only the limiter and the output device care.
- **Frequencies in Hz, times in frames.** Times are in frames for the reason
  `rt::AdsrFrames` gives: the device's block size must not be able to change the shape of
  anything, and a frame count cannot be affected by it.
- **Sample rate** is `Engine::Config::sample_rate`. Coefficients are derived from it once, on
  the control thread, and published — never recomputed per block.

### Transparency is a requirement, not a happy accident

**A default strip must be bit-transparent.** Gain 1.0, balance centre, EQ bypassed,
compressor bypassed, limiter disabled: every sample must reach the output exactly as the
voice produced it.

This is load-bearing. The M3 and M4 e2e goldens are committed hashes of the existing signal
path, and anything that multiplies by a not-quite-one number moves them for no musical
reason. Two specific traps it rules out:

- **Balance, not a constant-power pan law.** An equal-power law is `cos/sin` of the pan angle
  and gives 0.7071 on both sides at centre — 3 dB down. For a stereo source that is simply
  wrong, and it would have made the whole program quieter the day the mixer landed. Balance
  attenuates only the side being panned away from and is exactly 1.0 on both at centre.
- **The limiter is off by default.** Lookahead is a delay, and a delay moves every golden
  while changing nothing anybody asked to change.

**M5 moves no committed hash.** That was not the expectation going in, and the section below
is the measurement that replaced it.

### Summation is regrouped, and it turned out not to be visible

Before M5, every voice summed into one output buffer in voice-slot order. Now each pad's
voices sum into their own strip, strips into buses, buses into master. The same numbers,
added in a different order — and **float addition is not associative**, so in general the
result differs in the last bits. Over 2000 random six-voice sums, regrouping changed the
result in **993 of them (49.6%)**.

The plan for this milestone therefore budgeted a justified re-baseline of both M4 goldens.
**Neither moved**, and the reason is worth stating because it is a property to keep rather
than a coincidence to be relieved about:

- Adding into a buffer that starts at zero is **exact** — `0.0f + x == x` for every finite
  `x`, and `+0.0f + -0.0f` is `+0.0f`, so no signed zero appears either. Every node in the
  graph starts at zero, so the intermediate hops add nothing.
- The graph walks **pads in ascending order**; the flat mix walked **voice slots**, which are
  allocated lowest-free-first and therefore follow trigger order. When a session triggers in
  ascending pad order — which both e2e suites do, and which anything playing a pattern
  top-to-bottom does — the two orders coincide and the graph performs the *identical sequence
  of additions*.
- A bus with nothing routed to it contributes exactly `0.0f`, so summing all four costs
  nothing in accuracy. Verified rather than reasoned: reversing the bus walk produces
  bit-identical output.

Regrouping is still real when trigger order and pad order disagree. Measured on six voices
triggered in scrambled pad order: worst-case difference **1.19e-7**, which is one ULP at unity
amplitude, or −138 dBFS.

Both facts are pinned in `tests/unit/engine_render_test.cpp`:

| Test | Claim |
|---|---|
| the mixer graph sums the same numbers the flat mix did | scrambled order agrees within 1e-6 (−120 dBFS) |
| the graph is bit-exact when trigger order follows pad order | ascending order agrees **bit for bit** |

They catch different things, which is why both exist: reversing the strip walk fails the
second and passes the first. `rt::VoicePool::render_add()` — the pre-M5 signal path — is kept
solely as the reference these are measured against.

**Nothing in M5 may move a hash.** If one moves, that is the bug; do not re-baseline it.

## The strip

### Gain

Linear, applied to both channels. Default 1.0, range **[0, 4]** — silence to +12 dB, which is
past any mix decision and short of the range where one bad number makes the output painful.

**Out-of-range values fall back to the default, they are not clamped to the nearest bound.**
`std::clamp` would map an infinity to the ceiling and a NaN to whichever bound it compared
first, turning "this number is meaningless" into "play this as loud as the mixer allows". The
same rule applies to balance and to the bus index, and it is the discipline
`rt::VoicePool::step_for` already uses: these values crossed a thread boundary and the audio
thread does not get to abort on bad input.

### Balance

One control in [−1, +1], 0 at centre.

```
  left  gain = pan <= 0 ? 1 : 1 - pan
  right gain = pan >= 0 ? 1 : 1 + pan
```

Exactly 1.0 on both channels at centre; a hard pan silences the other side. Linear rather than
`cos/sin` for the transparency reason above — this is a balance control on a stereo signal,
not a panner placing a mono source in a field.

**A no-op at any channel count other than two.** Left and right is what balance means; there
is no honest reading of it for one channel or for five, and inventing one — "apply the left
gain to the even channels" — is a rule nobody asked for that silences half a mono strip the
moment it is panned. Placing a mono source in a stereo field is a different job and, if it is
ever wanted, a different control.

### Mute and solo

Mute is per strip. **Solo is a property of the set, not of a strip**: if any strip is soloed,
every strip that is not soloed is silent. It is derived per block from all sixteen rather
than stored, so there is no "solo count" to leak when a strip is reconfigured.

Solo overrides mute — a soloed strip that is also muted is audible, because solo is a
statement about what you want to hear now and mute is a statement about the mix.

## The buses

Four, named A–D, each summing the strips routed to it and passing the result to master at its
own gain. No processing of their own in M5 — M5.2's sends return alongside them.

**Every strip defaults to bus A** (`rt::kDefaultBus`), not "spread the sixteen pads across the
four". A mixer sends everything to the main mix until you decide otherwise, and grouping pads
is a musical judgement about one particular track: a default that guesses at drums/bass/music/
vocals is wrong for every track that disagrees with it, and silently so.

The other three buses are summed anyway, every block, whether anything reaches them or not.
The graph's shape is fixed (`rt::kNumBuses`), so there is no topology to branch on in the
callback, and — as measured above — an empty bus contributes exactly `0.0f` and costs nothing
in accuracy.

**The master is not a node.** It is the caller's output buffer, which `Engine::render()` has
already cleared and which everything sums into. One buffer fewer, one copy fewer, and the
offline bounce writes where the device would have.

## The EQ

Four bands per strip, each independently bypassable, in fixed order:

| Band | Type | Parameters |
|---|---|---|
| 1 | Low shelf | frequency, gain, S (shelf slope; S = 1 is the steepest without a peak) |
| 2 | Peaking | frequency, gain, Q |
| 3 | Peaking | frequency, gain, Q |
| 4 | High shelf | frequency, gain, S |

Each band is one **Direct Form I biquad**, coefficients from the RBJ Audio EQ Cookbook. Direct
Form I because its state is the input and output history rather than an internal accumulator,
which is the form that behaves best when coefficients change between blocks — and coefficients
do change, because the control thread republishes them.

### Coefficients

With `A = 10^(gain_db/40)`, `w0 = 2*pi*f0/Fs`, and `alpha` as given per type:

**Peaking**, `alpha = sin(w0)/(2Q)`:

```
  b0 = 1 + alpha*A     b1 = -2*cos(w0)     b2 = 1 - alpha*A
  a0 = 1 + alpha/A     a1 = -2*cos(w0)     a2 = 1 - alpha/A
```

**Low shelf**, `alpha = sin(w0)/2 * sqrt((A + 1/A)(1/S - 1) + 2)`:

```
  b0 =    A((A+1) - (A-1)cos(w0) + 2*sqrt(A)*alpha)
  b1 =  2A((A-1) - (A+1)cos(w0))
  b2 =    A((A+1) - (A-1)cos(w0) - 2*sqrt(A)*alpha)
  a0 =      (A+1) + (A-1)cos(w0) + 2*sqrt(A)*alpha
  a1 =   -2((A-1) + (A+1)cos(w0))
  a2 =      (A+1) + (A-1)cos(w0) - 2*sqrt(A)*alpha
```

**High shelf**, same `alpha`:

```
  b0 =    A((A+1) + (A-1)cos(w0) + 2*sqrt(A)*alpha)
  b1 = -2A((A-1) + (A+1)cos(w0))
  b2 =    A((A+1) + (A-1)cos(w0) - 2*sqrt(A)*alpha)
  a0 =      (A+1) - (A-1)cos(w0) + 2*sqrt(A)*alpha
  a1 =    2((A-1) - (A+1)cos(w0))
  a2 =      (A+1) - (A-1)cos(w0) - 2*sqrt(A)*alpha
```

All six are normalised by `a0` before use, so the difference equation is:

```
  y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
```

**Q is the classical definition** — bandwidth in octaves is *not* the parameter, and a caller
that wants octaves converts. **Gain is in dB and applies to the band's peak or shelf**, not to
the whole filter: `A` is `10^(dB/40)` rather than `10^(dB/20)` precisely because the amplitude
response reaches `A^2` at the peak.

### Ranges

Frequency is clamped to `[10 Hz, min(20000, 0.45*Fs)]`. The upper bound is below Nyquist on
purpose: RBJ coefficients degenerate as `w0` approaches pi, and a UI that allows 20 kHz at a
44.1 kHz rate is a UI that allows a filter which is no longer the filter it claims to be. Q is
clamped to `[0.1, 18]`, **S to `[0.1, 1]`**, gain to `[-24, +24] dB`.

Finite values out of range are clamped; **non-finite ones fall back to the default** rather
than to the nearest bound, for the reason the Gain section gives.

#### Why S stops at 1

This range said `[0.1, 2]` when it was first written, and that was a bug — the band table
above already said the right thing ("S = 1 is the steepest without a peak") and the two
disagreed.

The shelf `alpha` contains `sqrt((A + 1/A)(1/S - 1) + 2)`. For `S > 1` the `(1/S - 1)` term is
negative, and at high gain it drags the radicand below zero: **at +24 dB with S = 2 it is
−0.116**, so `sqrt` returns NaN, every coefficient is NaN, and the strip fills with NaN for the
rest of the session. That combination was *inside* the permitted range.

Clamping the radicand at zero keeps the arithmetic finite and was rejected: the result is not a
shelf. A "+24 dB" low shelf built that way measures **+69.8 dB at 100 Hz** with a −45 dB notch
above it, peaking at +47.4 dB overall. Finite is not the same as correct.

Capping S at 1 makes `(1/S - 1)` non-negative, so the radicand is at least 2 for **every** gain
and the degenerate case cannot arise at all. It also happens to be the boundary the band table
names: measured overshoot above the shelf plateau is 0.000 dB at S = 1, +0.116 dB at S = 1.2
and +1.383 dB at S = 2.

### How it is tested (the acceptance)

`|H(e^jw)|` is evaluated **directly from the same coefficients**:

```
  H(e^jw) = (b0 + b1*e^-jw + b2*e^-2jw) / (1 + a1*e^-jw + a2*e^-2jw)
```

and compared against the magnitude measured by driving a sine through the implementation.
**Within ±0.1 dB**, across the band, for each type.

**The measured amplitude is taken by correlation, not by the peak sample, and that is part of
the acceptance rather than an implementation detail of the test.** Project the steady-state
output onto `sin` and `cos` at the test frequency and take the magnitude:

```
  amplitude = 2/N * |sum( y[n]*cos(w*n) ) + j*sum( y[n]*sin(w*n) )|
```

The reason is measured, not assumed. Peak-of-samples underestimates whenever there are few
samples per cycle, because the sampling instants straddle the true peak rather than landing on
it — against a correct high shelf (8 kHz, +9 dB, S = 1, 48 kHz) it reads:

| Probe | peak-of-samples error | correlation error |
|---|---|---|
| 1 kHz | −0.009 dB | 0.000 dB |
| 8 kHz | **−0.151 dB** | 0.000 dB |
| 15 kHz | −0.021 dB | 0.000 dB |
| 18 kHz | **−0.153 dB** | 0.000 dB |
| 21 kHz | −0.032 dB | 0.000 dB |

A peak-based test would therefore fail the ±0.1 dB acceptance on a filter that is exactly
right, and the tempting repair — widening the tolerance — would be widening it to accommodate
the yardstick. `docs/TESTING.md` already names that failure for the interpolator: "a test that
has to allow slop is a test that has stopped pinning anything down."

`tests/unit/biquad_test.cpp` asserts both halves of this: the correlation is inside the budget
by two orders of magnitude *and* the peak is outside it, on the same filter, from the same
samples. So nobody can quietly simplify the measurement back to the obvious thing.

#### The settle time is derived, not chosen

The measurement must not begin until the transient has gone, and how long that takes depends on
the filter. The poles are the roots of `z^2 + a1*z + a2`; the transient decays as `r^n` where
`r` is the magnitude of the **slower** one.

Two traps, both paid for. A flat settle of 2000 frames is wrong by a factor of thirty for a
narrow low band — 120 Hz at Q = 8 with +24 dB needs **65363 frames**, 1.36 s. And
`r = sqrt(a2)`, true for a conjugate pair, is wrong whenever the discriminant is non-negative
and the poles are **real and split**: at 120 Hz, −24 dB, Q = 0.5 it gives 0.9393 and asks for
257 frames while the dominant pole is 0.9980 and needs 8039. In both cases the filter was
right and the measurement was reading a transient and calling it the response.

#### What this acceptance cannot check

It compares the measured response against `|H|` evaluated **from the same coefficients**, so it
judges whether the implementation realises its coefficients — and is blind to coefficients that
are self-consistent and wrong. Deriving `A` as `10^(dB/20)` instead of `10^(dB/40)` passes it
completely.

So there is a second test, and it is not optional: a band asked for +6 dB must *produce* +6 dB
where it acts — at `f0` for a peak, at DC for a low shelf, at Nyquist for a high shelf. Those
three points are exact rather than sampled, since `H(1) = (b0+b1+b2)/(1+a1+a2)` and
`H(-1) = (b0-b1+b2)/(1-a1+a2)`.

No FFT is involved either, and deliberately: an FFT brings its own windowing error to the
measurement, and the thing under test is the filter. Reference arithmetic is `double`, for the
same reason `tests/unit/interpolator_test.cpp` uses it.

Also asserted: **bypass is bit-exact passthrough** — not "within a tolerance", exactly equal,
because a bypassed band must not touch the sample at all. And a settled filter fed silence
decays to zero rather than being cut, which is what makes skipping silent strips a determinism
bug rather than an optimisation.

Bypass is a *real* bypass, not "set the gain to 0 dB". At `A = 1` the numerator and denominator
become the same polynomial, so `H(z)` is identically 1 in algebra — and not in floating point,
where the recursion still accumulates rounding the algebra says is zero. An enabled 0 dB band
is measurably not bit-transparent, and the committed hashes are safe only because a default
band is switched **off**.

### State, and where it lives

Filter state is **not** in `rt::StripConfig`. A published config is immutable and shared
between the pad and every voice holding it; filter history belongs to one strip in one engine.
The `rt::Biquad` instances live in `Engine`, indexed by pad, band and channel, allocated once.

A bypassed band has its state **reset** rather than frozen. Frozen state would make the first
block after re-enabling depend on how long ago the band was switched off — a click, and a
determinism bug: the same session played twice would differ if the toggles landed on different
blocks.

The property that catches a mistake here is **block-size invariance**, not a level comparison.
Pointing every strip at one shared set of filters looks like it would corrupt a neighbouring
strip and does not — the strips with no EQ reset the shared state on their way past, so the
filtered strip merely restarts from silence at the top of each block, and two renders that are
equally wrong compare equal. What it destroys is history across block boundaries, which is
exactly what rendering the same material at 2048 frames and at 64 will catch.

### Denormals

The biquad flushes state below `1e-30` (−600 dBFS) to zero **itself**, rather than relying on
the FTZ/DAZ that `rt::ScopedDenormalDisable` sets. The offline renderer never opens a device
and so never sets them: leaving it to the FPU mode would make a live render and a bounce of the
same material disagree in the far tail, and "same input, same bytes" is a promise this project
makes. NaN is deliberately *not* flushed — a NaN in the audio is a bug to find, not one to hide
by turning it into silence.

## The compressor

One per strip, feed-forward, **peak-detecting**, operating on the maximum of the two channels
so the stereo image cannot be pulled sideways by gain reduction.

### Detector

```
  d[n]   = max(|left[n]|, |right[n]|)
  env[n] = d[n] > env[n-1] ? attack_coeff *(env[n-1] - d[n]) + d[n]
                           : release_coeff*(env[n-1] - d[n]) + d[n]
```

with `coeff = exp(-1 / time_frames)`. That is the standard one-pole; the consequence worth
stating is that **the envelope reaches 1 - 1/e (about 63.2%) of a step after exactly
`time_frames` frames**, and that is what the time-constant test measures. "Attack time" means
that here, and not "time to full gain reduction" — a definition that varies between
manufacturers and cannot be tested against.

Peak rather than RMS: this is a drum machine, and an RMS detector on a kick transient is late
by design. RMS is not offered, rather than offered and wrong.

The detector reads the **maximum across the channels** and one gain is applied to all of them.
Two independent detectors would apply two different gains, which moves a hard-panned hit toward
the centre every time it fires. A test for this has to put the loud material on **each** channel
in turn: against a signal loudest on channel 0, "the maximum of the channels" and "channel 0"
are the same number, and a detector reading only the first channel passes.

#### The envelope never quite arrives, and that is arithmetic

A one-pole in `float` stalls short of its target: once the per-step change falls below half an
ULP, `coeff * (env - d) + d` rounds back to `env` and the iteration stops. The residual is
about `eps / (1 - coeff)`, so it **grows with the time constant**:

| `time_frames` | residual `1 - env` |
|---|---|
| 10 | 3.0e-7 |
| 48 | 1.4e-6 |
| 240 | 7.2e-6 |
| 4800 | 1.4e-4 |
| 48000 | 1.4e-3 |

Inaudible for a gain-control signal, and worth writing down because a test that asserts the
envelope reaches its target to some fixed tolerance is asserting something `float` cannot do
for the slower settings. The time-constant test measures the **fraction traversed** from the
level actually reached, not the absolute value.

### Curve

In dB, with `x` the detector level, `T` the threshold, `R` the ratio and `W` the knee width:

```
  x - T < -W/2      ->  y = x                                       (below the knee)
  |x - T| <= W/2    ->  y = x + (1/R - 1)*(x - T + W/2)^2 / (2W)    (in the knee)
  x - T > W/2       ->  y = T + (x - T)/R                           (above the knee)
```

The gain applied is `10^((y - x)/20)`, multiplied by the makeup gain.

The quadratic knee is **continuous in value and in first derivative** at both boundaries,
which is the property the test checks — a knee that is merely continuous still produces an
audible edge, and the derivative is where that lives. The slope interpolates linearly from 1 at
the bottom of the knee to `1/R` at the top; at the centre it is exactly `(1 + 1/R)/2`.

Ratio is clamped to `[1, 20]`, threshold to `[-60, 0] dB`, knee to `[0, 24] dB`, makeup to
`[0, +24] dB`. Out-of-range handling is the same everywhere in this file: finite values clamp,
non-finite ones fall back to the default. A ratio of infinity becomes 1, not 20.

#### The implementation computes reduction, not output level

The curve above is written as an output level because that is how it reads. The code computes
`y - x` **directly**, which is algebraically the same and numerically not:

```
  above the knee:  y - x  ==  -(x - T)(1 - 1/R)
```

Two reasons, one of them a bug that was caught by its own test:

- **`R = 1` is only exactly unity in the reduction form.** This section used to claim that
  `R = 1` is unity at every level, and therefore that the enabled flag is an optimisation
  rather than a second code path. That is true in algebra and false in float: `T + (x - T)`
  does not round back to `x`, and a ratio of 1 measured **0.99999976**. Written as a reduction
  the `(1 - 1/R)` factor is exactly zero, the expression vanishes, and the claim holds for
  every threshold and every knee width.
- Differencing `y` and `x` cancels catastrophically exactly when reduction is small, which is
  when the answer most needs its precision.

A reduction of exactly zero returns unity without calling `pow` at all.

## The master limiter

**Off by default** (see transparency above). When enabled: a brick wall at a configurable
ceiling, default -0.3 dBFS.

Same detector and one-pole release as the compressor, with the ratio fixed at infinity — the
output never exceeds the ceiling — and a **lookahead** of `lookahead_frames`, default 64
(about 1.3 ms at 48 kHz). The lookahead is a delay line on the audio and an equal delay on
nothing else: the detector reads ahead, so gain reduction is already applied when the peak
arrives rather than after it.

That delay is why this is off by default. It is inaudible and it is still a delay, and a delay
that is always on would move every committed hash in the project for a feature nobody asked to
enable.

Tested as: with the limiter engaged, **no output sample exceeds the ceiling** for any input,
including a full-scale step — the case a feed-forward limiter without lookahead fails.
Disengaged, the path is bit-exact.

## Metering

Per strip, per bus and at master, published through `engine::Telemetry` with the same
discipline as the existing meters: written by the audio thread, read by the interface, and
explicitly **not part of the determinism contract** — the fall rate depends on the block size,
which depends on the device, while `render()`'s output does not.

Peak with a fall time of `Engine::kPeakFallSeconds` (0.4 s), reusing the existing constant so
every meter in the program falls at the same rate.

**Gain reduction is metered separately**, as a linear gain rather than a dB value, converted at
the boundary like everything else. Three details it is easy to get wrong:

- **No fall is applied to it.** It is not a peak that needs holding; it is the gain the
  compressor is applying now, and a decaying gain-reduction meter shows reduction that has
  already stopped.
- **The makeup gain is divided back out.** A compressor pulling 6 dB down with 6 dB of makeup
  is doing something, and a meter reporting the net would say it was not.
- **Unity means none, so it is initialised to 1.0** — including the published copy the
  interface reads, which nothing writes until the first block renders. Left at the
  default-constructed zero, an engine that has not rendered yet reports every strip crushed
  flat.

**Strip peaks are post-fader, and read zero when the strip is muted or soloed out.** That is
not the same question `Telemetry::pad_peak` answers, and both are wanted:

| Meter | Question | Muted pad, hit hard |
|---|---|---|
| `pad_peak` | did this pad *play*? | non-zero — the grid acknowledges the hit |
| `strip_peak` | is this strip *reaching the mix*? | zero — it is not |

The strip meter sits beside the fader on the MIX screen, where "muted but showing level" would
be a lie about where the sound is going. The pad meter sits on the grid, where a hit that
lights nothing looks like a dropped trigger.

Strip peaks measure **every channel**, unlike `rt::Voice::peak`, which measures channel 0 only
to keep the innermost loop cheap. A strip panned hard right must not read as silent.

## M5.2, and where it attaches

Specified only far enough that the graph does not get re-cut:

- **Saturation** is a strip processor, after the compressor and before balance, oversampled to
  keep its harmonics below Nyquist. The alias-floor acceptance belongs to it.
- **Sends** tap the strip post-fader, sum into two send buses, and return to master alongside
  the four mix buses.
- **Reverb and delay** live on those send buses.

None of that changes the shape above: it adds processors to a strip that already has a chain,
and buses to a master that already sums several.
