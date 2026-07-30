#!/usr/bin/env bash
# Install the toolchain CRATEDIG needs to build and lint. Idempotent.
set -euo pipefail

cd "$(dirname "$0")/.."

have() { command -v "$1" >/dev/null 2>&1; }

if have brew; then
  echo "==> macOS / Homebrew"
  # llvm brings clang-format and clang-tidy; AppleClang ships neither.
  # ffmpeg provides both the shared libraries src/ingest/ links and the CLI that
  # transcodes derived audio fixtures; pkg-config is how cmake/ffmpeg.cmake finds
  # them. Homebrew's ffmpeg is a --enable-gpl build, which is fine for local
  # development — see docs/LICENSING.md for why that does not encumber anything
  # until a binary is actually distributed.
  for pkg in cmake ninja llvm pkg-config ffmpeg pre-commit; do
    if brew list --versions "${pkg}" >/dev/null 2>&1; then
      echo "    ${pkg} already installed"
    else
      brew install "${pkg}"
    fi
  done
  llvm_bin="$(brew --prefix llvm)/bin"
  if ! have clang-format; then
    echo "    note: add ${llvm_bin} to PATH for clang-format/clang-tidy"
  fi
elif have apt-get; then
  echo "==> Debian / Ubuntu"
  sudo apt-get update
  # libasound2-dev is required, not optional: RtAudio's CMakeLists hard-fails
  # without it when the ALSA API is enabled (which it is, on Linux).
  sudo apt-get install -y --no-install-recommends \
    cmake ninja-build clang-18 clang-format-18 clang-tidy-18 libclang-rt-18-dev \
    ffmpeg libavcodec-dev libavformat-dev libavutil-dev libswresample-dev \
    libasound2-dev pkg-config \
    git python3 pipx
  pipx install pre-commit || true
else
  echo "unsupported platform: install cmake, ninja, clang-format, clang-tidy, pre-commit manually" >&2
  exit 1
fi

if have pre-commit && [ -d .git ]; then
  pre-commit install
  echo "==> pre-commit hook installed"
fi

echo "==> done. next: cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev"
