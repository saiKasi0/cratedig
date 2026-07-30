#!/usr/bin/env bash
# Download the CC0 audio fixtures listed in assets/starter-pack/MANIFEST.toml,
# verify each against its recorded sha256, and locally transcode any derived
# fixtures with ffmpeg.
#
# Audio binaries are never committed (docs/TESTING.md); this script is how they
# get onto a machine. Idempotent: a file already present with the right hash is
# left alone. Required before `ctest -L e2e`.
set -euo pipefail

cd "$(dirname "$0")/.."

PACK_DIR="assets/starter-pack"
MANIFEST="${PACK_DIR}/MANIFEST.toml"

have() { command -v "$1" >/dev/null 2>&1; }

if ! have curl; then
  echo "error: curl is required" >&2
  exit 1
fi

if have sha256sum; then
  sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
elif have shasum; then
  sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
  echo "error: need sha256sum or shasum" >&2
  exit 1
fi

if [ ! -f "${MANIFEST}" ]; then
  echo "error: missing ${MANIFEST}" >&2
  exit 1
fi

mkdir -p "${PACK_DIR}"

# Emit "path<TAB>source_url<TAB>sha256" for each fetched fixture. Parsing TOML in
# bash is a bad idea; python3 ships with the toolchain (scripts/setup.sh).
# Derived fixtures are built by scripts/derive_fixtures.py below.
read_fetch_list() {
  python3 -c '
import sys, tomllib
with open(sys.argv[1], "rb") as fh:
    manifest = tomllib.load(fh)
for entry in manifest.get("fixture", []):
    if "derived_from" not in entry:
        print("\t".join([entry["path"], entry["source_url"], entry["sha256"]]))
' "$1"
}

fetched=0
skipped=0

while IFS=$'\t' read -r path url expected; do
  [ -n "${path}" ] || continue
  target="${PACK_DIR}/${path}"

  if [ -f "${target}" ] && [ "$(sha256_of "${target}")" = "${expected}" ]; then
    skipped=$((skipped + 1))
    continue
  fi

  echo "==> fetching ${path}"
  mkdir -p "$(dirname "${target}")"
  tmp="${target}.part"
  curl -fsSL --retry 3 -o "${tmp}" "${url}"

  actual="$(sha256_of "${tmp}")"
  if [ "${actual}" != "${expected}" ]; then
    rm -f "${tmp}"
    echo "error: ${path} sha256 mismatch" >&2
    echo "    manifest: ${expected}" >&2
    echo "    download: ${actual}" >&2
    echo "    Refusing to install an unverified fixture." >&2
    exit 1
  fi
  mv "${tmp}" "${target}"
  fetched=$((fetched + 1))
done < <(read_fetch_list "${MANIFEST}")

read -r derived present < <(python3 scripts/derive_fixtures.py "${MANIFEST}" "${PACK_DIR}")
derived=$((derived + present))

total=$((fetched + skipped + derived))
if [ "${total}" -eq 0 ]; then
  echo "starter pack: manifest is empty — no fixtures until M1 (see docs/TESTING.md)"
else
  echo "starter pack: ${fetched} fetched, ${skipped} already present, ${derived} derived"
fi

python3 scripts/verify_fixtures.py --require-present
