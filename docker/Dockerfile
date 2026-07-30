FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang-18 \
        clang-format-18 \
        clang-tidy-18 \
        cmake \
        git \
        libclang-rt-18-dev \
        ninja-build \
        pipx \
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
