#!/usr/bin/env python3
"""Build every derived fixture in the manifest. Called by fetch_starter_pack.sh.

Derivation is python rather than shell because a single-source transcode is a
one-liner but a multi-source fixture needs a concat filtergraph with one aformat
per input, and building that in bash would be an exercise in quoting.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

QUIET = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"]


def build_single(pack_dir: Path, source: str, args: list[str], target: Path) -> None:
    print(f"==> transcoding {target.name} from {source}", file=sys.stderr)
    result = subprocess.run(QUIET + ["-i", str(pack_dir / source)] + args + [str(target)])
    if result.returncode != 0:
        sys.exit(result.returncode)


def build_sequence(pack_dir: Path, sources: list[str], args: list[str], seconds: int,
                   target: Path) -> None:
    """Concatenate, then loop the whole sequence and trim to length.

    Two passes rather than one because -stream_loop is an input option and does
    not apply to a filtergraph output.
    """
    print(f"==> assembling {target.name} from {len(sources)} segments", file=sys.stderr)
    inputs: list[str] = []
    chain: list[str] = []
    labels: list[str] = []
    for index, source in enumerate(sources):
        inputs += ["-i", str(pack_dir / source)]
        # Per input, because the sources differ in channel count: concat refuses
        # a mono segment next to a stereo one.
        chain.append(f"[{index}:a]aformat=sample_rates=44100:channel_layouts=stereo[a{index}]")
        labels.append(f"[a{index}]")
    graph = ";".join(chain) + ";" + "".join(labels) + f"concat=n={len(sources)}:v=0:a=1[out]"

    sequence = target.with_suffix(".sequence.flac")
    result = subprocess.run(
        QUIET + inputs + ["-filter_complex", graph, "-map", "[out]", str(sequence)])
    if result.returncode != 0:
        sequence.unlink(missing_ok=True)
        sys.exit(result.returncode)

    result = subprocess.run(
        QUIET + ["-stream_loop", "-1", "-i", str(sequence), "-t", str(seconds)] + args
        + [str(target)])
    sequence.unlink(missing_ok=True)
    if result.returncode != 0:
        sys.exit(result.returncode)


def main() -> int:
    manifest_path = Path(sys.argv[1])
    pack_dir = Path(sys.argv[2])
    with manifest_path.open("rb") as handle:
        manifest = tomllib.load(handle)

    built = present = 0
    for entry in manifest.get("fixture", []):
        if "derived_from" not in entry:
            continue
        target = pack_dir / entry["path"]
        if target.is_file():
            present += 1
            continue

        if shutil.which("ffmpeg") is None:
            print(f"error: {entry['path']} must be built with ffmpeg, which is not installed",
                  file=sys.stderr)
            return 1

        sources = entry["derived_from"]
        args = entry["ffmpeg_args"].split()
        if isinstance(sources, str):
            build_single(pack_dir, sources, args, target)
        else:
            build_sequence(pack_dir, sources, args, entry["derived_duration_seconds"], target)
        built += 1

    print(f"{built} {present}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
