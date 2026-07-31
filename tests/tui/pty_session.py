#!/usr/bin/env python3
"""Drive the real cratedig binary under a pseudo-terminal and check what it paints.

This is the test the offscreen snapshots structurally cannot be: it proves the
binary starts, negotiates a real terminal, responds to real keystrokes, paints a
frame, and gives the terminal back. tests/tui/layout_snapshot_test.cpp proves the
layout is right; this proves there is a program around it.

WHY THERE IS A TERMINAL EMULATOR IN HERE
----------------------------------------
FTXUI redraws differentially. After the first frame it emits cursor moves and a
handful of changed cells, so the byte stream with the escape codes stripped out
is not the screen -- it is the first screen plus a pile of edits. Reconstructing
the grid is the only way to assert on what a person would actually see, and it
is about eighty lines. The alternative, asserting on the raw stream, would pass
just as happily if every frame after the first were garbage.

Usage: pty_session.py <cratedig-binary> <snapshot-dir>
Set CRATEDIG_UPDATE_SNAPSHOTS=1 to rewrite the golden instead of comparing.
"""

from __future__ import annotations

import fcntl
import os
import pty
import re
import select
import shutil
import struct
import sys
import tempfile
import termios
import time
import wave
from pathlib import Path

COLUMNS = 100
ROWS = 30

# Long enough for FTXUI to install, paint, and settle. The interface is static
# under --no-audio, so these only have to be generous, not precise.
SETTLE_SECONDS = 1.2
KEY_SECONDS = 0.35
EXIT_SECONDS = 3.0


class Grid:
    """The subset of a terminal cratedig actually drives."""

    def __init__(self, columns: int, rows: int) -> None:
        self.columns = columns
        self.rows = rows
        self.cells = [[" "] * columns for _ in range(rows)]
        self.x = 0
        self.y = 0
        self.saw_alt_screen_exit = False

    def _put(self, char: str) -> None:
        if 0 <= self.y < self.rows and 0 <= self.x < self.columns:
            self.cells[self.y][self.x] = char
        self.x += 1
        if self.x >= self.columns:  # FTXUI does not rely on autowrap, but be safe
            self.x = self.columns - 1

    def _csi(self, params: str, final: str) -> None:
        private = params.startswith("?")
        numbers = [int(part) if part else 0 for part in params.lstrip("?<>=").split(";") if part != ""]

        def arg(index: int, default: int = 1) -> int:
            return numbers[index] if index < len(numbers) and numbers[index] != 0 else default

        if private:
            # ?1049l leaves the alternate screen: the thing that puts a user's
            # terminal back the way they left it.
            if final == "l" and 1049 in numbers:
                self.saw_alt_screen_exit = True
            return

        if final == "H" or final == "f":
            self.y = (numbers[0] - 1) if numbers and numbers[0] else 0
            self.x = (numbers[1] - 1) if len(numbers) > 1 and numbers[1] else 0
        elif final == "A":
            self.y -= arg(0)
        elif final == "B":
            self.y += arg(0)
        elif final == "C":
            self.x += arg(0)
        elif final == "D":
            self.x -= arg(0)
        elif final == "G":
            self.x = arg(0) - 1
        elif final == "J":
            mode = numbers[0] if numbers else 0
            if mode == 2:
                self.cells = [[" "] * self.columns for _ in range(self.rows)]
            elif mode == 0:
                for x in range(self.x, self.columns):
                    self.cells[self.y][x] = " "
                for y in range(self.y + 1, self.rows):
                    self.cells[y] = [" "] * self.columns
        elif final == "K":
            mode = numbers[0] if numbers else 0
            if 0 <= self.y < self.rows:
                if mode == 0:
                    for x in range(self.x, self.columns):
                        self.cells[self.y][x] = " "
                elif mode == 2:
                    self.cells[self.y] = [" "] * self.columns
        # SGR (m) and everything else changes styling, not content.

        self.x = max(0, min(self.x, self.columns - 1))
        self.y = max(0, min(self.y, self.rows - 1))

    def feed(self, text: str) -> None:
        index = 0
        while index < len(text):
            char = text[index]
            if char == "\x1b":
                rest = text[index + 1 :]
                if rest.startswith("["):
                    match = re.match(r"\[([0-9;?<>=]*)([A-Za-z@`])", rest)
                    if match:
                        self._csi(match.group(1), match.group(2))
                        index += 1 + match.end()
                        continue
                if rest.startswith("]"):  # OSC, terminated by BEL or ST
                    end = re.search(r"(\x07|\x1b\\)", rest)
                    index += 1 + (end.end() if end else len(rest))
                    continue
                if rest[:1] in ("(", ")", "#"):
                    index += 3
                    continue
                index += 2  # ESC = , ESC > and friends
                continue
            if char == "\r":
                self.x = 0
            elif char == "\n":
                self.y = min(self.y + 1, self.rows - 1)
            elif char == "\b":
                self.x = max(0, self.x - 1)
            elif char == "\t":
                self.x = min(((self.x // 8) + 1) * 8, self.columns - 1)
            elif char >= " ":
                self._put(char)
            index += 1

    def render(self) -> str:
        return "\n".join("".join(row) for row in self.cells)


def write_fixture_wav(path: Path) -> None:
    """A deterministic 48 kHz WAV, written here rather than fetched.

    48 kHz on purpose: it matches the engine rate, so load_sample() takes the
    resampler's bit-exact passthrough and the frame this test compares against
    cannot depend on a libsamplerate or FFmpeg build. The starter pack is not
    used for the same reason a decoder test does not use it -- this must never
    skip.
    """
    rate = 48_000
    frames = rate * 2
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        samples = bytearray()
        for frame in range(frames):
            beat = (frame % (rate // 2)) / (rate / 2.0)
            envelope = (1.0 - beat) ** 3
            # An integer recurrence rather than sin(): no libm is involved, so
            # the bytes are identical on every platform by construction.
            tone = 1.0 if (frame // 60) % 2 == 0 else -1.0
            value = int(28_000 * envelope * tone)
            samples += struct.pack("<h", value)
        out.writeframes(bytes(samples))


def start_time_on(painted: str) -> str:
    """The EDIT screen's `start N.NNNs` readout, or "" if it is not up.

    Read off the painted grid rather than tracked alongside it: the point is
    that a nudge reaches the screen, and a value this test computed itself would
    prove only that this test can add.
    """
    match = re.search(r"start (\d+\.\d+)s", painted)
    return match.group(1) if match else ""


def drain(fd: int, seconds: float, sink: bytearray) -> bool:
    """Reads for `seconds`. Returns False once the child has closed the pty.

    Bytes, not text. A read boundary lands wherever the kernel put it, and the
    interface is full of three-byte box-drawing and braille characters, so
    decoding each chunk on its own turns whichever character got split into a
    replacement character. That failure is intermittent by nature -- it depends
    on timing -- which is exactly the kind that survives a green test run.
    """
    deadline = time.time() + seconds
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            return False
        if not chunk:
            return False
        sink += chunk
    return True


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <cratedig-binary> <snapshot-dir>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    snapshot_dir = Path(sys.argv[2])
    updating = os.environ.get("CRATEDIG_UPDATE_SNAPSHOTS", "") not in ("", "0")

    if not binary.is_file():
        print(f"error: no such binary {binary}", file=sys.stderr)
        return 1

    # A unique DIRECTORY with a fixed filename inside it. Running this under
    # several presets at once must not have one run rewriting another's fixture
    # mid-decode -- but the name reaches the screen, in the header and in the pad
    # label, so putting the uniqueness in the path instead of the name is what
    # keeps the snapshot a snapshot. Found by running it three times in a row.
    workdir = Path(tempfile.mkdtemp(prefix="cratedig-pty-"))
    fixture = workdir / "cratedig_pty_fixture.wav"
    write_fixture_wav(fixture)

    pid, fd = pty.fork()
    if pid == 0:
        # The child. A fixed, unremarkable terminal type and no NO_COLOR, so the
        # program is not being asked to behave differently from a normal run.
        os.environ["TERM"] = "xterm-256color"
        os.environ.pop("NO_COLOR", None)
        os.environ.pop("CRATEDIG_UPDATE_SNAPSHOTS", None)
        os.execv(str(binary), [str(binary), "--no-audio", str(fixture)])

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLUMNS, 0, 0))
    try:
        attributes_before = termios.tcgetattr(fd)
    except termios.error:
        attributes_before = None

    output = bytearray()
    alive = drain(fd, SETTLE_SECONDS, output)

    # A short session that touches every part of the interface this test can
    # reach: trigger pads on two different QWERTY keys, zoom in twice, scroll
    # right, switch panel tab.
    #
    # "w" is pad 2, which is unloaded, so it produces no sound and no glow --
    # deliberately, because a silent unloaded pad still has to be a no-op rather
    # than an error, and this is where that gets exercised on the real binary.
    for key in (" ", "q", "w", "+", "+", "l", "\t"):
        if not alive:
            break
        os.write(fd, key.encode())
        alive = drain(fd, KEY_SECONDS, output)

    frame = Grid(COLUMNS, ROWS)
    frame.feed(output.decode("utf-8", "replace"))
    painted = frame.render()

    def send(text: str) -> bool:
        os.write(fd, text.encode())
        return drain(fd, KEY_SECONDS, output)

    def screen_now() -> str:
        grid = Grid(COLUMNS, ROWS)
        grid.feed(output.decode("utf-8", "replace"))
        return grid.render()

    # The command line, driven through a real terminal.
    #
    # The offscreen snapshots prove the prompt DRAWS; only this can prove the
    # keystrokes reach it. Two things here are invisible to any test that builds
    # a UiState by hand: Enter arrives as CR rather than LF once raw mode clears
    # ICRNL, and Backspace arrives as DEL. FTXUI normalises both -- this is
    # where that keeps being true. Bytes are what a pty has.
    command_failures: list[str] = []
    if alive:
        alive = send(":chop grid 4x")
        prompt = screen_now()
        if ":chop grid 4x" not in prompt:
            command_failures.append("the prompt did not show what was typed")
        if "esc quit" in prompt:
            command_failures.append("the keymap is still on the mode line under the prompt")

        # DEL removes the typo; escape then cancels the whole line. Cancelling
        # must NOT quit -- everything after this depends on the program still
        # being there.
        alive = send("\x7f\x1b")
        if ":chop" in screen_now():
            command_failures.append("escape did not cancel the prompt")

        # \r, not \n: CR is the byte an actual Enter key produces here.
        alive = send(":chop grid 4\r")
        chopped = screen_now()
        for expect in ("4 slices", "s01", "s04"):
            if expect not in chopped:
                command_failures.append(f"after :chop grid 4 the screen has no {expect!r}")
        if not alive:
            command_failures.append("the program exited during the chop")

        # The answer is cleared by the next keystroke, so it can never be read
        # as a reply to something else. "q" is pad 1, which now holds slice 1.
        alive = send("q")
        after = screen_now()
        if "4 slices" in after:
            command_failures.append("the message survived the next keystroke")
        if "esc quit" not in after:
            command_failures.append("the keymap did not come back after the message went")

        # EDIT, on the real binary. Enter opens it on the selected pad's slice;
        # the boundary nudges and the slice step are the reason the screen
        # exists, and nothing but a running program can show that the keys reach
        # them -- the offscreen snapshots are handed a UiState that has already
        # been edited.
        alive = send("\r")
        opened = screen_now()
        if "  edit " not in opened:
            command_failures.append("Enter did not open EDIT")
        # The `┻` is the handle marker itself; the `h -1` / `+1 l` labels beside
        # it are trimmed when a boundary is near the edge of the panel, which is
        # exactly where slice 1 puts it.
        if "┻" not in opened:
            command_failures.append("EDIT opened without its boundary handles")

        # `]` steps to slice 2 and `l` nudges its start one frame later. The
        # start time on screen has to move, and `u` has to put it back.
        alive = send("]")
        stepped = screen_now()
        if "slice 02" not in stepped:
            command_failures.append("`]` did not step to the next slice")

        # 200 single-frame nudges is 4 ms at 48 kHz, which moves the third
        # decimal of the readout by four. Forty would move it by one, and a
        # rounding boundary could then hide the whole effect.
        before_nudge = start_time_on(stepped)
        alive = send("l" * 200)
        nudged = screen_now()
        if start_time_on(nudged) == before_nudge:
            command_failures.append("nudging the start boundary changed nothing on screen")
        if "undo" not in nudged:
            command_failures.append("nudging left nothing to undo")

        alive = send("u" * 200)
        undone = screen_now()
        if start_time_on(undone) != before_nudge:
            command_failures.append(
                f"undo did not restore the boundary: {start_time_on(undone)!r} "
                f"!= {before_nudge!r}"
            )

        # Escape LEAVES EDIT rather than quitting. Everything after this line
        # depends on the program still being there.
        alive = send("\x1b")
        if "  perform " not in screen_now():
            command_failures.append("escape did not return to PERFORM from EDIT")

    if alive:
        # ESCAPE, not "q" -- the QWERTY pad map claims q for pad 1 from M3, and
        # a session that sent "q" here would trigger a pad and then hang.
        os.write(fd, b"\x1b")
    drain(fd, EXIT_SECONDS, output)

    # `frame` is the screen as it stood before quitting. `tail` replays the whole
    # session including the teardown, which is where the alternate-screen exit
    # lives -- the sequence that decides whether a user gets their terminal back.
    tail = Grid(COLUMNS, ROWS)
    tail.feed(output.decode("utf-8", "replace"))

    _, status = os.waitpid(pid, 0)
    exit_code = os.waitstatus_to_exitcode(status)

    try:
        attributes_after = termios.tcgetattr(fd)
    except termios.error:
        attributes_after = None
    os.close(fd)

    failures: list[str] = list(command_failures)

    if exit_code != 0:
        failures.append(f"exit code {exit_code}, expected 0")

    if not tail.saw_alt_screen_exit:
        failures.append("the program never left the alternate screen buffer "
                        "(no CSI ?1049l) -- it would leave a user's terminal wrong")

    if attributes_before is not None and attributes_after is not None:
        # Index 3 is c_lflag, which is where ICANON and ECHO live: the two a
        # terminal program turns off and must turn back on.
        if attributes_before[3] != attributes_after[3]:
            failures.append(
                f"terminal c_lflag not restored: {attributes_before[3]} -> {attributes_after[3]}"
            )

    snapshot = snapshot_dir / "pty_session_100x30.txt"
    if updating:
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.write_text(painted, encoding="utf-8")
        print(f"wrote {snapshot}")
    elif not snapshot.is_file():
        failures.append(f"missing snapshot {snapshot}; run scripts/update_tui_snapshots.sh")
    else:
        expected = snapshot.read_text(encoding="utf-8")
        if painted != expected:
            failures.append("the painted frame does not match the snapshot")
            expected_rows = expected.split("\n")
            actual_rows = painted.split("\n")
            for index in range(max(len(expected_rows), len(actual_rows))):
                want = expected_rows[index] if index < len(expected_rows) else "<missing>"
                got = actual_rows[index] if index < len(actual_rows) else "<missing>"
                if want != got:
                    failures.append(f"  row {index}:\n    want |{want}|\n    got  |{got}|")

    shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print("PTY session FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        print("\n--- painted frame ---", file=sys.stderr)
        print(painted, file=sys.stderr)
        return 1

    print(f"PTY session ok: {COLUMNS}x{ROWS}, exit 0, terminal restored")
    return 0


if __name__ == "__main__":
    sys.exit(main())
