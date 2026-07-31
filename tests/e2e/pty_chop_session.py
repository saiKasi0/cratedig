#!/usr/bin/env python3
"""The M3 acceptance, typed into the real binary: import, `:chop transient`, play chops.

docs/ROADMAP.md's acceptance sentence names a command, and a command is a thing a
person types. tests/e2e/chop_e2e_test.cpp proves the pipeline behind it produces
the right audio, bit for bit, with no terminal in the way; this proves the
sentence itself -- that starting cratedig on a file, typing those two words, and
hitting the pad keys does what it says.

WHAT THIS ADDS OVER tests/tui/pty_session.py
--------------------------------------------
That session is a TUI test: it drives `:chop grid 4`, whose slice boundaries are
arithmetic, and asserts on the layout. This one runs the ANALYSIS -- the real
onset detector on real decoded audio, inside the real program -- and asserts on
the consequences: the number of chops matches the number of hits that were built
into the fixture, and the pads take them. The two are labelled differently
(`e2e` against `tui`) because they answer different questions and are run at
different times.

WHAT THIS DELIBERATELY DOES NOT ASSERT
--------------------------------------
That the pads LIGHT. `--no-audio` opens no device, so Engine::render() never
runs, so the audio thread never publishes a glow -- there is nothing to see and
a check for it here would either fail or, worse, pass for the wrong reason. The
acceptance item "pads light on trigger at any sample level" is asserted where the
signal actually exists, in tests/unit/engine_telemetry_test.cpp, against a
-60 dBFS sample. What the keystrokes below are for is that triggering a chopped
pad through the real key path is a no-op-free round trip.

Usage: pty_chop_session.py <cratedig-binary>
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

# The terminal replay and the reader come from the TUI session. Sharing them is
# deliberate and already the established pattern here -- pty_kitty_session.py
# does the same. The grid reconstruction is the part with the bugs in it, and
# three copies of it would be three things to keep right.
from pty_session import (  # noqa: E402
    COLUMNS,
    EXIT_SECONDS,
    KEY_SECONDS,
    ROWS,
    SETTLE_SECONDS,
    Grid,
    drain,
)

# Eight hits, 0.25 s apart, after 0.1 s of silence -- the same shape as the
# material in chop_e2e_test.cpp, for the same reason: the truth about where the
# hits are is a property of how the file was written, not of what the detector
# says about it afterwards.
RATE = 48_000
LEAD_FRAMES = 4_800
HIT_SPACING = 12_000
NUM_HITS = 8


def write_chop_fixture(path: Path) -> None:
    """A deterministic 48 kHz WAV with NUM_HITS unmistakable transients in it.

    48 kHz because it is the engine rate, so load_sample() takes the resampler's
    bit-exact passthrough and nothing here depends on which libsamplerate is
    installed. Integer arithmetic throughout and no math module, so the bytes are
    identical on every platform by construction -- the same rule
    write_fixture_wav() follows in tests/tui/pty_session.py.
    """
    attack_frames = 96
    body_frames = 8_000
    peak = 30_000
    total = LEAD_FRAMES + (HIT_SPACING * NUM_HITS)

    samples = bytearray(2 * total)
    noise = 0x5EED1234
    for hit in range(NUM_HITS):
        start = LEAD_FRAMES + (hit * HIT_SPACING)
        period = 20 - (hit * 2)
        for offset in range(body_frames):
            frame = start + offset
            if frame >= total:
                break
            decay = (peak * (body_frames - offset)) // body_frames
            if offset < attack_frames:
                noise = ((noise * 1_664_525) + 1_013_904_223) & 0xFFFFFFFF
                value = decay if (noise & 0x80000000) else -decay
            else:
                value = decay if (offset // period) % 2 == 0 else -decay
            struct.pack_into("<h", samples, 2 * frame, value)

    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(bytes(samples))


def slice_count_on(painted: str) -> int:
    """How many chops the program says it made, read off its own message line.

    Read from the screen rather than counted from the pad labels: the message is
    what the program claims, and the pad labels are what it did. Checking both
    against the fixture is what makes a disagreement between them visible.
    """
    match = re.search(r"transient: (\d+) slices", painted)
    return int(match.group(1)) if match else 0


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
    workdir = Path(tempfile.mkdtemp(prefix="cratedig-chop-"))
    fixture = workdir / "cratedig_chop_fixture.wav"
    write_chop_fixture(fixture)

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
        # Before the chop: one pad holds the whole file. If s01 were already on
        # screen the assertions after the chop would prove nothing.
        if "s01" in screen_now():
            failures.append("slice labels are on the pads before anything was chopped")

        # \r, not \n: CR is the byte an Enter key produces on a pty in raw mode.
        alive = send(":chop transient\r")
        chopped = screen_now()

        # The detector ran on the real file and found the hits that were built
        # into it. This is the assertion the whole script exists for -- and the
        # one that fails if analysis is silently not wired to the command.
        found = slice_count_on(chopped)
        if found != NUM_HITS:
            failures.append(f":chop transient reported {found} slices, expected {NUM_HITS}")

        # The chops reached the pads. Eight hits go to pads 1..8, so s01 and s08
        # are labelled and pad 9 is not.
        for expect in ("s01", "s08"):
            if expect not in chopped:
                failures.append(f"after :chop transient the pad grid has no {expect!r}")
        if "s09" in chopped:
            failures.append("a ninth slice reached the pads; the fixture has eight hits")

        if not alive:
            failures.append("the program exited during the chop")

    if alive:
        # Play the chops. "q" is pad 1 and "e" is pad 3, both now holding a
        # slice, so this is the acceptance's "play chops" through the real key
        # path -- the same path a player uses, on a program that has just run its
        # own analysis.
        alive = send("q")
        played = screen_now()

        # The answer to the chop is cleared by the next keystroke, so it can
        # never be read as a reply to something else. The string here is the one
        # THIS session's command produced -- checking for another command's
        # message would be a test that passes because it asks nothing.
        if f"{NUM_HITS} slices" in played:
            failures.append("the chop message survived the next keystroke")
        if "esc quit" not in played:
            failures.append("the keymap did not come back after the message went")

        alive = send("e")

        # Into EDIT on the played slice and back out, so the analysis-produced
        # boundaries are shown by the screen built to show them. `:chop grid`
        # boundaries are round numbers; these are not, which is the case where a
        # formatting bug in the readout actually surfaces.
        alive = send("\r")
        opened = screen_now()
        if "  edit " not in opened:
            failures.append("Enter did not open EDIT after a transient chop")
        if "snapped" not in opened and "free" not in opened:
            failures.append("EDIT does not report what the zero-crossing snap did")

        alive = send("\x1b")
        if "  perform " not in screen_now():
            failures.append("escape did not return to PERFORM")

    if alive:
        # Escape quits from PERFORM. "q" is pad 1 from M3 and would only play it.
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
        print("chop e2e session FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        print("\n--- last painted frame ---", file=sys.stderr)
        print(tail.render(), file=sys.stderr)
        return 1

    print(f"chop e2e session ok: {NUM_HITS} transients chopped and played, exit 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
