#!/usr/bin/env bash
# Regenerate the committed TUI snapshots in tests/tui/snapshots/.
#
# READ THIS BEFORE RUNNING IT.
#
# The snapshots are the ground truth for what CRATEDIG's interface IS; the
# mockups in docs/design are what it aspires to (CLAUDE.md). Regenerating them
# turns a failing test into a passing one without anybody having decided the new
# output is correct, which is the single easiest way to let the interface rot.
#
# CLAUDE.md's rule for this script is: look at the rendered output in a real
# terminal first. `cratedig <file>` at 100x30 is the same layout the goldens
# capture -- if it does not look right there, it is not right here either.
#
# A diff of one character is a real change to the product. Explain it in the
# commit message the same way a changed golden hash has to be explained.
set -euo pipefail

cd "$(dirname "$0")/.."

preset="${1:-dev}"
build_dir="build/${preset}${CRATEDIG_BUILD_SUFFIX:-}"
binary="${build_dir}/tests/cratedig_unit_tests"

if [ ! -x "${binary}" ]; then
  echo "==> building ${preset} (no test binary yet)"
  cmake --preset "${preset}"
  cmake --build --preset "${preset}" -j
fi

echo "==> regenerating tests/tui/snapshots/"
CRATEDIG_UPDATE_SNAPSHOTS=1 "${binary}" "[tui]"

echo
echo "==> what changed"
if git diff --stat -- tests/tui/snapshots | grep -q .; then
  git --no-pager diff -- tests/tui/snapshots
  echo
  echo "Review the diff above. If any of it is a surprise, it is a bug, not a"
  echo "snapshot that needed updating."
else
  echo "nothing (the snapshots already matched)"
fi
