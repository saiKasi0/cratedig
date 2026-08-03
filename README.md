# CRATEDIG

the terminal crate-digging DAW — pull samples in, chop them, play chops on pads,
sequence, mix, export. keep it CLI smooth.

**Status: pre-alpha (M5.5 — the crate).** Loads files, draws them, finds the
transients, cuts them into slices, lays them across sixteen pads and plays them;
records patterns and chains them into a song; mixes the result through sixteen
channel strips with four-band EQ, a compressor, four buses and a master limiter;
and holds several files at once, so one bank can mix a break, a vocal take and a
one-shot.

**Not yet:** recording your own audio (M6 — see *What is not built* below),
export, and yt-dlp import.

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
| `space` | play the slice — through its pad if it has one, auditioned if it has not |
| `z` | zoom in; once there is no more to see, reframe the slice |
| `u` | undo the last nudge |
| `esc` | back to PERFORM |

A chop of more than sixteen leaves the rest on no pad. They are still editable,
drawable and now **playable**: `space` auditions them on their own voices, so a
preview cannot light a pad or be silenced by one's mute.

## Commands

| | |
|---|---|
| `:chop transient` | cut at detected onsets |
| `:chop grid N` | cut into N equal pieces |
| `:chop reset` | back to the whole file on pad 1 |
| `:slot assign S P` | put slice S on pad P; either may be a range (`1-8 1`) |
| `:pad gate [N]` | pad N (or all of them) sustaings while held |
| `:pad oneshot [N]` | pad N (or all of them) plays to the end of its slice |
| `:edit [S]` | open EDIT, on slice S or on the current one |
| `:env P a`/`d`/`s`/`r` `V` | pad P's envelope: attack, decay and release in ms, sustain in dB |
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
| `:swing N` | shift the odd steps late by N% of a step, up to 75 |
| `:pattern N` | select one of the sixteen patterns |
| `:pattern length N` | 1–32 steps |
| `:pattern clear` | erase its steps, keeping length and swing |
| `:song 1 2 3` | chain patterns in play order |
| `:song clear` | back to one pattern repeating |
| `:metro [on\|off]` | metronome; bare `metro` flips it |
| `:stop [N]` | stop pad N, or everything (and the transport) |
| `:q` `:quit` | quit |

`slice` is accepted anywhere `chop` is.

**Tab completes.** At the `:` prompt it offers every command that starts with
what you have typed, with a line each saying what it does; Tab and Shift-Tab (or
the arrows) move through them and the line always shows the selection, so Enter
runs what you can see. After `:load` or `:browse` it completes paths instead,
offering directories and audio files from wherever you have got to. Esc closes
the menu; Esc again closes the prompt.

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
| **Recording your own audio** | M6, and it goes first in that milestone. A sampler that cannot sample is a player. Today material comes from a file on disk; `:load` is the only way in. |
| **Export / bounce** | M6. `Engine::render()` already works with no device, which is what an offline bounce needs, but nothing writes a file yet. |
| **yt-dlp import** | M6, using a yt-dlp you install yourself (see `docs/LICENSING.md`). |
| **Banks and a 32-pad mode** | M6. Sixteen pads at a time; `:slot assign 17-32 1` reaches further slices in one line. |
| **Saturation, sends, reverb, delay** | M5.2. The mixer's graph already has the attachment points, which is why they can land without re-cutting it. |
| **Configurable transient detection** | M5.7. `:chop transient` runs at one fixed threshold today; the profiles that let you say what counts as a hit come next. |
| **Varispeed and BPM detection** | M5.7. The chipmunk/screwed knob, and a tempo read off the sample rather than typed. |
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
