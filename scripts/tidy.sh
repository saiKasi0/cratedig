#!/usr/bin/env bash
# Run clang-tidy over src/ and tests/ using the dev build's compile database.
set -euo pipefail

cd "$(dirname "$0")/.."

build_dir="build/dev${CRATEDIG_BUILD_SUFFIX:-}"

if [ ! -f "${build_dir}/compile_commands.json" ]; then
  echo "==> configuring ${build_dir} (no compile database yet)"
  cmake --preset dev
fi

if command -v run-clang-tidy >/dev/null 2>&1; then
  runner=(run-clang-tidy -p "${build_dir}" -quiet)
else
  runner=(run-clang-tidy.py -p "${build_dir}" -quiet)
fi

# Exclude the FetchContent tree: third-party sources are not ours to lint.
"${runner[@]}" "^$(pwd)/(src|tests)/"
