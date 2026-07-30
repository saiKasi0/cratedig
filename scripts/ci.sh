#!/usr/bin/env bash
# Run the configure/build/test cycle for one or more presets.
# Usage: scripts/ci.sh dev asan tsan ubsan
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ $# -eq 0 ]]; then
  echo "usage: $0 <preset> [preset...]" >&2
  exit 2
fi

# Audio fixtures are fetched, not committed. Verify whatever is present against
# assets/starter-pack/MANIFEST.toml so checksum drift fails the build. No network
# needed: fixtures that were never fetched are reported, not fatal (e2e jobs run
# scripts/fetch_starter_pack.sh first, which enforces presence).
echo "==> verifying audio fixtures"
python3 scripts/verify_fixtures.py

for preset in "$@"; do
  echo "==> preset: ${preset}"
  cmake --preset "${preset}"
  cmake --build --preset "${preset}" -j
  ctest --preset "${preset}" --output-on-failure
done
