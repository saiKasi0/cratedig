# Licensing — normative rules

CRATEDIG is **Apache-2.0** (see `/LICENSE`, `/NOTICE`). Everything that ships in this
repo or links into its binaries must be compatible with that. These rules are
normative: a change that violates them is a bug regardless of what it improves.

## Hard bans

1. **VST3 SDK is GPLv3. It is never linked, vendored, or FetchContent-ed into this
   codebase.** No "just for a prototype", no optional CMake flag. The v2 plan for
   VST3 support is a GPL bridge process in a **separate repository** that talks to
   CRATEDIG over IPC; nothing in this repo may include VST3 headers.
2. **No GPL or LGPL-static code in the binary.** LGPL libraries are allowed only
   when dynamically linked (see FFmpeg below).
3. **No bundled audio without CC0 proof.** Every file under `assets/starter-pack/`
   must have a row in `assets/starter-pack/MANIFEST.toml` recording source URL,
   author, CC0 declaration, and sha256. CI verifies hashes against the manifest;
   a file without a manifest row fails the build.

## FFmpeg

- Use an **LGPL build** of FFmpeg (libavformat / libavcodec / swresample) only:
  no `--enable-gpl`, no `--enable-nonfree` components (x264, x265, etc. must not
  be present in the linked build).
- **Dynamic linking only.** The engine loads the system/homebrew FFmpeg shared
  libraries; we never link FFmpeg statically and never ship FFmpeg binaries.
- Decode happens in worker processes/threads in `src/ingest/`; nothing in `src/rt/`
  touches FFmpeg.

## yt-dlp

- yt-dlp is **user-installed** and invoked as a **subprocess** (`src/ingest/`).
  It is never distributed with CRATEDIG, never imported as a library, and its
  absence must degrade gracefully (the `:yt` command reports "yt-dlp not found").
- Tests use a `fake-yt-dlp` stub; CI never invokes the real tool or the network.
- Users are responsible for complying with the terms of the services they access.

## Dependency license table (fixed set, `cmake/deps.cmake`)

| Dependency | License | Linkage | Notes |
|---|---|---|---|
| RtAudio | MIT-style | static | |
| RtMidi | MIT-style | static | |
| FFmpeg (avformat/avcodec/swresample) | LGPL-2.1+ | **dynamic only** | LGPL build, see above |
| libsamplerate | BSD-2-Clause | static | |
| PFFFT | FFTPACK (BSD-style) | static | |
| FTXUI | MIT | static | |
| Lua 5.4 | MIT | static | |
| sol2 | MIT | header-only | |
| CLAP | MIT | header-only | hosting API |
| lilv (LV2 host) | ISC | dynamic (system) | Linux/macOS |
| Catch2 | BSL-1.0 | tests only | not shipped |
| CLI11 | BSD-3-Clause | static | |

Adding a dependency is a human decision (CLAUDE.md): propose it with its license,
linkage, and why it can't be avoided — do not vendor it.

## Attribution

When a dependency begins shipping in release binaries, add its attribution to
`/NOTICE` in the same PR that wires it in.
