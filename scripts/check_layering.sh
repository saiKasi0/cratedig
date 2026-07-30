#!/usr/bin/env bash
# Enforce the module-boundary rules from CLAUDE.md that a compiler cannot.
#
# These are the rules that decay quietly. Nothing breaks the day someone
# includes RtAudio.h in the engine "just to get the device enum" -- it builds, it
# runs, and the cost only shows up later when offline export or Docker CI needs
# an engine that works with no sound card present. A grep in CI is cheap; finding
# out at M6 that the engine cannot be built headless is not.
set -euo pipefail

cd "$(dirname "$0")/.."

failed=0

report() {
  echo "error: $1" >&2
  failed=1
}

# Rule: src/io/ is the ONLY module allowed to include the device headers.
device_leaks="$(grep -rn --include='*.hpp' --include='*.cpp' -E '#include *[<"](RtAudio|RtMidi)\.h' src \
  | grep -v '^src/io/' || true)"
if [ -n "${device_leaks}" ]; then
  report "device headers included outside src/io/:"
  echo "${device_leaks}" >&2
fi

# Rule: even inside src/io/, only .cpp files see them. A device type in a public
# header would leak into every consumer that includes it, which is the same
# problem one level down.
header_leaks="$(grep -rn --include='*.hpp' -E '#include *[<"](RtAudio|RtMidi)\.h' src/io || true)"
if [ -n "${header_leaks}" ]; then
  report "device headers included from a src/io/ *header* (use a forward declaration):"
  echo "${header_leaks}" >&2
fi

# Rule: FTXUI is the TUI lane's business and nobody else's. The engine, ingest
# and io modules must stay renderable-free so offline bounce and the headless
# tests keep working; an ftxui::Element returned from src/engine/ would be the
# same mistake as an RtAudio type, one lane over.
ui_leaks="$(grep -rn --include='*.hpp' --include='*.cpp' -E '#include *[<"]ftxui/' src \
  | grep -v '^src/tui/' || true)"
if [ -n "${ui_leaks}" ]; then
  report "FTXUI headers included outside src/tui/:"
  echo "${ui_leaks}" >&2
fi

# Rule: nothing in src/tui/ drives the terminal by hand any more.
#
# M1 shipped a termios shell because FTXUI was staged for M2. It is gone, and
# FTXUI now owns raw mode, signal handling and restoration -- doing any of that
# alongside it means two owners of one global, and the failure mode is a user's
# terminal left unusable. If this ever needs to come back it should be a
# deliberate change to this rule, not a quiet include.
termios_leaks="$(grep -rn --include='*.hpp' --include='*.cpp' -E '#include *[<"](termios|sys/ioctl)\.h' src/tui || true)"
if [ -n "${termios_leaks}" ]; then
  report "src/tui/ drives the terminal directly (that is FTXUI's job now):"
  echo "${termios_leaks}" >&2
fi

# Rule: src/rt/ depends on nothing outside src/rt/. It is the real-time lane;
# an include from engine/ or ingest/ here would drag allocation and I/O into it.
rt_leaks="$(grep -rn --include='*.hpp' --include='*.cpp' -E '#include *"(engine|ingest|io|tui|lua|host)/' src/rt || true)"
if [ -n "${rt_leaks}" ]; then
  report "src/rt/ includes another module:"
  echo "${rt_leaks}" >&2
fi

# Rule: the engine never sees a device. This is what makes Engine::render() work
# in a plain loop for offline bounce and CI.
engine_leaks="$(grep -rn --include='*.hpp' --include='*.cpp' -E '#include *"(io|tui)/' src/engine || true)"
if [ -n "${engine_leaks}" ]; then
  report "src/engine/ includes the device or UI layer:"
  echo "${engine_leaks}" >&2
fi

# Rule: ingest depends on rt:: types only, never on the engine (CLAUDE.md module
# map). Workers build Samples; they do not drive playback.
ingest_leaks="$(grep -rn --include='*.hpp' --include='*.cpp' -E '#include *"(engine|io|tui)/' src/ingest || true)"
if [ -n "${ingest_leaks}" ]; then
  report "src/ingest/ includes the engine, device or UI layer:"
  echo "${ingest_leaks}" >&2
fi

if [ "${failed}" -ne 0 ]; then
  echo "layering: FAILED (see docs/ARCHITECTURE.md module map)" >&2
  exit 1
fi

echo "layering: module boundaries clean"
