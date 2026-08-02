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


def decode_stream(data: bytes | bytearray) -> str:
    """Decode the captured stream, dropping a TRUNCATED TAIL rather than corrupting it.

    drain() stops on a wall clock, and the screen is reconstructed from the bytes
    captured so far -- so on a slow machine it can cut the stream in the middle of
    a three-byte box-drawing character. Decoded with errors="replace" that becomes
    a replacement glyph painted at wherever the cursor had reached, which looks
    exactly like the program painting garbage into the middle of the frame.

    It is not: the character completes on the next read, and the full stream
    decodes strictly clean under errors="strict". It only ever failed in the
    Docker CI, because macOS was fast enough that the window always ended on a
    character boundary -- and it cost a while to find because the evidence (one
    bad glyph, in the same cell every run) reads as a rendering bug rather than
    as a measurement one.

    An invalid sequence anywhere but the last few bytes is real corruption and is
    left visible as one. A decoder that hid those would hide the bug this exists
    not to be mistaken for.
    """
    view = bytes(data)
    while view:
        try:
            return view.decode("utf-8")
        except UnicodeDecodeError as error:
            if error.start < len(view) - 3:  # 4 bytes is the longest sequence
                return view.decode("utf-8", "replace")
            view = view[: error.start]
    return ""


def lane_cells(painted: str) -> dict[int, str]:
    """The pattern lane, as {pad number: its step cells}.

    Matched on a two-digit label followed by groups of step cells, which is the
    lane and nothing else -- the pad grid's own labels are followed by a name,
    not by dots. Each lane ROW carries two pads (1-8 on the left, 9-16 on the
    right) and also has the pad grid to its left on the same screen line, so
    picking rows out is not enough: the pad has to come from the label beside the
    cells, which is the whole point of reading it this way.
    """
    found: dict[int, str] = {}
    for match in re.finditer(r"(\d\d) ([█·]{4}(?: [█·]{4})*)", painted):
        found[int(match.group(1))] = match.group(2)
    return found


def steps_on(painted: str) -> int:
    """How many steps are lit across the whole lane."""
    return sum(cells.count("█") for cells in lane_cells(painted).values())


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
    # reach: trigger pads on two different keys, zoom in twice, scroll right,
    # switch panel tab.
    #
    # No space here any more -- it is the transport as of M4.5, and starting the
    # transport before the snapshot is taken would put a `play` on the mode line
    # that has nothing to do with what this frame is meant to show.
    #
    # "2" is pad 2, which is unloaded, so it produces no sound and no glow --
    # deliberately, because a silent unloaded pad still has to be a no-op rather
    # than an error, and this is where that gets exercised on the real binary.
    #
    # The number row is the TOP row of the map as of M4.5: `1` is pad 1 and `q`
    # is pad 5. This session used to press `q` for pad 1 and, after a four-slice
    # chop, land on a pad with nothing on it -- which is how the rotation proved
    # it had actually taken effect.
    for key in ("1", "2", "+", "+", "l", "\t"):
        if not alive:
            break
        os.write(fd, key.encode())
        alive = drain(fd, KEY_SECONDS, output)

    frame = Grid(COLUMNS, ROWS)
    frame.feed(decode_stream(output))
    painted = frame.render()

    def send(text: str) -> bool:
        os.write(fd, text.encode())
        return drain(fd, KEY_SECONDS, output)

    def screen_now() -> str:
        grid = Grid(COLUMNS, ROWS)
        grid.feed(decode_stream(output))
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
        # as a reply to something else. "1" is pad 1, which now holds slice 1.
        alive = send("1")
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

    # The sequencer, driven through the same terminal.
    #
    # Everything checked here is CONTROL-SIDE state coming back through the
    # engine's published sequencer, which is what makes it visible under
    # --no-audio: nothing calls render(), so the transport never advances and no
    # telemetry is ever published. That is also why `p` is checked by the
    # message it prints rather than by the mode line changing to "play" -- with
    # no device the transport genuinely does not run, and the program says so
    # instead of pretending.
    if alive:
        # Off the lane first, whichever panel the session left up, so that the
        # step key has to bring it back by itself.
        #
        # BACKSLASH, NOT TAB, and unconditionally. This was `if on the lane:
        # press tab`, which meant the assertion after it never ran when the lane
        # was already down -- and Tab has never reached the program at all
        # (FTXUI consumes it before CatchEvent), so the panel switch had been
        # dead since M2 behind a check that could not fail. Pressing the key
        # every time is what makes the next line an assertion.
        alive = send("\\")
        if "pattern 01" in screen_now():
            alive = send("\\")
        if "pattern 01" in screen_now():
            command_failures.append("\\ did not leave the pattern lane")

        # `t` BRINGS THE LANE UP as well as writing a step. A step key that
        # edited a grid nobody could see would be a way to write a pattern you
        # did not mean to.
        alive = send("t")
        if "pattern 01" not in screen_now():
            command_failures.append("`t` did not bring the pattern lane up")

        # `1` selected pad 1 earlier, so the step lands on its row. The toggle
        # prints no message on purpose -- the lane is where the answer is, and
        # this is the only test that can say the lane really redraws.
        #
        # Counted as DIFFERENCES rather than absolutes: a stray block character
        # elsewhere on the screen shifts every count equally.
        lit = screen_now().count("█")
        alive = send("t")
        empty = screen_now().count("█")
        if lit != empty + 1:
            command_failures.append(f"`t` did not turn one step on and off again ({lit} vs {empty})")

        # `]` moves the cursor, so the second `t` writes a DIFFERENT step. Two
        # toggles of one step land back where they started, which a single
        # toggle cannot tell apart from a cursor that never moved.
        alive = send("t]]]]t")
        if screen_now().count("█") != empty + 2:
            command_failures.append("`]` then `t` did not write a second, separate step")

        # The tempo, round-tripped through the fixed-point parse. `92.5` must
        # come back as `92.50` and not as `92.49`.
        alive = send(":bpm 92.5\r")
        if "92.50" not in screen_now():
            command_failures.append("`:bpm 92.5` did not answer with 92.50")

        # And it reaches the mode line once the message clears, which is the
        # part that proves the edit was published rather than merely parsed.
        alive = send("q")
        if "92.50 bpm" not in screen_now():
            command_failures.append("the tempo did not reach the mode line")

        alive = send(":metro on\r")
        if "metronome on" not in screen_now():
            command_failures.append("`:metro on` was not acknowledged")

        alive = send(":pattern 3\r")
        after_select = screen_now()
        if "pattern 3" not in after_select:
            command_failures.append("`:pattern 3` was not acknowledged")
        alive = send("q")
        selected = screen_now()
        if "pattern 03" not in selected:
            command_failures.append("the lane did not follow `:pattern 3`")
        # A different pattern is an EMPTY one, so the steps written above must
        # not be showing on it. Chained onto the check above rather than run
        # independently: with no caption to split on this would raise, and a
        # harness that crashes on the failure path reports nothing useful --
        # including whatever else was wrong.
        elif "█" in selected.split("pattern 03", 1)[1]:
            command_failures.append("the newly selected pattern shows the old pattern's steps")

        # A refusal has to look like one, on the real binary.
        alive = send(":bpm 900\r")
        if "outside" not in screen_now():
            command_failures.append("`:bpm 900` was not refused")

        # THE TRANSPORT IS SPACE, and space is no longer a pad. Both halves are
        # asserted, because either alone would pass on a build that had done only
        # one of them -- a space that still triggered pad 1 *and* ran the
        # transport would answer the message check perfectly.
        #
        # With no device nothing renders, so the honest answer to pressing play
        # is to say so rather than to light up a transport that cannot move,
        # which is also what makes the message a usable signal here.
        #
        # The lane is up from the step edits above, so a space that still
        # selected a pad would move the cursor's row -- and the way to see that
        # is to WRITE with it afterwards and look at which row lit up. Counting
        # steps alone would not: selecting a pad lights nothing by itself.
        alive = send("z")  # pad 13, a row nothing has written to yet
        alive = send(" ")
        if "no audio device" not in screen_now():
            command_failures.append("space did not say why the transport cannot run")

        alive = send("t")
        landed = [pad for pad, cells in lane_cells(screen_now()).items() if "█" in cells]
        if 13 not in landed:
            command_failures.append(
                f"space moved the pad selection off 13 before the step was written: {landed}"
            )
        alive = send("t")  # and take it back off

        # `p` is the alias and has to keep working; M6 takes it back for a pad.
        alive = send("p")
        if "no audio device" not in screen_now():
            command_failures.append("`p` no longer runs the transport")

        # THE PANIC. Nothing is audible under --no-audio, so what is checked here
        # is that the key and the verb both reach the program and say so -- the
        # audio itself is asserted in engine_render_test.cpp, where voices exist.
        alive = send(".")
        if "stopped everything" not in screen_now():
            command_failures.append("`.` did not stop everything")

        alive = send(":stop 3\r")
        if "stopped pad 3" not in screen_now():
            command_failures.append("`:stop 3` did not stop one pad")

        alive = send(":stop\r")
        if "stopped everything" not in screen_now():
            command_failures.append("`:stop` did not stop everything")

        alive = send(":stop 99\r")
        if "no pad 99" not in screen_now():
            command_failures.append("`:stop 99` was not refused")

    if alive:
        # A SLICE THAT IS NOT ON A PAD. Twenty chops onto sixteen pads leaves
        # four that nothing can play: every route to sound is a pad trigger, so
        # a slice no pad holds is editable, drawable and silent.
        #
        # That is a real limit until M5.5 gives auditioning its own path. What is
        # checked here is that EDIT SAYS SO -- the key did nothing at all before,
        # which reads as EDIT being broken on those slices rather than as the
        # slice being unreachable.
        alive = send(":chop grid 20\r")
        if "20 slices" not in screen_now():
            command_failures.append(":chop grid 20 did not report twenty slices")

        alive = send(":edit 20\r")
        if "slice 20/20" not in screen_now():
            command_failures.append("`:edit 20` did not open EDIT on slice 20")

        alive = send(" ")
        unreachable = screen_now()
        if "not on a pad" not in unreachable:
            command_failures.append("auditioning a slice with no pad said nothing")
        if ":slot assign 20" not in unreachable:
            command_failures.append("the message does not say how to reach the slice")

        # And a slice that IS on a pad still auditions silently -- no message,
        # because nothing went wrong. Without this the check above would pass on
        # a build that had simply broken auditioning altogether.
        alive = send("\x1b")
        alive = send(":edit 3\r")
        alive = send(" ")
        if "not on a pad" in screen_now():
            command_failures.append("auditioning slice 3, which is on pad 3, was refused")

        alive = send("\x1b")  # out of EDIT before the quit below

    if alive:
        # ESCAPE, not "q" -- the pad map claims every letter it uses, and a
        # session that sent one here would trigger a pad and then hang.
        os.write(fd, b"\x1b")
    drain(fd, EXIT_SECONDS, output)

    # `frame` is the screen as it stood before quitting. `tail` replays the whole
    # session including the teardown, which is where the alternate-screen exit
    # lives -- the sequence that decides whether a user gets their terminal back.
    tail = Grid(COLUMNS, ROWS)
    tail.feed(decode_stream(output))

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
