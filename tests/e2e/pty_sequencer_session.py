#!/usr/bin/env python3
"""The M4 acceptance, typed into the real binary: write a pattern, chain it, play it.

docs/ROADMAP.md's M4 acceptance is about audio, and tests/e2e/sequencer_e2e_test.cpp
proves that half bit for bit with no terminal in the way. This is the other half:
that a person sitting in front of the program can actually RECORD the pattern the
other test renders -- that the keys and the `:` verbs reach the engine's sequencer
rather than only a copy of it in the interface.

WHAT THIS ADDS OVER tests/tui/pty_session.py
--------------------------------------------
That session touches the sequencer as one part of a tour of the interface. This
one is only about the sequencer, and asserts the thing that session cannot: that
what the lane draws came back out of the ENGINE. Every check below reads a value
that was published through the handoff ring and read back from
`Engine::sequencer_state()`, so a UI that drew its own local copy and never
published would fail here and pass there.

WHY THE TRANSPORT IS NOT DRIVEN HERE
------------------------------------
`--no-audio` opens no device, so Engine::render() is never called, so the
transport cannot advance and no telemetry is ever published. Pressing play is
covered -- the program has to SAY that, and this checks it does -- but "the
pattern sounded" is asserted where the audio exists, in sequencer_e2e_test.cpp.
A check for a moving playhead here would either fail or, worse, pass for the
wrong reason.

Usage: pty_sequencer_session.py <cratedig-binary>
"""

from __future__ import annotations

import fcntl
import os
import pty
import re
import shutil
import struct
import sys
import tempfile
import termios
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tui"))

# The terminal replay and the reader come from the TUI session, as
# pty_chop_session.py and pty_kitty_session.py already do. The grid
# reconstruction is the part with the bugs in it, and four copies of it would be
# four things to keep right.
from pty_session import (  # noqa: E402
    COLUMNS,
    EXIT_SECONDS,
    KEY_SECONDS,
    ROWS,
    SETTLE_SECONDS,
    Grid,
    drain,
)

RATE = 48_000


def write_fixture(path: Path) -> None:
    """A short deterministic 48 kHz WAV, so `:chop grid 4` gives the pads something.

    48 kHz because it is the engine rate, so load_sample() takes the resampler's
    bit-exact passthrough and nothing here depends on which libsamplerate is
    installed. Integer arithmetic and no math module, so the bytes are identical
    on every platform -- the rule write_fixture_wav() follows in
    tests/tui/pty_session.py, for the same reason.
    """
    total = RATE  # one second
    samples = bytearray(2 * total)
    for frame in range(total):
        # A square wave that changes period every quarter, so the four grid
        # slices are audibly different from one another.
        period = 40 + (60 * (frame // (total // 4)))
        value = 24_000 if (frame // period) % 2 == 0 else -24_000
        struct.pack_into("<h", samples, 2 * frame, value)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(samples))


def lane_rows(painted: str) -> list[str]:
    """The pattern lane's pad rows, as they are drawn.

    Matched on the two-digit pad label followed by step cells, which is the lane
    and nothing else on the screen -- the pad grid's own labels are followed by a
    name, not by dots.
    """
    return [row for row in painted.split("\n") if re.search(r"\d\d [█·]{4} ", row)]


def steps_on(painted: str) -> int:
    """How many steps are lit across the whole lane."""
    return sum(row.count("█") for row in lane_rows(painted))


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <cratedig-binary>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print(f"error: no such binary {binary}", file=sys.stderr)
        return 1

    # A unique directory with a fixed name inside it: several presets may run
    # this at once, and the file name reaches the screen.
    workdir = Path(tempfile.mkdtemp(prefix="cratedig-seq-"))
    fixture = workdir / "cratedig_seq_fixture.wav"
    write_fixture(fixture)

    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ.pop("NO_COLOR", None)
        os.environ.pop("CRATEDIG_UPDATE_SNAPSHOTS", None)
        os.execv(str(binary), [str(binary), "--no-audio", str(fixture)])

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLUMNS, 0, 0))

    output = bytearray()
    alive = drain(fd, SETTLE_SECONDS, output)

    def send(text: str) -> bool:
        os.write(fd, text.encode())
        return drain(fd, KEY_SECONDS, output)

    def screen_now() -> str:
        grid = Grid(COLUMNS, ROWS)
        grid.feed(output.decode("utf-8", "replace"))
        return grid.render()

    failures: list[str] = []

    if not alive:
        failures.append("the program exited before the session started")

    if alive:
        # Something on the pads first, so a step that fires has a sound to play.
        # `:chop grid 4` rather than transient: this session is about the
        # sequencer, and arithmetic boundaries keep the analysis out of it.
        alive = send(":chop grid 4\r")
        if "4 slices" not in screen_now():
            failures.append(":chop grid 4 did not report four slices")

        # Onto the lane. `t` brings it up by itself, which is the binding M4.5
        # will keep -- a step key that edited an invisible grid would be a way to
        # write a pattern you cannot see.
        alive = send("t")
        opened = screen_now()
        if "pattern 01" not in opened:
            failures.append("`t` did not bring the pattern lane up")
        if steps_on(opened) != 1:
            failures.append(f"`t` wrote {steps_on(opened)} steps, expected exactly 1")

        # A four-on-the-floor on pad 1: steps 1, 5, 9 and 13. The cursor starts
        # on step 1, which `t` above already turned on.
        alive = send("]]]]t]]]]t]]]]t")
        beat = screen_now()
        if steps_on(beat) != 4:
            failures.append(f"the pad 1 row has {steps_on(beat)} steps, expected 4")

        # The shape, not just the count. Four steps could be four adjacent ones.
        rows = lane_rows(beat)
        if not rows:
            failures.append("no pattern lane rows on screen at all")
        elif "█··· █··· █··· █···" not in rows[0]:
            failures.append(f"pad 1's row is not four-on-the-floor: {rows[0]!r}")

        # A second pad, so the lane is carrying more than one row and the pad
        # selection is what decides where a step lands. "w" is pad 2.
        alive = send("w")
        alive = send("]]]]t")
        two = screen_now()
        if steps_on(two) != 5:
            failures.append(f"after writing on pad 2 the lane has {steps_on(two)} steps, expected 5")

    if alive:
        # The verbs, each checked by its answer AND -- where it is visible -- by
        # what the lane draws once the answer clears. The answer proves the
        # command was understood; the lane proves the edit was published.
        alive = send(":bpm 92.5\r")
        if "92.50" not in screen_now():
            failures.append("`:bpm 92.5` did not answer with 92.50")
        alive = send("q")
        if "92.50 bpm" not in screen_now():
            failures.append("the tempo did not reach the mode line")

        alive = send(":swing 58\r")
        if "58" not in screen_now():
            failures.append("`:swing 58` was not acknowledged")
        alive = send("q")
        if "swing 58%" not in screen_now():
            failures.append("the swing did not reach the lane caption")

        alive = send(":pattern length 32\r")
        alive = send("q")
        long_pattern = screen_now()
        if "32 steps" not in long_pattern:
            failures.append("`:pattern length 32` did not reach the lane")
        if "showing 1-16" not in long_pattern:
            failures.append("a 32-step pattern does not say it is showing part of itself")

        alive = send(":metro on\r")
        if "metronome on" not in screen_now():
            failures.append("`:metro on` was not acknowledged")
        alive = send("q")
        if "metro" not in screen_now():
            failures.append("the metronome does not show in the lane caption")

        # Chaining, which is the half of M4 the ROADMAP called "multiple patterns
        # + chaining". Pattern 2 must be EMPTY -- if selecting it showed pattern
        # 1's steps, every check above would still have passed.
        alive = send(":song 1 2\r")
        if "song" not in screen_now():
            failures.append("`:song 1 2` was not acknowledged")

        alive = send(":pattern 2\r")
        alive = send("q")
        second = screen_now()
        if "pattern 02" not in second:
            failures.append("`:pattern 2` did not move the lane")
        if steps_on(second) != 0:
            failures.append(f"pattern 2 shows {steps_on(second)} steps; it was never written to")

        # And back, to prove the first pattern survived being navigated away
        # from. A published state that lost its patterns on selection would look
        # perfectly fine right up to this line.
        alive = send(":pattern 1\r")
        alive = send("q")
        back = screen_now()
        if steps_on(back) != 5:
            failures.append(f"pattern 1 came back with {steps_on(back)} steps, expected 5")

    if alive:
        # The transport. With no device nothing renders, so the honest answer is
        # to say so -- and saying so is the assertion.
        alive = send("p")
        if "no audio device" not in screen_now():
            failures.append("`p` did not say why the transport cannot run")

    if alive:
        # Escape quits from PERFORM; "q" is pad 1 and would only play it.
        os.write(fd, b"\x1b")
    drain(fd, EXIT_SECONDS, output)

    _, status = os.waitpid(pid, 0)
    exit_code = os.waitstatus_to_exitcode(status)
    os.close(fd)

    if exit_code != 0:
        failures.append(f"exit code {exit_code}, expected 0")

    tail = Grid(COLUMNS, ROWS)
    tail.feed(output.decode("utf-8", "replace"))
    if not tail.saw_alt_screen_exit:
        failures.append("the program never left the alternate screen buffer (no CSI ?1049l)")

    shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print("sequencer e2e session FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        print("\n--- last painted frame ---", file=sys.stderr)
        print(tail.render(), file=sys.stderr)
        return 1

    print("sequencer e2e session ok: pattern written, chained and published, exit 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
