#!/usr/bin/env python3
"""The Tab menu, typed into the real binary: open, cycle, dismiss, run.

tests/unit/completion_test.cpp proves complete_verbs() and complete_paths()
choose the right things. tests/tui/layout_snapshot_test.cpp proves the menu is
drawn correctly given a CompletionState. Neither presses Tab, and every one of
their assertions would still pass if Tab were unbound.

THE REASON THAT IS NOT A THEORETICAL CONCERN
--------------------------------------------
M5 recorded, in three comments and one design decision, that FTXUI consumes Tab
before the program sees it -- so PERFORM's panel switch was declared dead and
MIX's paging was put on `[`/`]` instead. It was wrong. Tab arrives as 9 on a
plain xterm and always did; the probe that "proved" otherwise read a stale
screen. This session is what makes that unable to happen again: it presses the
key and checks the screen changed.

It also covers the two keys that genuinely were not arriving until M5.5 T7 --
Shift-Tab and the vertical arrows, which legacy_key() never mapped.

Usage: pty_complete_session.py <cratedig-binary>
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

# The escape sequences a terminal sends. Written out rather than named by a
# library so that what is being pressed is visible in the test.
TAB = "\t"
SHIFT_TAB = "\x1b[Z"
ARROW_DOWN = "\x1b[B"
ARROW_UP = "\x1b[A"
ESCAPE = "\x1b"
RETURN = "\r"


def write_fixture(path: Path) -> None:
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(b"\x00\x00" * (RATE // 4))


def prompt_line(screen: str) -> str:
    """The `:` line, and only it.

    Read as its own row rather than by searching the screen, for the reason four
    tests in this project have already needed: the menu below quotes the same
    words the prompt does, so an unclipped search cannot tell "the line says
    chop grid" from "a menu row offers chop grid" -- which is the entire
    distinction this session exists to check.
    """
    for line in screen.splitlines():
        if line.startswith(" :"):
            return line.rstrip()
    return ""


def menu_rows(screen: str) -> list[str]:
    """The menu's entry rows: everything between the rule and the caption."""
    lines = [line.rstrip() for line in screen.splitlines()]
    rule = next((n for n, line in enumerate(lines) if line.startswith("─" * 10)), None)
    if rule is None:
        return []
    rows = []
    for line in lines[rule + 1 :]:
        if line.startswith("  tab next"):
            break
        rows.append(line)
    return rows


def selected_row(screen: str) -> str:
    for row in menu_rows(screen):
        if row.startswith("  ▌"):
            return row.strip()
    return ""


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <cratedig-binary>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print(f"error: no such binary {binary}", file=sys.stderr)
        return 1

    workdir = Path(tempfile.mkdtemp(prefix="cratedig-complete-"))
    write_fixture(workdir / "break.wav")
    write_fixture(workdir / "bass.wav")
    (workdir / "notes.txt").write_text("not audio\n")

    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ.pop("NO_COLOR", None)
        os.environ.pop("CRATEDIG_UPDATE_SNAPSHOTS", None)
        os.execv(str(binary), [str(binary), "--no-audio", str(workdir / "break.wav")])

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
        # THE KEY M5 SAID NEVER ARRIVES.
        alive = send(":ch")
        if menu_rows(screen_now()):
            failures.append("the menu was open before Tab was pressed")
        alive = send(TAB)
        rows = menu_rows(screen_now())
        if not rows:
            failures.append("Tab did not open the menu — the key never reached the program")
        elif len(rows) != 3:
            failures.append(f"expected the three chop phrases, got {len(rows)}: {rows}")
        if "chop grid" not in selected_row(screen_now()):
            failures.append(f"the first offer is not selected: {selected_row(screen_now())!r}")

        # The LINE shows the selection, so Enter runs what is visible.
        if prompt_line(screen_now()) != " :chop grid█":
            failures.append(f"the line does not show the selection: {prompt_line(screen_now())!r}")

    if alive:
        alive = send(TAB)
        if "chop transient" not in selected_row(screen_now()):
            failures.append(f"a second Tab did not advance: {selected_row(screen_now())!r}")
        if prompt_line(screen_now()) != " :chop transient█":
            failures.append("the line did not follow the selection")

    if alive:
        # SHIFT-TAB, which legacy_key() did not map until T7. Before that this
        # produced nothing at all rather than going back one.
        alive = send(SHIFT_TAB)
        if "chop grid" not in selected_row(screen_now()):
            failures.append(f"shift-tab did not go back: {selected_row(screen_now())!r}")

    if alive:
        # And the vertical arrows, which were not mapped either -- so every
        # arrow binding on every vertical list in this program did nothing.
        alive = send(ARROW_DOWN)
        if "chop transient" not in selected_row(screen_now()):
            failures.append(f"arrow-down did not advance: {selected_row(screen_now())!r}")
        alive = send(ARROW_UP)
        if "chop grid" not in selected_row(screen_now()):
            failures.append(f"arrow-up did not go back: {selected_row(screen_now())!r}")

    if alive:
        # Cycling WRAPS, so the last entry is reachable going backwards.
        alive = send(SHIFT_TAB)
        if "chop reset" not in selected_row(screen_now()):
            failures.append(f"cycling does not wrap: {selected_row(screen_now())!r}")

    if alive:
        # Typing dismisses it. The set was captured when Tab was pressed, and
        # cycling it against a line that has since changed would offer things
        # that no longer match what is on screen.
        alive = send(" ")
        if menu_rows(screen_now()):
            failures.append("typing did not dismiss the menu")
        if prompt_line(screen_now()) != " :chop reset █":
            failures.append(f"typing did not reach the line: {prompt_line(screen_now())!r}")

    if alive:
        # And a FINISHED phrase offers nothing. `chop reset ` -- the trailing
        # space is what makes it finished -- is a command waiting for arguments,
        # not a verb waiting to be chosen, and a menu over the top of it would
        # hide the thing it was helping write.
        alive = send(TAB)
        if menu_rows(screen_now()):
            failures.append("a menu opened over a phrase that is already complete")
        alive = send(ESCAPE)

    if alive:
        # Esc closes the MENU first and the prompt second. Two steps, because
        # opening a menu by accident and wanting the whole line gone are two
        # different intentions.
        alive = send(":ch")
        alive = send(TAB)
        if not menu_rows(screen_now()):
            failures.append("Tab did not open the menu")
        alive = send(ESCAPE)
        if menu_rows(screen_now()):
            failures.append("esc did not close the menu")
        if not prompt_line(screen_now()):
            failures.append("esc closed the whole prompt instead of just the menu")
        alive = send(ESCAPE)
        if prompt_line(screen_now()):
            failures.append("a second esc did not close the prompt")

    if alive:
        # A completed command RUNS. The whole feature is worthless if the line
        # Tab produced is not one the parser accepts -- which is the same claim
        # completion_test.cpp makes against the table, made here against the
        # real binary.
        alive = send(":chop gri")
        alive = send(TAB)
        alive = send(" 4" + RETURN)
        if "4 slices" not in screen_now():
            failures.append("a Tab-completed `chop grid 4` did not run")

    if alive:
        # PATHS. Two audio files and one text file in the directory; only the
        # audio is offered.
        alive = send(f":load {workdir}/")
        alive = send(TAB)
        rows = menu_rows(screen_now())
        text = "\n".join(rows)
        if not rows:
            failures.append("Tab offered nothing for a directory that has files in it")
        if "bass.wav" not in text or "break.wav" not in text:
            failures.append(f"the audio files were not offered: {rows}")
        if "notes.txt" in text:
            failures.append("notes.txt was offered — the audio filter is not applied")

        # Sorted, so `bass` comes before `break` and the first is selected.
        if "bass.wav" not in selected_row(screen_now()):
            failures.append(f"paths are not sorted: {selected_row(screen_now())!r}")

    if alive:
        # And loading it works, which is the end-to-end claim.
        alive = send(RETURN)
        if "bass.wav" not in screen_now():
            failures.append("a Tab-completed `load` did not load the file")

    if alive:
        # Tab with nothing to offer says so rather than doing nothing silently.
        # A Tab that did nothing is indistinguishable from a Tab the terminal
        # ate, which is the confusion that cost M5 a wrong finding.
        alive = send(":zzz")
        alive = send(TAB)
        if menu_rows(screen_now()):
            failures.append("a menu opened for a verb that does not exist")
        if "no command starts with that" not in screen_now():
            failures.append("Tab with no matches said nothing")
        alive = send(ESCAPE)

    if alive:
        send("q")
        drain(fd, EXIT_SECONDS, output)

    os.close(fd)
    os.waitpid(pid, 0)

    if failures:
        print("completion session failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print("\n--- final screen ---", file=sys.stderr)
        grid = Grid(COLUMNS, ROWS)
        grid.feed(decode_stream(output))
        print(grid.render(), file=sys.stderr)
        return 1

    print("completion session ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
