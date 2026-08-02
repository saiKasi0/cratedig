#!/usr/bin/env python3
"""M5.5's crate, typed into the real binary: two files at once, and a pad from each.

tests/unit/pool_test.cpp proves the model holds several files. tests/unit/
engine_render_test.cpp proves the engine can play a slice of one while playing a
slice of another. Neither proves a PERSON can get there, which is what this does:
`:load` a second file, chop it, put it on pads, and check that the pads holding
the first one still play the first one.

WHY THIS IS THE TEST THAT CLOSES A GAP
--------------------------------------
Until `:load` existed, a session held exactly one file, so a slice index alone
named a sound and pad_for_slice()'s file comparison was unobservable -- deleting
it passed the whole suite, which the comment beside it said outright. Two files
is the smallest arrangement in which "slice 3" is ambiguous, and this is the
smallest test that creates one.

Usage: pty_crate_session.py <cratedig-binary>
"""

from __future__ import annotations

import fcntl
import os
import pty
import struct
import sys
import tempfile
import termios
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tui"))

from pty_session import (  # noqa: E402
    COLUMNS,
    EXIT_SECONDS,
    KEY_SECONDS,
    ROWS,
    SETTLE_SECONDS,
    Grid,
    decode_stream,
    drain,
)

RATE = 48_000

# The pad grid occupies the left 46 columns at the 100-column design grid; the
# sample/pattern panel has the rest.
PAD_GRID_COLUMNS = 46


def write_fixture(path: Path, period_base: int) -> None:
    """A short deterministic 48 kHz WAV whose pitch says which file it is.

    Two files that sound the same would make "the pad kept playing the first one"
    unprovable; `period_base` is what makes them tell apart.
    """
    total = RATE // 2
    samples = bytearray(2 * total)
    for frame in range(total):
        period = period_base + (10 * (frame // (total // 4)))
        value = 22_000 if (frame // period) % 2 == 0 else -22_000
        struct.pack_into("<h", samples, 2 * frame, value)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(samples))


def pad_row(screen: str) -> list[str]:
    """The pad grid's name rows, which say what each pad is holding.

    Read as its own thing rather than by searching the whole screen: the mode
    line quotes file names too, and a check that matched anywhere would pass on
    the program's own answer instead of on the grid.
    """
    # Selected by the grid's BOX STRUCTURE, not by the numbers in it. Matching
    # "01" and "02" anywhere also matches the wave panel's slice ruler, which
    # legitimately changes when the shown file changes -- so the first version of
    # this reported the pads had moved when only the ruler had.
    # And CLIPPED TO THE GRID'S COLUMNS. A screen row spans the pad grid and the
    # right-hand panel both, and that panel names the file being shown -- so an
    # unclipped row changes whenever the shown file does, which is exactly what
    # this check is supposed to be able to ignore. Twice now a version of this
    # test has matched the program's own answer instead of the thing asked about.
    return [
        line[:PAD_GRID_COLUMNS]
        for line in screen.splitlines()
        if line.count("\u2502") >= 4 and " 01 " in line and " 02 " in line
    ]


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <cratedig-binary>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print(f"error: no such binary {binary}", file=sys.stderr)
        return 1

    workdir = Path(tempfile.mkdtemp(prefix="cratedig-crate-"))
    first = workdir / "break_one.wav"
    second = workdir / "vocal_two.wav"
    write_fixture(first, 20)
    write_fixture(second, 55)

    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ.pop("NO_COLOR", None)
        os.environ.pop("CRATEDIG_UPDATE_SNAPSHOTS", None)
        os.execv(str(binary), [str(binary), "--no-audio", str(first)])

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLUMNS, 0, 0))

    output = bytearray()
    alive = drain(fd, SETTLE_SECONDS, output)

    def send(text: str) -> bool:
        os.write(fd, text.encode())
        return drain(fd, KEY_SECONDS, output)

    def screen_now() -> str:
        grid = Grid(COLUMNS, ROWS)
        grid.feed(decode_stream(output))
        return grid.render()

    failures: list[str] = []

    if not alive:
        failures.append("the program exited before the session started")

    if alive:
        # Chop the first file. `:chop` puts its slices on pads 1-4.
        alive = send(":chop grid 4\r")
        if "4 slices" not in screen_now():
            failures.append(":chop grid 4 on the first file did not report four slices")


    if alive:
        # THE VERB THIS TASK IS ABOUT. Adds, so the pads keep what they have.
        alive = send(f":load {second}\r")
        screen = screen_now()
        if "loaded vocal_two.wav" not in screen:
            failures.append(":load did not report loading the second file")
        if "2 in the crate" not in screen:
            failures.append(":load replaced the first file instead of adding to it")

    if alive:
        # The wave panel followed the load, so what is on screen is the new file.
        if "vocal_two.wav" not in screen_now():
            failures.append("the wave panel did not follow the newly loaded file")

    if alive:
        # And the pads did NOT follow it: they still hold the first file's chop,
        # which is what "adds rather than replaces" has to mean to be worth
        # anything.
        alive = send(":files\r")
        screen = screen_now()
        if "1 break_one.wav" not in screen or "2 vocal_two.wav" not in screen:
            failures.append(":files did not list both files")
        # The second is the one showing, and the listing marks it.
        if "vocal_two.wav*" not in screen:
            failures.append(":files did not mark which file is showing")

    if alive:
        # Chop the second file, which puts ITS slices on pads 1-4. Now pads 1-4
        # play slices of one file and pads 5-8 play slices of another -- the M5.5
        # acceptance, reached by typing.
        alive = send(":chop grid 4\r")
        if "4 slices" not in screen_now():
            failures.append(":chop grid 4 on the second file did not report four slices")

    if alive:
        # Switching back shows the first file, and ITS chop -- each file carries
        # its own slices, which is the other half of the assumption.
        alive = send(":file 1\r")
        screen = screen_now()
        if "break_one.wav" not in screen:
            failures.append(":file 1 did not switch back to the first file")

    if alive:
        # NOW build the two-file arrangement, and it has to be in this order:
        # `:chop` publishes all sixteen pads, so chopping the second file
        # evicted the first from pads 1-4 AND cleared 5-16. Assigning afterwards
        # is what puts the first file back on pads 5-8 while the second keeps
        # 1-4.
        alive = send(":slot assign 1-4 5\r")
        if "pad" not in screen_now():
            failures.append(":slot assign 1-4 5 said nothing")

    if alive:
        # THE AMBIGUOUS CASE, and the whole reason a pad names a file rather than
        # just a slice.
        #
        # Slice index 0 now exists twice: on pad 1, which holds the SECOND file,
        # and on pad 5, which holds the FIRST. The interface is showing the first
        # file, so "which pad plays slice 1 of what I am looking at" must answer
        # PAD 5.
        #
        # A lookup that compared only the slice index answers pad 1 -- the first
        # pad whose index matches -- and EDIT would then audition the vocal while
        # drawing the break. That is the failure M5.5 T2 left unasserted, because
        # with one file loaded it cannot arise.
        alive = send(":edit 1\r")
        screen = screen_now()
        # Pad 5's key is `q` (the map is 1234/qwer/asdf/zxcv), and pad 1's is `1`.
        if "pad q" not in screen:
            failures.append(
                "EDIT named the wrong pad key for the shown file's slice 1 "
                "(a slice-index-only lookup answers pad 1)"
            )
        if alive:
            alive = send("\x1b")

    # What the pad grid shows BEFORE the unload, so "the pads were left alone"
    # is a comparison rather than a hope.
    pads_before = pad_row(screen_now())

    if alive:
        # Unloading the file that is showing leaves the pads alone: they hold
        # their own reference to the audio.
        alive = send(":unload\r")
        screen = screen_now()
        if "unloaded break_one.wav" not in screen:
            failures.append(":unload did not report unloading the shown file")
        if "still play" not in screen:
            failures.append(":unload did not say that pads holding it still play")

        # AND IT IS TRUE, not merely claimed. A first version of this session
        # checked only the message, and clearing every pad on unload passed it --
        # the program said the pads still played while showing sixteen empty
        # ones. The grid is the evidence.
        if not pads_before:
            failures.append("could not read the pad grid before the unload")
        elif pad_row(screen_now()) != pads_before:
            failures.append("unload changed the pad grid: the pads did not keep their slices")

    if alive:
        alive = send(":files\r")
        screen = screen_now()
        if "break_one.wav" in screen:
            failures.append(":unload left the file in the crate")
        if "vocal_two.wav" not in screen:
            failures.append(":unload dropped the wrong file")

    if alive:
        alive = send(":q\r")
        drain(fd, EXIT_SECONDS, output)

    os.close(fd)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass

    if failures:
        print("FAILED", file=sys.stderr)
        for note in failures:
            print(f"  - {note}", file=sys.stderr)
        print("\n--- final screen ---", file=sys.stderr)
        print(screen_now(), file=sys.stderr)
        return 1

    print("crate pty session: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
