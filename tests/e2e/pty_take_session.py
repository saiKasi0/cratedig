#!/usr/bin/env python3
"""Arming a take, and seeing that the machine is listening.

Covers BOTH recording features, because the one thing that has to stay true of
them is that they are separate: `R` and `:rec` play a pattern in, `:capture`
records audio, and they share no verb, no key and no indicator. Testing them in
one session is what makes "these two do not overlap" an assertion rather than an
intention.

tests/unit/live_take_test.cpp proves that hits become steps, and
tests/unit/take_test.cpp proves the quantiser. Both of those run an engine in a
loop with no terminal in the way. Every assertion in them would still pass with
the `R` key deleted, the `:rec` verbs unparsed and the mode line never mentioning
recording at all -- which is the whole feature from where a person is sitting.

WHAT ONLY THIS CAN SEE
----------------------
That the controls exist and reach the state the interface draws from: `R` arms,
`:rec quant` changes what the line says, `replace` announces itself because it is
destructive, and `:rec undo` refuses when there is nothing to put back.

WHAT THIS DELIBERATELY CANNOT SEE
---------------------------------
A note being recorded. With --no-audio nothing calls Engine::render(), so the
transport never advances and a hit has no position to be recorded against -- by
design, and the reason the program SAYS "press space to roll" rather than
silently keeping nothing. Recording a figure needs a rendering engine, which is
what live_take_test.cpp is.

Usage: pty_take_session.py <cratedig-binary>
"""

from __future__ import annotations

import fcntl
import os
import pty
import struct
import sys
import termios
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

# An unbound key in PERFORM. Pressing it clears the last message and does
# nothing else, which is the only way to see the mode line's FACTS -- a message
# takes the whole right-hand side while it is up, counters included.
CLEAR_KEY = "y"


def mode_line(screen: str) -> str:
    """The bottom line, and nothing else.

    BOUNDED ON PURPOSE. docs/TESTING.md's standing rule: an assertion that
    searches the whole screen finds the program's own commentary about what it
    did rather than the thing it did. "rec" appears in a confirmation message,
    in the completion menu and in the mode line, and only one of those is the
    claim being made here.
    """
    for line in reversed(screen.splitlines()):
        if line.strip():
            return line
    return ""


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <cratedig-binary>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        print(f"error: no such binary {binary}", file=sys.stderr)
        return 1

    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ.pop("NO_COLOR", None)
        os.environ.pop("CRATEDIG_UPDATE_SNAPSHOTS", None)
        os.execv(str(binary), [str(binary), "--no-audio"])

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

    def facts() -> str:
        """The mode line with any message cleared out of the way."""
        send(CLEAR_KEY)
        return mode_line(screen_now())

    failures: list[str] = []

    if not alive:
        failures.append("the program exited before the session started")

    if alive:
        # Nothing is recording, and the line says nothing about recording. A
        # fact that is always there is a fact nobody reads.
        if "rec" in facts():
            failures.append(f"the mode line mentions recording before anything armed: {facts()!r}")

    if alive:
        alive = send("R")
        said = mode_line(screen_now())
        # It names the transport, because with nothing rolling an armed
        # recorder keeps nothing and would otherwise look broken.
        if "recording armed" not in said:
            failures.append(f"R did not arm: {said!r}")
        if "space" not in said:
            failures.append(f"arming did not say what to do next: {said!r}")

    if alive:
        line = facts()
        if "rec 1/16" not in line:
            failures.append(f"armed, but the mode line does not show it: {line!r}")

    if alive:
        # The resolution is a setting, and the line has to follow it -- knowing
        # what a take will come out as matters while your hands are on the pads
        # rather than afterwards.
        alive = send(":rec quant 8\r", 1.0)
        line = facts()
        if "rec 1/8" not in line:
            failures.append(f"rec quant 8 did not change what the line shows: {line!r}")

    if alive:
        alive = send(":rec quant 3\r", 1.0)
        said = mode_line(screen_now())
        # Refused rather than rounded, and it names what would have worked.
        if "16" not in said or "does not" in said.lower() and "quant" not in said.lower():
            failures.append(f"rec quant 3 was not refused usefully: {said!r}")

    if alive:
        # REPLACE EARNS ITS OWN CELL because it is destructive: arming with it
        # set throws the pattern away.
        alive = send(":rec replace\r", 1.0)
        line = facts()
        if "replace" not in line:
            failures.append(f"replace is armed but not shown: {line!r}")

        alive = send(":rec overdub\r", 1.0)
        line = facts()
        if "replace" in line:
            failures.append(f"overdub still shows replace: {line!r}")
        if "rec 1/8" not in line:
            failures.append(f"overdub lost the record state entirely: {line!r}")

    if alive:
        # And off again, all the way off -- not merely a different resolution.
        alive = send("R")
        line = facts()
        if "rec 1/" in line:
            failures.append(f"R did not disarm: {line!r}")

    if alive:
        # Undo with nothing to undo says so. THE PATTERN WAS NEVER RECORDED
        # INTO here, so this is the refusal rather than the restore -- the
        # restore needs a rendering engine and lives in live_take_test.cpp.
        alive = send(":rec undo\r", 1.0)
        said = mode_line(screen_now())
        if "nothing to put back" not in said:
            failures.append(f"rec undo with no take did not say so: {said!r}")

    # -- recording AUDIO, which is a different feature ------------------------
    #
    # `capture` and `rec` share nothing: not a verb, not a key, not a line on
    # the mode line. Asserting that here is what keeps them apart, because the
    # one thing this milestone has already proved is that a single word for both
    # is ambiguous enough to build the wrong feature from.

    if alive:
        # REFUSED, because this session has no device and therefore no input.
        # Accepting it would set a mode that records nothing, and the only
        # symptom would be an empty take with nothing to explain it.
        #
        # Asserting the refusal rather than merely "input appears somewhere":
        # the refusal message contains the word too, so the loose check passed
        # for the wrong reason the moment this behaviour was added.
        alive = send(":capture source input\r", 1.0)
        said = mode_line(screen_now())
        if "no audio device" not in said or "input" not in said:
            failures.append(f"capture source input was not refused with a reason: {said!r}")

        alive = send(":capture source master\r", 1.0)
        said = mode_line(screen_now())
        if "master" not in said:
            failures.append(f"capture source master was not acknowledged: {said!r}")

    if alive:
        # ARMING WORKS WITH NO DEVICE, and that is not an accident: the command
        # ring is drained by Engine::adopt_offline() on the frame tick, which is
        # the mechanism that stops a producer with no consumer filling it. So
        # this reaches the recorder's state machine for real.
        alive = send(":capture arm -12\r", 1.0)
        said = mode_line(screen_now())
        if "armed" not in said:
            failures.append(f"capture arm did not arm: {said!r}")
        line = facts()
        if "cap armed" not in line:
            failures.append(f"armed, but the line does not show it: {line!r}")
        # And it is NOT the pattern recorder. Two features, two indicators.
        if "rec 1/" in line:
            failures.append(f"capture armed the pattern recorder as well: {line!r}")

    if alive:
        alive = send(":capture drop\r", 1.0)
        line = facts()
        if "cap" in line:
            failures.append(f"capture drop left the capture running: {line!r}")

    if alive:
        # With --no-audio nothing renders, so nothing can be captured -- and the
        # program says so rather than presenting an empty take as a take.
        alive = send(":capture start\r", 1.0)
        said = mode_line(screen_now())
        if "no audio device" not in said:
            failures.append(f"capture with no device did not say so: {said!r}")

        alive = send(":capture stop\r", 2.0)
        said = mode_line(screen_now())
        if "nothing was recorded" not in said:
            failures.append(f"an empty take was not reported as empty: {said!r}")

    if alive:
        # The menu offers it, which is the only way somebody finds a verb they
        # have not been told about.
        alive = send(":rec", 1.0)
        alive = send("\t", 1.0)
        screen = screen_now()
        if "rec quant" not in screen:
            failures.append("tab did not offer the rec verbs")
        alive = send("\x1b", 1.0)  # close the menu
        alive = send("\x1b", 1.0)  # close the prompt

    if alive:
        send("\x1b", EXIT_SECONDS)

    os.close(fd)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass

    if failures:
        print("take session FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print("\n--- final screen ---", file=sys.stderr)
        print(screen_now(), file=sys.stderr)
        return 1

    print("take session: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
