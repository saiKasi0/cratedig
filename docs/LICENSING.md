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

### What is and is not at stake

CRATEDIG's source is Apache-2.0 and **linking cannot change that**. A license on
someone else's library is not an assignment of ours: our copyright, and the terms
we offer this source under, are unaffected by what the binary links against.

What FFmpeg's licensing constrains is narrower — **the terms under which a
distributed binary may be offered**:

- FFmpeg is LGPL-2.1+ by default. LGPL explicitly permits dynamic linking from a
  differently-licensed program, so an Apache-2.0 CRATEDIG binary that dynamically
  links an LGPL FFmpeg stays Apache-2.0. **This is the only configuration in which
  a release binary may be distributed.**
- Built with `--enable-gpl`, FFmpeg becomes GPL-2-**or-later**. Distributing a
  binary linked against such a build would make the *combination* GPL. Because it
  is "or later", GPLv3 is available, and Apache-2.0 is compatible with GPLv3 — so
  the combination is distributable, just under GPLv3 rather than Apache-2.0.
  (Apache-2.0 is *not* compatible with GPLv2-only; the "or later" is what avoids
  that dead end.)
- Running the program is not distribution. The GPL imposes no conditions on
  private use, so a developer's machine or a CI container triggers nothing.

### The rules

1. **Only LGPL-covered APIs are called.** Demux and decode via libavformat /
   libavcodec / libavutil / libswresample. No GPL-only filters, encoders, or
   components, ever — that is what keeps the LGPL path available at all.
2. **Dynamic linking only.** We link the system FFmpeg shared libraries via
   pkg-config. FFmpeg is never linked statically, never vendored, and no FFmpeg
   binary or source is shipped with CRATEDIG.
3. **Release binaries MUST link an LGPL build** (no `--enable-gpl`, no
   `--enable-nonfree`). This is asserted mechanically, not on trust: configuring
   with `-DCRATEDIG_REQUIRE_LGPL_FFMPEG=ON` compiles and runs a probe calling
   `avutil_license()` and **fails the configure step** unless it reports LGPL.
   The release preset turns it on; it is OFF by default.
4. **Local development and CI may use whatever FFmpeg the machine has.** Both
   Homebrew and Debian/Ubuntu ship `--enable-gpl` builds and neither offers an
   LGPL-only package, so requiring one for development would mean every
   contributor compiling FFmpeg from source to satisfy an obligation that
   distribution has not triggered. `cratedig --version` prints the linked build's
   license so the situation is always visible rather than assumed.
5. Decode happens on worker threads in `src/ingest/`; nothing in `src/rt/` touches
   FFmpeg.

This is a normative rule about what we may ship, not legal advice.

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
