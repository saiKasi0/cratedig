# CRATEDIG

the terminal crate-digging DAW — pull samples in, chop them, play chops on pads,
sequence, mix, export. keep it CLI smooth.

**Status: pre-alpha (M5.5 complete — the crate).** Loads files, draws them, finds
the transients, cuts them into slices, lays them across sixteen pads and plays
them; records patterns and chains them into a song; mixes the result through
sixteen channel strips with four-band EQ, a compressor, four buses and a master
limiter; and holds several files at once, so one bank can mix a break, a vocal
take and a one-shot.

Browse a folder, hear a file before you load it, see its waveform while it plays,
mark a bit of it and put that bit on a pad. Tab completes commands and paths.

Play a pattern in rather than typing it: `R` arms, and what you hit on the pads
while the transport rolls lands in the pattern, snapped as hard or as softly as
you ask. Resample yourself with `:capture` — the take arrives in the crate as a
file like any other, ready to chop and put back on a pad.

**`rec` and `capture` are different things.** One plays a pattern in; the other
records sound.

**Not yet:** recording from an audio *input* (M6 — see *What is not built*
below), export, and yt-dlp import.

## Quick start

```bash
scripts/setup.sh                  # toolchain: cmake, ninja, clang-format/tidy, pre-commit
cmake --preset dev
cmake --build --preset dev -j
ctest --preset dev

scripts/fetch_starter_pack.sh     # CC0 starter sounds (needed for the fixture tests)
build/dev/src/cratedig assets/starter-pack/loop_industrial.flac
```

Then, in the program:

```
:chop transient        find the hits and put one on each pad
1234 qwer asdf zxcv    play them
Enter                  open EDIT on the pad you just hit
hl / HL                nudge the slice start / end, one frame at a time
u                      undo a nudge
esc                    back to PERFORM, then esc again to quit
```

That is the whole loop: **import → chop → play**. Everything else is in service
of it.

Then make it play itself:

```
t                      bring up the pattern lane, and write the selected pad
[ ]                    move the step cursor
space                  play / stop
:bpm 92.5              set the tempo
:song 1 2              chain pattern 1 into pattern 2
```

Then bring in more records:

```
:load ~/breaks/amen.wav   add a file — it does not replace what is loaded
:files                    the crate, with a * on the one you are looking at
:file 2                   look at another one
:unload                   drop the one showing; pads holding it keep playing
```

Then mix it:

```
:mix                   open the mixer
[ ]                    page: channels 1-8, 9-16, buses a-d
h l · j k              pick a strip · ride its fader
m · s · b              mute · solo · send it to the next bus
:gain 3 -6             or say it outright
:eq 3 2 800 -4 1.2     band 2 of pad 3: 800 Hz, -4 dB, Q 1.2
:comp 3 -18 4          threshold -18 dB, 4:1
:limit on              a brick wall on the master, off by default
```

### If you have no sound card

`--no-audio` runs the interface with no device open. The waveform, the chopping,
writing patterns and the whole interface work; nothing is audible, and the mode
line says so. The transport is the one thing that genuinely cannot run — nothing
renders, so there is nothing to advance — and pressing play tells you that
rather than pretending.

## Keys

**PERFORM**

| | |
|---|---|
| `1234` `qwer` `asdf` `zxcv` | pads 1–16, in reading order |
| `space` | transport play / stop, always from the top (`p` too) |
| `:` | command line |
| `Enter` | open EDIT on the selected pad's slice |
| `[` `]` · `t` | move the step cursor · write the selected pad onto that step |
| `h` `l` · `H` `L` | scroll the view · scroll by a full screen |
| `+` `-` · `0` | zoom · fit the whole file |
| `g` `G` | jump to the start · the end |
| `\` | switch the right-hand panel |
| `h` `l` | scroll — only once `+` has zoomed in; the whole file is on screen by default |
| `R` | arm recording — play the pattern in instead of typing it |
| `.` | stop every sounding voice, and the transport |
| `esc` | quit |

`[`, `]` and `t` bring the pattern lane up rather than editing a grid you cannot
see. A step is written to whichever pad you last hit, so the way to change rows
is to play the pad.

The keys run in pad order with the number row on top, where it is on the
keyboard. `f` is pad 12, so "fit" is `0` — the one digit the map does not claim.
`q` is pad 5, so quit is `esc` (and `:q`, for the muscle memory that expects it).

**EDIT**

| | |
|---|---|
| `[` `]` | previous / next slice |
| `h` `l` | nudge the slice **start** back / forward one frame — the readout shows the frame number, so a single press is visible |
| `H` `L` | nudge the slice **end** |
| `space` | play the slice — through its pad if it has one, auditioned if it has not; press again to stop |
| `z` | zoom in; once there is no more to see, reframe the slice |
| `-` `=` | pitch the slice's pad down / up a semitone |
| `r` | play the slice's pad backwards |
| `u` | undo the last nudge |
| `esc` | back to PERFORM |

A chop of more than sixteen leaves the rest on no pad. They are still editable,
drawable and now **playable**: `space` auditions them on their own voices, so a
preview cannot light a pad or be silenced by one's mute.

**BROWSE** — `:browse`, or `:browse ~/Music`

| | |
|---|---|
| `j` `k` | move through the listing |
| `l` `Enter` | open a directory · load a file into the crate |
| `h` | up a directory — landing on the one you came out of |
| `space` | hear the file under the cursor, and **press again to stop it** |
| `i` `o` | mark the start / end of a region, at the playhead |
| `[` `]` | trim the region's end, a sixteenth of the file a press |
| `-` | forget the region |
| a pad key | put the marked region on that pad |
| `E` | load the file and open EDIT on it |
| `esc` | back to PERFORM |

Previewing draws the file underneath the listing with a marker showing where the
sound has got to, so you can see the shape of what you are hearing.

**Taking a piece rather than the whole record.** Mark a region with `i` and `o`,
then press a pad key: that region goes on that pad, and the file joins the crate
carrying **one** cut instead of sixteen. The audio was already decoded to preview
it, so nothing is saved by keeping the file out of the crate — and a pad naming a
file the crate does not have could not be drawn, listed or saved. What you avoid
is the chopping.

The pads are otherwise off in BROWSE — a browser that played a drum every time
you moved down a listing would be unusable — which is what frees those sixteen
keys to mean "put it here". `E` is capital because `e` is pad 7.

## Commands

| | |
|---|---|
| `:chop transient [strum\|beat\|bar]` | cut at detected onsets, as fine as you ask |
| `:chop tune` | open CHOP and adjust the chop while watching it change |
| `:chop grid N` | cut into N equal pieces |
| `:chop reset` | back to the whole file on pad 1 |
| `:slot assign S P` | put slice S on pad P; either may be a range (`1-8 1`) |
| `:pad gate [N]` | pad N (or all of them) sustaings while held |
| `:pad oneshot [N]` | pad N (or all of them) plays to the end of its slice |
| `:edit [S]` | open EDIT, on slice S or on the current one |
| `:env P a`/`d`/`s`/`r` `V` | pad P's envelope: attack, decay and release in ms, sustain in dB |
| `:pitch P V` | play pad P faster or slower — `+7` is semitones, `1.5` is a ratio |
| `:reverse P [on\|off]` | play pad P's slice backwards; bare flips it |
| `:load <path>` | add a file to the crate — the rest of the line is the path, spaces and all |
| `:files` · `:pool` | list the crate |
| `:file N` | look at another loaded file |
| `:unload [N]` | drop a file; pads holding it keep playing |
| `:browse [dir]` | open BROWSE, on a directory or where the last load came from |
| `:mix [buses]` | open MIX, on the channels or on the bus page |
| `:gain P N` | strip P's fader, in dB (`:gain bus 1 -3` for a bus) |
| `:pan P N` | place strip P, −100 (left) to 100 (right) |
| `:mute P [on\|off]` · `:solo P [on\|off]` | bare form flips it |
| `:bus P a`–`d` | route strip P to a bus |
| `:eq P B F G S` | band B of strip P: frequency, gain in dB, shape; `:eq P B off` bypasses |
| `:comp P T R A L` | strip P's compressor: threshold, ratio, attack, release; `:comp P off` |
| `:limit [on\|off] [dB]` | the master limiter and its ceiling |
| `:perform` | back to PERFORM |
| `:bpm N` | tempo, 20–300, one or two decimals (`:bpm 92.5`) |
| `:bpm detect` | read the tempo off the file that is showing |
| `:tape R` | scale the tempo — `1.05` a nudge faster, `2` double time |
| `:swing N` | shift the odd steps late by N% of a step, up to 75 |
| `:pattern N` | select one of the sixteen patterns |
| `:pattern length N` | 1–32 steps |
| `:pattern clear` | erase its steps, keeping length and swing |
| `:song 1 2 3` | chain patterns in play order |
| `:song clear` | back to one pattern repeating |
| `:metro [on\|off]` | metronome; bare `metro` flips it |
| `:rec [on\|off]` | arm recording; bare `rec` flips it, and so does `R` |
| `:rec quant N` | how hard to snap what you play: 16, 8, 4, 2 or 1 |
| `:rec replace` · `:rec overdub` | a take clears the pattern first · a take adds to it |
| `:rec undo` | put the pattern back as it was before the take |
| `:capture [start\|stop]` | record audio; bare flips it. Stopping puts the take in the crate |
| `:capture source master\|input` | what to record — the mix you hear, or an input |
| `:capture arm [dB]` | start by itself when the sound arrives (-24 dB by default) |
| `:capture drop` | end a take and throw it away |
| `:stop [N]` | stop pad N, or everything (and the transport) |
| `:q` `:quit` | quit |

`slice` is accepted anywhere `chop` is.

**Tuning it by eye.** `:chop tune` opens a screen showing the waveform with every
cut marked, the detection function underneath with the threshold drawn on it, and
sensitivity, minimum gap and low cut as knobs. The slice count and the cuts move
as you turn them. `enter` applies the chop, `esc` leaves the file alone. It is
worth opening when a chop comes out wrong, because the detection curve says *why*:
a peak below the threshold line is a hit that was not taken, and a stretch of line
sitting over every peak is a passage the detector has given up on.

**How fine a chop.** `:chop transient` cuts at every attack it can find — on a
fast riff that is roughly one slice per strum. `beat` and `bar` cut less often,
and they mean it musically: they are derived from the tempo on the transport, so
set `:bpm` first (tempo detection is M5.7's other half). Measured on an
18-second, 172 BPM rock excerpt: `strum` gives 92 slices, `beat` 48, `bar` 13 —
against 8ths, beats and bars at 5.7, 2.9 and 0.72 a second.

**Tab completes.** At the `:` prompt it offers every command that starts with
what you have typed, with a line each saying what it does; Tab and Shift-Tab (or
the arrows) move through them and the line always shows the selection, so Enter
runs what you can see. After `:load` or `:browse` it completes paths instead,
offering directories and audio files from wherever you have got to. Esc closes
the menu; Esc again closes the prompt.

**Tempo detection is a starting point, not an oracle.** `:bpm detect` reads the
periodicity out of the onset detection function. The number it gives is a *real*
period of the material — but which metrical level you meant is not something the
signal decides: a pulse train at 172 BPM repeats exactly at 86, at 57 and at 344,
and all of those are correct. It reports the one nearest where most music sits
and says so with a `?` when it is unsure, so double or halve it if it picked the
wrong level. Measured: a clean loop scores 0.96–0.99 confidence, eighteen seconds
of real rock scores 0.34.

**`:tape` changes the tempo, not the pitch**, and is named to say so. It scales
the sequencer's BPM: the pattern runs faster and nothing already sounding bends.
True master varispeed would mean a resampler in the audio callback, which would
move every committed hash in the project for a feature nobody asked to enable.
Per-pad `:pitch` is the one that actually retunes.

**Pitching up aliases, and that is worth knowing before you reach for it.**
Playing a sample `r` times faster folds everything above `sample_rate / (2r)`
back down into the audible band, and the interpolator cannot prevent it — a
4-point Hermite interpolates, it does not decimate. Measured, with a 15 kHz
component in the source: at 1.5× nothing folds (it lands at 22.5 kHz, still under
Nyquist); at 2× it folds to 18 kHz and at 4× to 12 kHz, **both at full level**.
Folding relocates energy rather than reducing it, so the artefact arrives as loud
as the music it lands under. Down is clean at any speed. A proper fix — a
decimating path, or a bounced pitch through libsamplerate — is on the roadmap;
until then, a fifth up is fine and an octave up is an effect.

**MIDI.** A controller plugged in before start-up plays the pads at real
velocity: note 36 is pad 1 (the General MIDI bass drum, which is what essentially
every pad controller ships mapped to its first pad), and the fifteen notes above
it take the rest of the grid. Note-on, note-off and velocity — no clock sync and
no CC mapping yet. A note-on at velocity 0 is treated as a note-off, because a
large share of controllers send it that way.

**Held pads need a terminal that can report key release.** cratedig asks yours
(the Kitty keyboard protocol) at start-up and uses it only if it answers; in a
terminal that does not, `:pad gate` pads behave as one-shots rather than sticking
on. `--legacy-keys` never asks.

## Command line

```
cratedig [file]
  --no-audio            run the interface without opening an audio device
  --sample-rate HZ      engine sample rate (default 48000)
  --block N             requested callback block size in frames
  --device ID           output device (0 = system default)
  --legacy-keys         skip the keyboard-protocol negotiation
  --list-devices        list output devices and exit
  --version             version, FFmpeg build and license, then exit
```

## What is not built

Written down because the difference between "not yet" and "broken" is the whole
point of a status section.

| | |
|---|---|
| **Recording from an input** | M6. `:capture source master` works — resampling what the sampler itself plays. `input` needs a duplex stream in `src/io/`, which is not built, so it captures nothing today. |
| **Export / bounce** | M6. `Engine::render()` already works with no device, which is what an offline bounce needs, but nothing writes a file yet. |
| **yt-dlp import** | M6, using a yt-dlp you install yourself (see `docs/LICENSING.md`). |
| **Banks and a 32-pad mode** | M6. Sixteen pads at a time; `:slot assign 17-32 1` reaches further slices in one line. |
| **Saturation, sends, reverb, delay** | M5.2. The mixer's graph already has the attachment points, which is why they can land without re-cutting it. |
| **Unquantised playback** | A later milestone, by decision. A take snaps to the grid because `rt::Step` has no micro-timing; giving a step a timing offset changes the struct the project file serialises and the sequencer's step scan, so it is a milestone rather than a knob. |
| **Lua config and scripting** | M7. |

## Building and testing

```bash
cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev
```

Swap `dev` for `asan`, `tsan` or `ubsan` to run under a sanitizer. TSan is the
authority on anything under `src/rt/`.

```bash
ctest --preset dev -L e2e        # the M3 acceptance: import, chop, play, bit-exact
ctest --preset dev -L tui        # offscreen layout snapshots + PTY-driven sessions
ctest --preset dev -L fixture    # needs scripts/fetch_starter_pack.sh
pre-commit run -a                # clang-format and the file hygiene hooks
docker compose -f docker/compose.yml run --rm ci    # full Linux CI locally
```

Fixture and device tests skip loudly when what they need is absent — no test
that matters depends on a fetched file or a sound card.

## Docs

- `CLAUDE.md` — the rules this codebase is built under (start here to contribute)
- `docs/ROADMAP.md` — milestone order; the build order is the law
- `docs/ARCHITECTURE.md` — threads, data flow, module map
- `docs/TESTING.md` — test strategy, determinism and golden policy
- `docs/LICENSING.md` — **read before touching plugins, FFmpeg, yt-dlp or audio**

## License

Apache-2.0 (see `LICENSE`, `NOTICE`). No GPL code is linked — notably **not**
VST3. Bundled audio is CC0-1.0 with provena nce recorded in
`assets/starter-pack/MANIFEST.toml` and verified in CI. See `docs/LICENSING.md`.
