#!/usr/bin/env python3
"""Drive cratedig under a pty that ANSWERS the Kitty keyboard query.

tests/tui/pty_session.py is the other half of this pair and covers the legacy
path by simply never replying -- which is what a terminal without the protocol
does, so that test keeps working unchanged and keeps proving the fallback.

This one plays the part of a terminal that implements the protocol: it answers
`CSI ? u`, checks that cratedig then pushes the flags it documented, and drives
the whole session in CSI-u from there. That matters because once the protocol is
on, EVERY keystroke arrives that way -- FTXUI's own Event::Character never fires
again -- so this is the only test that can show the interface is still usable.

The three things only a pty can check:

  1. Nothing is enabled before the terminal has answered. A pushed flag set on a
     terminal that does not understand it is a keyboard nobody can use.
  2. The colon reaches the command line. The protocol reports the UNSHIFTED key,
     so `:` arrives as `;` with the text alongside; a decoder that ignored the
     text field would leave the command line unreachable by the key that opens
     it.
  3. The flags are popped on the way out, while the alternate screen is still
     up.

WHAT THIS CANNOT CHECK, and why: it runs with --no-audio, so render() is never
called and no voice ever starts. A held pad therefore has no audible or
countable effect here. That a note-off releases a gate voice and leaves a
one-shot alone is unit-tested (tests/unit/voice_pool_test.cpp); what is checked
here is that holding and releasing a pad drives the real binary without
wedging it.

Usage: pty_kitty_session.py <cratedig-binary>
"""

from __future__ import annotations

import fcntl
import os
import pty
import shutil
import struct
import sys
import tempfile
import termios
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

# The terminal replay, the reader and the fixture are the same ones the legacy
# session uses. Sharing them is the point: two copies would drift, and the grid
# reconstruction is the part with the bugs in it.
from pty_session import (  # noqa: E402
    COLUMNS,
    EXIT_SECONDS,
    KEY_SECONDS,
    ROWS,
    SETTLE_SECONDS,
    Grid,
    drain,
    write_fixture_wav,
)

QUERY = "\x1b[?u"
PUSH = "\x1b[>27u"
POP = "\x1b[<u"


def csi_u(key: int, *, modifiers: int = 1, event: int = 1, text: int = 0) -> str:
    """One key event, in the protocol's own encoding.

    Written out in full rather than in the shortest form a real terminal would
    choose, because the decoder has to accept the full form and the unit tests
    already cover the abbreviations.
    """
    if text:
        return f"\x1b[{key};{modifiers}:{event};{text}u"
    return f"\x1b[{key};{modifiers}:{event}u"


def typed(text: str) -> str:
    """A string, as the press/release pairs a terminal would send for it."""
    out = ""
    for character in text:
        code = ord(character)
        if character.isupper() or character in ":+_":
            # A shifted key: the protocol reports the UNSHIFTED codepoint with
            # the typed character in the text field. `:` is Shift+semicolon on a
            # US layout, which is exactly the case that makes the text field
            # necessary.
            unshifted = {":": ord(";"), "+": ord("="), "_": ord("-")}.get(
                character, ord(character.lower())
            )
            out += csi_u(unshifted, modifiers=2, text=code)
            out += csi_u(unshifted, modifiers=2, event=3)
        else:
            out += csi_u(code, text=code)
            out += csi_u(code, event=3)
    return out


def legacy_flag_phase(binary: Path, fixture: Path) -> list[str]:
    """--legacy-keys must not even ASK.

    The escape hatch is only an escape hatch if it works on a terminal that
    would have answered -- which is the terminal this file is playing. A version
    that asked anyway and merely ignored the reply would pass every other check
    here.
    """
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.execv(str(binary), [str(binary), "--no-audio", "--legacy-keys", str(fixture)])

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLUMNS, 0, 0))
    output = bytearray()
    alive = drain(fd, SETTLE_SECONDS, output)

    failures: list[str] = []
    seen = output.decode("utf-8", "replace")
    if QUERY in seen:
        failures.append("--legacy-keys still asked the terminal about the protocol")
    if PUSH in seen:
        failures.append("--legacy-keys still pushed the keyboard flags")

    if alive:
        os.write(fd, b"\x1b")  # a bare escape, which is all the legacy path has
    drain(fd, EXIT_SECONDS, output)
    _, status = os.waitpid(pid, 0)
    if os.waitstatus_to_exitcode(status) != 0:
        failures.append("--legacy-keys session did not exit cleanly")
    os.close(fd)
    return failures


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <cratedig-binary>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print(f"error: no such binary {binary}", file=sys.stderr)
        return 1

    workdir = Path(tempfile.mkdtemp(prefix="cratedig-kitty-"))
    fixture = workdir / "cratedig_pty_fixture.wav"
    write_fixture_wav(fixture)

    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ.pop("NO_COLOR", None)
        os.environ.pop("CRATEDIG_UPDATE_SNAPSHOTS", None)
        os.execv(str(binary), [str(binary), "--no-audio", str(fixture)])

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLUMNS, 0, 0))

    output = bytearray()
    alive = drain(fd, SETTLE_SECONDS, output)
    failures: list[str] = []

    def seen() -> str:
        return output.decode("utf-8", "replace")

    def screen_now() -> str:
        grid = Grid(COLUMNS, ROWS)
        grid.feed(seen())
        return grid.render()

    def send(text: str, seconds: float = KEY_SECONDS) -> bool:
        os.write(fd, text.encode())
        return drain(fd, seconds, output)

    # 1. The query goes out, and NOTHING is pushed before it is answered.
    before_reply = seen()
    if QUERY not in before_reply:
        failures.append("cratedig never asked the terminal about the keyboard protocol")
    if PUSH in before_reply:
        failures.append("the flags were pushed before the terminal answered the query")

    # 2. Answer it, as a terminal with the protocol implemented and nothing
    #    currently enabled would.
    if alive:
        alive = send("\x1b[?0u")
    if PUSH not in seen():
        failures.append(f"after the reply, cratedig did not push {PUSH!r}")

    # 3. Drive the interface entirely in CSI-u. Every keystroke arrives this way
    #    now, so anything that still works proves the decoder feeds the same
    #    bindings the legacy path does.
    if alive:
        # The colon. Shift+semicolon, reported as `;` with `:` in the text field
        # -- the case the "report associated text" flag exists for.
        alive = send(typed(":chop grid 4"))
        if ":chop grid 4" not in screen_now():
            failures.append("the command line did not receive what CSI-u typed")

        # Enter, as its own key code rather than as a byte.
        alive = send(csi_u(13) + csi_u(13, event=3))
        chopped = screen_now()
        for expect in ("4 slices", "s01", "s04"):
            if expect not in chopped:
                failures.append(f"after :chop grid 4 the screen has no {expect!r}")

    # 4. Gate mode, which is the whole reason for negotiating any of this.
    if alive:
        alive = send(typed(":pad gate") + csi_u(13) + csi_u(13, event=3))
        gated = screen_now()
        if "gate" not in gated:
            failures.append("`:pad gate` said nothing")
        if "play through" in gated:
            failures.append(
                "`:pad gate` warned about missing key release on a terminal that has it"
            )

    # 5. A held pad. Press without release, then release -- neither may crash or
    #    hang, and the repeat in between must not be taken for a second hit.
    if alive:
        alive = send(csi_u(ord("q")) + csi_u(ord("q"), event=2) + csi_u(ord("q"), event=3))
        if not alive:
            failures.append("the program exited while a pad was held")

    # 6. Escape, which under this protocol is a key code like any other.
    if alive:
        os.write(fd, csi_u(27).encode())
    drain(fd, EXIT_SECONDS, output)

    tail = Grid(COLUMNS, ROWS)
    tail.feed(seen())

    _, status = os.waitpid(pid, 0)
    exit_code = os.waitstatus_to_exitcode(status)
    os.close(fd)

    if exit_code != 0:
        failures.append(f"exit code {exit_code}, expected 0")

    # 7. The flags are popped, and popped BEFORE the alternate screen is left --
    #    the stack is per-screen, so the other order would pop the shell's.
    everything = seen()
    if POP not in everything:
        failures.append(f"the keyboard flags were never popped ({POP!r} not sent)")
    elif everything.index(POP) > everything.rindex("\x1b[?1049l"):
        failures.append("the flags were popped after leaving the alternate screen")
    if not tail.saw_alt_screen_exit:
        failures.append("the program never left the alternate screen buffer")

    # 8. And the escape hatch, against the same answering terminal.
    failures += legacy_flag_phase(binary, fixture)

    shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print("PTY kitty session FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        print("\n--- painted frame ---", file=sys.stderr)
        print(tail.render(), file=sys.stderr)
        return 1

    print(f"PTY kitty session ok: negotiated, driven in CSI-u, popped, exit {exit_code}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
