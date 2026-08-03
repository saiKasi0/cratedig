#!/usr/bin/env python3
"""BROWSE, typed into the real binary: move, descend, audition, load.

tests/tui/layout_snapshot_test.cpp proves render_browse() draws a BrowserState
correctly. It says nothing about whether any key produces one, because a snapshot
is a function of a struct a test built by hand -- every browser assertion in it
would still pass if `j` were unbound and the listing were never read from disk.

WHAT THIS COVERS THAT NOTHING ELSE DOES
---------------------------------------
The keymap and the directory reader: that `j` moves, `l` descends into a real
directory, `h` climbs back out, SPACE auditions a file that is not loaded, and
ENTER puts it in the crate. Every one of those touches the filesystem, and the
whole point of the screen is that the thing on screen came from there.

The `loaded` marker is checked in BOTH directions -- present on the file the
session started with, absent on its neighbour, then present on the neighbour
after ENTER. A marker that was simply always drawn would pass a one-direction
check, and one that was never drawn would pass the other.

Usage: pty_browse_session.py <cratedig-binary>
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

# Longer than any listing this session makes, and short enough that giving up is
# quick. See move_to().
MAX_CURSOR_MOVES = 16

# Where the two panels are at the 100-column design grid. The crate is a fixed 34
# and the listing takes the rest, so the listing owns columns 0-64 and the crate
# 66-99 (render_browse.cpp: kCrateWidth, and one blank column between).
#
# BOUNDED ON PURPOSE, and this is the fourth test in this project to need saying
# so. A screen row spans both panels AND the mode line quotes file names when it
# reports what just happened -- so an unclipped search for "two.wav" matches the
# program's own answer rather than the listing it is supposed to be checking.
LISTING_COLUMNS = 65
CRATE_FIRST_COLUMN = 66


def write_fixture(path: Path, period: int) -> None:
    """A short deterministic 48 kHz WAV. Pitch differs so a listener could tell."""
    total = RATE // 4
    samples = bytearray(2 * total)
    for frame in range(total):
        value = 22_000 if (frame // period) % 2 == 0 else -22_000
        struct.pack_into("<h", samples, 2 * frame, value)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(samples))


def panel_rows(screen: str, first: int, last: int) -> list[str]:
    """The rows of one panel, clipped to its columns.

    Selected by box structure rather than by content, for the reason above: a row
    is a panel row if it has the box's vertical rules at both of that panel's
    edges.
    """
    rows = []
    for line in screen.splitlines():
        padded = line.ljust(COLUMNS)
        if padded[first] == "│" and padded[last] == "│":
            rows.append(padded[first : last + 1])
    return rows


def listing(screen: str) -> list[str]:
    return panel_rows(screen, 0, LISTING_COLUMNS - 1)


def crate(screen: str) -> list[str]:
    return panel_rows(screen, CRATE_FIRST_COLUMN, COLUMNS - 1)


def move_to(name: str, send, listing_now, key: str = "j") -> bool:
    """Walk the cursor down to `name`. Bounded, and that bound is not cosmetic.

    The first version of this was `while name not in cursor_line(...)`, and the
    first negative control -- unbinding `j` -- made it spin forever instead of
    failing. A test that hangs when the thing it checks is broken reports a
    timeout, which says nothing about what went wrong; one that gives up after a
    listing's worth of presses reports the assertion that failed.
    """
    for _ in range(MAX_CURSOR_MOVES):
        if name in cursor_line(listing_now()):
            return True
        if not send(key):
            return False
    return False


def cursor_line(rows: list[str]) -> str:
    """The row the cursor is on, or "" -- the marker render_browse.cpp draws."""
    for row in rows:
        if row.startswith("│ ▸"):
            return row
    return ""


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <cratedig-binary>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print(f"error: no such binary {binary}", file=sys.stderr)
        return 1

    # A real directory tree, because reading one is half of what is under test.
    workdir = Path(tempfile.mkdtemp(prefix="cratedig-browse-"))
    (workdir / "kits").mkdir()
    write_fixture(workdir / "one.wav", 20)
    write_fixture(workdir / "two.wav", 55)
    write_fixture(workdir / "kits" / "deep.wav", 33)
    # Not audio: it must not be listed. The filter is what keeps a browser
    # pointed at a music folder from being a file manager.
    (workdir / "notes.txt").write_text("not audio\n")

    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ.pop("NO_COLOR", None)
        os.environ.pop("CRATEDIG_UPDATE_SNAPSHOTS", None)
        os.execv(str(binary), [str(binary), "--no-audio", str(workdir / "one.wav")])

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
        rows = listing(screen_now())
        text = "\n".join(rows)

        # What is in the directory, and what is not.
        for name in ("kits/", "one.wav", "two.wav"):
            if name not in text:
                failures.append(f"the listing does not show {name}")
        if "notes.txt" in text:
            failures.append("the listing shows notes.txt — the audio filter is not applied")

        # The marker, direction one: the file the session opened is in the pool.
        one = [row for row in rows if "one.wav" in row]
        two = [row for row in rows if "two.wav" in row]
        if not one or "loaded" not in one[0]:
            failures.append("one.wav is in the crate but the listing does not say so")
        if not two:
            failures.append("two.wav is missing from the listing")
        elif "loaded" in two[0]:
            failures.append("two.wav is not loaded but the listing says it is")

    if alive:
        # Descend. `..` is first and `kits` second, so one `j` reaches it.
        before = cursor_line(listing(screen_now()))
        alive = send("j")
        after = cursor_line(listing(screen_now()))
        if before == after:
            failures.append("j did not move the cursor")
        if "kits" not in after:
            failures.append(f"j did not land on kits, it landed on: {after.strip()}")

    if alive:
        alive = send("l")
        text = "\n".join(listing(screen_now()))
        if "deep.wav" not in text:
            failures.append("l did not descend into kits — deep.wav is not listed")
        if "two.wav" in text:
            failures.append("l descended but the listing still shows the parent directory")

    if alive:
        alive = send("h")
        text = "\n".join(listing(screen_now()))
        if "two.wav" not in text:
            failures.append("h did not climb back out of kits")
        # AND LANDS ON WHERE YOU CAME FROM. read_directory() resets the cursor,
        # which is right for arriving somewhere new and wrong here: landing on
        # `..` makes `l` then `h` a round trip that does not come back.
        if "kits" not in cursor_line(listing(screen_now())):
            failures.append(
                "h climbed out but lost the cursor, landing on: "
                f"{cursor_line(listing(screen_now())).strip()}"
            )

    if alive:
        # Audition. SPACE on a DIRECTORY first, which is what separates "the key
        # is wired to the audition" from "the key prints a message": a browser
        # that answered `playing kits` would pass a one-sided check.
        if not move_to("kits", send, lambda: listing(screen_now())):
            failures.append("could not move the cursor onto kits")
        alive = send(" ")
        if "playing" in screen_now():
            failures.append("space on a directory tried to play it")

    if alive:
        # And on a file, which must.
        if not move_to("two.wav", send, lambda: listing(screen_now())):
            failures.append("could not move the cursor onto two.wav")
        alive = send(" ")
        if "playing two.wav" not in screen_now():
            failures.append("space on two.wav did not audition it")
        # Auditioning is not loading: the crate must not have grown.
        if "two.wav" in "\n".join(crate(screen_now())):
            failures.append("auditioning two.wav put it in the crate — it must only preview")

    if alive:
        # SPACE STOPS WHAT SPACE STARTED, reported as "browsing has no
        # deselection ability". The key only ever played: a preview ran to its
        # end whatever you did, pressing space again stacked a SECOND copy over
        # the first, and the panic key did not reach the audition lane either --
        # so there was no way to stop one at all.
        alive = send(" ")
        if "stopped two.wav" not in screen_now():
            failures.append("space a second time did not stop the preview")

        # And a third press plays it again, rather than the toggle latching off.
        alive = send(" ")
        if "playing two.wav" not in screen_now():
            failures.append("space a third time did not replay it")

    if alive:
        # Moving to another file and pressing space plays THAT one rather than
        # stopping this one -- on a different entry the useful thing is to hear
        # it, not to press space twice.
        if not move_to("one.wav", send, lambda: listing(screen_now()), key="k"):
            failures.append("could not move the cursor onto one.wav")
        alive = send(" ")
        if "playing one.wav" not in screen_now():
            failures.append("space on a different file did not switch the preview to it")
        alive = send(" ")
        if not move_to("two.wav", send, lambda: listing(screen_now())):
            failures.append("could not move back onto two.wav")

    if alive:
        # Load it. The crate panel is the observable, not the mode line.
        alive = send("\r")
        crate_text = "\n".join(crate(screen_now()))
        if "two.wav" not in crate_text:
            failures.append("enter did not put two.wav in the crate")
        if "one.wav" not in crate_text:
            failures.append("loading two.wav dropped one.wav from the crate")

        # The marker, direction two: the listing now says so, having not before.
        two = [row for row in listing(screen_now()) if "two.wav" in row]
        if not two or "loaded" not in two[0]:
            failures.append("two.wav was loaded but the listing does not say so")

        # AND YOU ARE STILL STANDING WHERE YOU WERE. Loading re-reads the
        # directory so the marker updates, and a re-read resets the cursor --
        # so without care you are thrown to the top of the listing every time,
        # which is exactly while you are working down one loading several.
        if "two.wav" not in cursor_line(listing(screen_now())):
            failures.append(
                "loading moved the cursor off two.wav, onto: "
                f"{cursor_line(listing(screen_now())).strip()}"
            )

    if alive:
        alive = send("\x1b")
        if "browse" in screen_now().splitlines()[0]:
            failures.append("esc did not leave BROWSE")

    if alive:
        send("q")
        drain(fd, EXIT_SECONDS, output)

    os.close(fd)
    os.waitpid(pid, 0)

    if failures:
        print("BROWSE session failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print("\n--- final screen ---", file=sys.stderr)
        grid = Grid(COLUMNS, ROWS)
        grid.feed(decode_stream(output))
        print(grid.render(), file=sys.stderr)
        return 1

    print("BROWSE session ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
