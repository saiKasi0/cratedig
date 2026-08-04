#!/usr/bin/env python3
"""Turning the chop knobs and watching the cuts move.

tests/tui/layout_snapshot_test.cpp proves render_chop() draws a ChopState. Every
assertion in it would pass with the keymap deleted and the detector never run --
which is the whole feature, because docs/ROADMAP.md's argument for this screen is
that a parameter you have to re-run a command to evaluate is one nobody tunes.

WHAT ONLY THIS CAN SEE
----------------------
That a keystroke changes the SLICE COUNT. Everything else about the screen is a
picture of a struct; this is the part where a key reaches the detector, the
detector re-picks, and the answer comes back before the next frame. It also
covers the two things that are easy to get backwards and impossible to notice
from a snapshot: that lower sensitivity cuts MORE, and that Esc leaves the file's
existing slices alone while Enter replaces them.

Usage: pty_chop_tune_session.py <cratedig-binary>
"""

from __future__ import annotations

import fcntl
import os
import pty
import re
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
NUM_HITS = 12


def write_fixture(path: Path) -> None:
    """Hits of two strengths, so sensitivity has something to choose between.

    Deterministic integer arithmetic, no libm -- the rule every fixture in this
    project follows, because `std::sin` is not required to give the same last bit
    on two platforms.
    """
    spacing = RATE // 6  # 167 ms, fast enough that the gap control bites
    total = spacing * (NUM_HITS + 1)
    samples = bytearray(2 * total)

    state = 0x1234_5678

    def add_hit(at: int, peak: int) -> None:
        nonlocal state
        decay = RATE // 20
        for offset in range(decay):
            if at + offset >= total:
                return
            state = (state * 1_664_525 + 1_013_904_223) & 0xFFFF_FFFF
            noise = ((state >> 8) % 2_000) - 1_000
            envelope = (decay - offset) / decay
            value = int(peak * envelope * envelope * noise / 1_000)
            existing = struct.unpack_from("<h", samples, 2 * (at + offset))[0]
            struct.pack_into("<h", samples, 2 * (at + offset),
                             max(-32_000, min(32_000, existing + value)))

    for hit in range(NUM_HITS):
        add_hit(spacing * (hit + 1), 26_000)

        # A GHOST between each pair, and they get quieter across the file.
        #
        # A GRADIENT rather than two levels, which took two attempts to arrive
        # at. Twelve clean hits over silence gave 12 slices at every setting --
        # nothing marginal to include or exclude. Twelve hits plus twelve equal
        # ghosts gave 24 at every setting, for the same reason one level up: both
        # populations sat far from the threshold, so moving it crossed neither.
        #
        # Ghosts spanning 200..2600 put SOMETHING near the threshold wherever the
        # threshold is, so the count moves monotonically with it -- which is what
        # a test of a continuous control needs.
        add_hit(spacing * (hit + 1) + (spacing // 2), 200 + (200 * hit))

    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(samples))


def slice_count(screen: str) -> int:
    """The count in the wave panel's title, not the mode line.

    Bounded for the reason every screen assertion in this project is: the mode
    line reports counts too, so an unclipped search cannot tell the panel from
    the program's own commentary about it.
    """
    for line in screen.splitlines():
        if line.startswith("╭ cutting"):
            match = re.search(r"(\d+) slices?", line)
            return int(match.group(1)) if match else -1
    return -1


def selected_field(screen: str) -> str:
    for line in screen.splitlines():
        if line.startswith("│ ▸"):
            return line.strip("│ ").strip()
    return ""


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <cratedig-binary>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print(f"error: no such binary {binary}", file=sys.stderr)
        return 1

    workdir = Path(tempfile.mkdtemp(prefix="cratedig-chop-tune-"))
    fixture = workdir / "hits.wav"
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

    def send(text: str, seconds: float = KEY_SECONDS) -> bool:
        os.write(fd, text.encode())
        return drain(fd, seconds, output)

    def screen_now() -> str:
        grid = Grid(COLUMNS, ROWS)
        grid.feed(decode_stream(output))
        return grid.render()

    failures: list[str] = []

    if not alive:
        failures.append("the program exited before the session started")

    baseline = -1
    if alive:
        alive = send(":chop tune\r", 4.0)
        if "chop" not in screen_now().splitlines()[0]:
            failures.append(":chop tune did not open the CHOP screen")
        baseline = slice_count(screen_now())
        if baseline <= 0:
            failures.append(f"the screen opened with no cuts at all: {baseline}")
        if "sensitivity" not in selected_field(screen_now()):
            failures.append(f"sensitivity is not selected first: {selected_field(screen_now())!r}")

    if alive:
        # LOWER SENSITIVITY CUTS MORE, which is the one thing about this screen
        # that is easy to get backwards: the field is a threshold multiplier, so
        # the number goes DOWN as the chop gets busier.
        alive = send("HHHH", 2.0)
        busier = slice_count(screen_now())
        if busier <= baseline:
            failures.append(f"lowering sensitivity did not cut more ({baseline} -> {busier})")

        alive = send("LLLLLLLL", 2.0)
        sparser = slice_count(screen_now())
        if sparser >= busier:
            failures.append(f"raising sensitivity did not cut less ({busier} -> {sparser})")

    if alive:
        # THE GAP, on the second row.
        alive = send("j")
        if "minimum gap" not in selected_field(screen_now()):
            failures.append(f"j did not move to the gap: {selected_field(screen_now())!r}")

        before = slice_count(screen_now())
        alive = send("LLLLLL", 2.0)
        after = slice_count(screen_now())
        if after >= before:
            failures.append(f"lengthening the gap did not cut less ({before} -> {after})")

    if alive:
        # And k goes back, so the list is navigable in both directions.
        alive = send("k")
        if "sensitivity" not in selected_field(screen_now()):
            failures.append("k did not move back to sensitivity")

    if alive:
        # ESC LEAVES THE FILE ALONE. The screen is a preview; a preview that
        # committed itself on the way out would be an edit nobody asked for.
        alive = send("\x1b", 2.0)
        if "unchanged" not in screen_now():
            failures.append("esc did not say the chop was left alone")
        alive = send(":files\r", 2.0)
        listed = screen_now()
        if "(0)" not in listed:
            failures.append(f"esc changed the file's slices: {listed.splitlines()[-1].strip()!r}")

    if alive:
        # ENTER APPLIES IT, and the pads follow.
        alive = send(":chop tune\r", 4.0)
        alive = send("j")
        alive = send("LLLLLL", 2.0)
        applied = slice_count(screen_now())
        alive = send("\r", 3.0)
        if f"chop: {applied} slices" not in screen_now():
            failures.append("enter did not apply the previewed chop")
        alive = send(":files\r", 2.0)
        if f"({applied})" not in screen_now():
            failures.append(
                f"the file does not carry the {applied} previewed slices: "
                f"{screen_now().splitlines()[-1].strip()!r}"
            )

    if alive:
        send("q")
        drain(fd, EXIT_SECONDS, output)

    os.close(fd)
    os.waitpid(pid, 0)

    if failures:
        print("chop tune session failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print("\n--- final screen ---", file=sys.stderr)
        grid = Grid(COLUMNS, ROWS)
        grid.feed(decode_stream(output))
        print(grid.render(), file=sys.stderr)
        return 1

    print("chop tune session ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
