#!/usr/bin/env python3
"""Taking a piece of a record without loading the whole thing: preview, mark, grab.

The other BROWSE session covers getting AROUND -- moving, descending, loading.
This one covers what you do once you have found something: hear it, see it, mark
a bit of it, and put that bit on a pad.

WHY THESE ARE TWO SESSIONS
--------------------------
They fail for different reasons and one long session hides which. Navigation
breaks when the directory reader or the cursor is wrong; this breaks when the
preview, the region arithmetic or the crate bookkeeping is wrong. A single
session that did both would report "browsing is broken" for either.

WHAT ONLY AN END-TO-END TEST CAN SEE HERE
-----------------------------------------
Grabbing a region touches four things that are separately tested and have never
been exercised together: the audition path decodes the file, the peak pyramid
draws it, the sample pool takes the file and a slice, and the engine takes a pad
config pointing into audio the crate did not have a moment earlier. The unit
tests prove each; only this proves that pressing `i`, `[`, `3` in a real terminal
ends with a pad holding a piece of a file that was never `:load`ed.

Usage: pty_grab_session.py <cratedig-binary>
"""

from __future__ import annotations

import fcntl
import math
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

# The pad grid occupies the left 46 columns at the 100-column design grid.
# Clipped for the reason every panel assertion in this project is: the mode line
# quotes pad numbers and file names when it reports what just happened.
PAD_GRID_COLUMNS = 46


def write_fixture(path: Path, seconds: float = 2.0) -> None:
    """Bursts separated by silence, so the drawn waveform is a picture of something.

    A flat tone would draw an even band and make "the preview is showing the
    file" indistinguishable from "the preview is showing anything at all".
    """
    total = int(RATE * seconds)
    samples = bytearray(2 * total)
    for frame in range(total):
        loud = (frame // 6_000) % 2 == 0
        value = int(20_000 * math.sin(frame * 0.05)) if loud else 0
        struct.pack_into("<h", samples, 2 * frame, value)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(samples))


def preview_rows(screen: str) -> list[str]:
    """The preview strip: the panel whose title starts with `preview`."""
    lines = [line.rstrip() for line in screen.splitlines()]
    start = next((n for n, line in enumerate(lines) if line.startswith("╭ preview")), None)
    if start is None:
        return []
    rows = []
    for line in lines[start:]:
        rows.append(line)
        if line.startswith("╰") and len(rows) > 1:
            break
    return rows


def region_bar(screen: str) -> str:
    """The row under the waveform carrying the playhead and the region bracket."""
    for row in preview_rows(screen):
        if "▲" in row or "△" in row or "▏" in row:
            return row
    return ""


def pad_row(screen: str) -> str:
    """The pad grid's name row, clipped to the grid's own columns."""
    for line in screen.splitlines():
        clipped = line[:PAD_GRID_COLUMNS]
        if clipped.count("│") >= 4 and " 01 " in clipped and " 02 " in clipped:
            return clipped
    return ""


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <cratedig-binary>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print(f"error: no such binary {binary}", file=sys.stderr)
        return 1

    workdir = Path(tempfile.mkdtemp(prefix="cratedig-grab-"))
    write_fixture(workdir / "amen.wav")
    write_fixture(workdir / "vocal.wav", seconds=1.0)
    # The file the session starts with, kept OUT of the browsed directory so the
    # crate's growth is unambiguous: anything that appears in it came from here.
    (workdir / "start").mkdir()
    write_fixture(workdir / "start" / "start.wav")

    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ.pop("NO_COLOR", None)
        os.environ.pop("CRATEDIG_UPDATE_SNAPSHOTS", None)
        os.execv(str(binary), [str(binary), "--no-audio", str(workdir / "start" / "start.wav")])

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
        alive = send(f":browse {workdir}\r")
        if preview_rows(screen_now()):
            failures.append("the preview strip was showing before anything was previewed")

    if alive:
        # Onto amen.wav: the listing is `..`, `start/`, `amen.wav`, `vocal.wav`.
        alive = send("jj")
        alive = send(" ")
        rows = preview_rows(screen_now())
        if not rows:
            failures.append("space did not open the preview strip")
        elif "amen.wav" not in rows[0]:
            failures.append(f"the preview names the wrong file: {rows[0]!r}")

        # A PICTURE OF SOMETHING. The fixture is bursts and silence, so the
        # braille must not be a uniform band -- which is what a preview drawing
        # the wrong thing, or nothing, would give.
        drawn = "".join(rows[1:4])
        if not any(glyph in drawn for glyph in "⣿⣤⠛⠿⣶"):
            failures.append("the preview strip is empty — nothing was drawn")
        if drawn.count(" ") < 20:
            failures.append("the preview has no silence in it — it is not this file")

    if alive:
        # The whole file is not a region until one is marked.
        if "▕" in region_bar(screen_now()):
            failures.append("a region was marked before i was pressed")

    if alive:
        # MARK ONE. With no audio device nothing advances, so `i` takes frame 0
        # and the out point takes the end of the file -- the whole file as a
        # region rather than an empty one, which is what makes this testable at
        # all without a sound card.
        alive = send("i")
        if "region" not in screen_now():
            failures.append("i did not mark a region")
        bar = region_bar(screen_now())
        if "▕" not in bar:
            failures.append(f"the region has no end bracket: {bar!r}")

    if alive:
        # Trim it. A sixteenth of the file a press, so three presses take a
        # visible bite out of the bracket.
        before = region_bar(screen_now()).index("▕")
        alive = send("[[[")
        after = region_bar(screen_now()).index("▕")
        if after >= before:
            failures.append(f"[ did not shorten the region ({before} -> {after})")

        alive = send("]")
        longer = region_bar(screen_now()).index("▕")
        if longer <= after:
            failures.append(f"] did not lengthen the region ({after} -> {longer})")

    if alive:
        # GRAB IT. A pad key is what puts it on a pad -- the pads are otherwise
        # off on this screen, so the same sixteen keys are free to mean this.
        if "s01" in pad_row(screen_now()):
            failures.append("pad 3 was holding something before the grab")
        alive = send("3")
        if "→ pad 3" not in screen_now():
            failures.append("a pad key did not grab the region")

    if alive:
        # The pad grid says so, read from the grid rather than from the message.
        alive = send("\x1b")
        row = pad_row(screen_now())
        if "03 s01" not in row:
            failures.append(f"pad 3 is not holding the grabbed region: {row!r}")

    if alive:
        # And the crate has the file it came from -- with ONE cut, which is the
        # whole difference between grabbing a slice and loading and chopping.
        alive = send(":files\r")
        listed = screen_now()
        if "amen.wav" not in listed:
            failures.append("the grabbed file is not in the crate")
        if "(1)" not in listed:
            failures.append(f"the grabbed file does not have exactly one cut: {listed.splitlines()[-1]!r}")

    if alive:
        # A SECOND GRAB FROM THE SAME FILE appends a cut rather than adding the
        # file twice. pool.add() deliberately leaves an existing entry alone,
        # including its slices, so this path is the one that has to get it right.
        alive = send(f":browse {workdir}\r")
        alive = send("jj")
        alive = send(" ")
        alive = send("i")
        alive = send("[[[[")
        alive = send("4")
        alive = send("\x1b")
        alive = send(":files\r")
        listed = screen_now()
        if "(2)" not in listed:
            failures.append(f"a second grab did not append a cut: {listed.splitlines()[-1]!r}")
        row = pad_row(screen_now())
        if "04 s02" not in row:
            failures.append(f"the second grab is not on pad 4 as slice 2: {row!r}")

    if alive:
        # A NEW PREVIEW CLEARS THE OLD REGION. Carrying it over would offer a
        # bracket measured against a file it does not belong to.
        alive = send(f":browse {workdir}\r")
        alive = send("jj")
        alive = send(" ")
        alive = send("i")
        alive = send("j")  # onto vocal.wav
        alive = send(" ")
        # THE END BRACKET, not the start one. `i` with no audio device marks
        # from frame 0, and the playhead is drawn over the bracket -- so a check
        # for the opening `▏` is blind at exactly the position this test creates,
        # and passed happily with the clearing deleted.
        if "▕" in region_bar(screen_now()):
            failures.append("previewing another file kept the previous file's region")

    if alive:
        # A pad key with nothing marked SAYS SO rather than doing nothing. A key
        # that silently does nothing reads as broken -- the lesson h/l and the
        # ADSR panel both taught this project.
        alive = send("-")
        # `q`, which is PAD 5 -- the map is `1234 qwer asdf zxcv`, so `5` is not
        # a pad key at all and pressing it would prove nothing.
        alive = send("q")
        if "mark a region" not in screen_now():
            failures.append("a pad key with no region marked said nothing")

    if alive:
        # E loads and opens EDIT, for when the answer is the whole file rather
        # than one region. Capital, because `e` is pad 7.
        alive = send("E")
        if "edit" not in screen_now().splitlines()[0]:
            failures.append("E did not open EDIT")
        if "vocal.wav" not in screen_now().splitlines()[0]:
            failures.append("E opened EDIT on the wrong file")

    if alive:
        send("\x1b")
        send("q")
        drain(fd, EXIT_SECONDS, output)

    os.close(fd)
    os.waitpid(pid, 0)

    if failures:
        print("grab session failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print("\n--- final screen ---", file=sys.stderr)
        grid = Grid(COLUMNS, ROWS)
        grid.feed(decode_stream(output))
        print(grid.render(), file=sys.stderr)
        return 1

    print("grab session ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
