FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# ffmpeg (the CLI) transcodes the derived audio fixtures; libav*-dev is what
# src/ingest/ links against. Ubuntu's build is --enable-gpl, which is fine here:
# CI distributes nothing, so no GPL obligation is triggered. Release builds are
# the ones that must link an LGPL FFmpeg, enforced by
# -DCRATEDIG_REQUIRE_LGPL_FFMPEG=ON (docs/LICENSING.md).
#
# libasound2-dev is not optional: RtAudio's CMakeLists hard-fails with
# "ALSA API requested but no ALSA dev libraries found" when RTAUDIO_API_ALSA is on.
# The container still has no /dev/snd, so zero devices is the expected runtime
# state and src/io/ must degrade gracefully to it.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang-18 \
        clang-format-18 \
        clang-tidy-18 \
        cmake \
        ffmpeg \
        git \
        libasound2-dev \
        libavcodec-dev \
        libavformat-dev \
        libavutil-dev \
        libclang-rt-18-dev \
        libswresample-dev \
        ninja-build \
        pipx \
        pkg-config \
        python3 \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/clang-18 /usr/local/bin/clang \
    && ln -s /usr/bin/clang++-18 /usr/local/bin/clang++ \
    && ln -s /usr/bin/clang-format-18 /usr/local/bin/clang-format \
    && ln -s /usr/bin/clang-tidy-18 /usr/local/bin/clang-tidy \
    && ln -s /usr/bin/run-clang-tidy-18 /usr/local/bin/run-clang-tidy

RUN pipx install pre-commit
ENV PATH="/root/.local/bin:${PATH}"

ENV CC=clang-18 CXX=clang++-18
WORKDIR /work
