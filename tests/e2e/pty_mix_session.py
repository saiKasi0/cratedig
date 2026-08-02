#!/usr/bin/env python3
"""M5's mixer, typed into the real binary: reach every strip control and read it back.

tests/unit/command_test.cpp proves the parser understands `:gain 3 -6`. It cannot
prove that anything HAPPENS. This session is the other half: that the verbs and
the MIX keymap reach the engine's strip configs rather than a copy of them in the
interface.

WHY THE READBACK IS EVIDENCE
----------------------------
src/tui/app.cpp builds MixState from `Engine::strip(pad)` and
`Engine::bus_gain(bus)` -- the control thread's view of what it PUBLISHED, not a
local mirror the UI keeps for itself. So a fader that reads -6.0 dB on screen is
a config that went through set_strip() and into the handoff ring. A UI that
applied the command to its own state and never published would draw the same
number and fail nothing, which is exactly the failure mode this exists to catch;
it cannot pass here.

WHY THE AUDIO IS NOT CHECKED HERE
---------------------------------
`--no-audio` opens no device, so Engine::render() is never called and no
telemetry is published: the meters stay dark and the compressor never reduces
anything. That the DSP does what it says is proved where the audio is, in the
unit and e2e render tests. Asserting a moving meter here would either fail or
pass for the wrong reason.

Usage: pty_mix_session.py <cratedig-binary>
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

# The terminal replay and the reader come from the TUI session, as the chop and
# sequencer sessions already do. The grid reconstruction is the part with the
# bugs in it, and four copies would be four things to keep right.
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


def write_fixture(path: Path) -> None:
    """A short deterministic 48 kHz WAV, so `:chop grid 4` gives the pads material.

    48 kHz because it is the engine rate, so the resampler takes its bit-exact
    passthrough and nothing here depends on which libsamplerate is installed.
    Integer arithmetic only, so the bytes are identical on every platform.
    """
    total = RATE // 2
    samples = bytearray(2 * total)
    for frame in range(total):
        period = 40 + (60 * (frame // (total // 4)))
        value = 24_000 if (frame // period) % 2 == 0 else -24_000
        struct.pack_into("<h", samples, 2 * frame, value)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(samples))


# The rows the strip panels occupy: below the header and its blank line, above
# the mode line. BOUNDED, and that is not tidiness.
#
# A version of this read every row of the grid, and the mode line lives on one of
# them -- so after `:gain 3 -6` it says "pad 3 gain -6.0 dB", and a check for
# "-6.0" in pad 3's ten columns found the ANSWER rather than the fader. It passed
# with the strip edits disabled entirely, which is the one thing this session
# exists to catch.
PANEL_FIRST_ROW = 2
PANEL_LAST_ROW = 27


def strip_column(screen: str, index: int) -> list[str]:
    """The text of one strip panel, as its rows.

    Panels are ten columns wide with one of gap, so strip N starts at 11*N. Read
    by SLICING THE GRID rather than by searching the whole screen: "-6.0" appears
    on whichever strip has that fader, and a test that looked for it anywhere
    would pass when the wrong strip moved -- or when nothing moved and only the
    mode line mentioned it.
    """
    left = index * 11
    rows = []
    for row, line in enumerate(screen.splitlines()):
        if row < PANEL_FIRST_ROW or row > PANEL_LAST_ROW:
            continue
        if len(line) > left:
            rows.append(line[left : left + 10])
    return rows


def panel_has(screen: str, index: int, text: str) -> bool:
    return any(text in row for row in strip_column(screen, index))


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <cratedig-binary>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print(f"error: no such binary {binary}", file=sys.stderr)
        return 1

    workdir = Path(tempfile.mkdtemp(prefix="cratedig-mix-"))
    fixture = workdir / "cratedig_mix_fixture.wav"
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
        grid.feed(decode_stream(output))
        return grid.render()

    failures: list[str] = []

    if not alive:
        failures.append("the program exited before the session started")

    if alive:
        alive = send(":chop grid 4\r")
        if "4 slices" not in screen_now():
            failures.append(":chop grid 4 did not report four slices")

    if alive:
        alive = send(":mix\r")
        screen = screen_now()
        if "master" not in screen:
            failures.append(":mix did not open the mixer")
        if "01 " not in screen:
            failures.append("the mixer drew no channel strips")

    # -- the verbs, each read back off the strip it names ---------------------

    if alive:
        alive = send(":gain 3 -6\r")
        if not panel_has(screen_now(), 2, "-6.0"):
            failures.append(":gain 3 -6 did not reach pad 3's fader")
        # And it moved THAT strip only. A command that wrote every strip would
        # pass every check above.
        if panel_has(screen_now(), 3, "-6.0"):
            failures.append(":gain 3 -6 also moved pad 4")

    if alive:
        alive = send(":pan 2 -50\r")
        if not panel_has(screen_now(), 1, "L50"):
            failures.append(":pan 2 -50 did not reach pad 2's balance")

    if alive:
        alive = send(":mute 4\r")
        if not panel_has(screen_now(), 3, "M"):
            failures.append(":mute 4 did not mute pad 4")

    if alive:
        alive = send(":solo 5 on\r")
        if not panel_has(screen_now(), 4, "S"):
            failures.append(":solo 5 on did not solo pad 5")
        if "solo" not in screen_now():
            failures.append("the header did not report that something is soloed")

    if alive:
        alive = send(":bus 6 c\r")
        if not panel_has(screen_now(), 5, "bus c"):
            failures.append(":bus 6 c did not route pad 6")

    if alive:
        alive = send(":eq 1 2 800 -4 1.2\r")
        if not panel_has(screen_now(), 0, "eq  1bd"):
            failures.append(":eq did not enable a band on pad 1")

    if alive:
        alive = send(":comp 1 -18 4\r")
        if not panel_has(screen_now(), 0, "cmp 4:1"):
            failures.append(":comp did not reach pad 1's compressor")

    if alive:
        alive = send(":limit on\r")
        if "lim on" not in screen_now():
            failures.append(":limit on did not engage the master limiter")

    # -- the keymap ------------------------------------------------------------

    if alive:
        # `l` moves the cursor, `k` raises the fader BY ONE dB. Pad 1 is at 0.0
        # to start, so after one `l` and one `k` it is pad 2 that reads 1.0.
        alive = send("lk")
        if not panel_has(screen_now(), 1, "1.0"):
            failures.append("l then k did not raise pad 2's fader by a decibel")

    if alive:
        alive = send("m")
        if not panel_has(screen_now(), 1, "M"):
            failures.append("m did not mute the strip under the cursor")

    if alive:
        alive = send("b")
        if not panel_has(screen_now(), 1, "bus b"):
            failures.append("b did not cycle pad 2's bus")

    if alive:
        # `]` pages to channels 9-16; the strips are renumbered and the header
        # says so. This is the departure from the mockup that makes the buses
        # reachable at all (docs/design/README.md).
        #
        # NOT Tab, which is what this was first written against: Tab never
        # reaches the program at all, because FTXUI consumes it before
        # CatchEvent. This session is what caught that.
        alive = send("]")
        screen = screen_now()
        if "09 " not in screen:
            failures.append("] did not page to channels 9-16")
        if alive:
            alive = send("]")
            screen = screen_now()
            if "bus a" not in screen or "master" not in screen:
                failures.append("] again did not page to the buses with master pinned")
        if alive:
            # And back the other way, which Tab could not do at all.
            alive = send("[")
            if "09 " not in screen_now():
                failures.append("[ did not page back to channels 9-16")
            alive = send("]")

    if alive:
        # A bus has no mute, and the program says so rather than doing nothing.
        alive = send("m")
        if "no mute" not in screen_now():
            failures.append("m on a bus did not explain that a bus has no mute")

    if alive:
        alive = send("\x1b")  # escape
        if "perform" not in screen_now():
            failures.append("escape did not return to PERFORM")

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

    print("mix pty session: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
