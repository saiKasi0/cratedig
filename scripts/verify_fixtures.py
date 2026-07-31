#!/usr/bin/env python3
"""Verify fetched audio fixtures against assets/starter-pack/MANIFEST.toml.

Fails on checksum drift so a substituted or edited fixture breaks the build
rather than silently changing test results. Run by scripts/ci.sh.

Derived (locally transcoded) fixtures are reported but not hash-enforced:
encoder output varies by ffmpeg build. They are validated by decode-and-compare
in the test suite instead — see docs/TESTING.md.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import tomllib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PACK_DIR = REPO_ROOT / "assets" / "starter-pack"
MANIFEST = PACK_DIR / "MANIFEST.toml"

ALLOWED_LICENSES = {"CC0-1.0", "public-domain"}


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_onset_labels(fixtures: list[dict], errors: list[str]) -> int:
    """Validate every committed *.onsets.txt.

    These are golden files (docs/TESTING.md): hand-made ground truth for the
    onset accuracy tests, tracked in git rather than fetched. Three things can
    go wrong silently, so all three are checked here rather than discovered as
    a confusing test failure:

      - a label file for audio that is not in the manifest, which would mean
        ground truth for something with no recorded provenance;
      - a malformed line, which std::stod would turn into a zero and quietly
        drag the measured accuracy down;
      - times out of order or negative, which would break the one-to-one
        matching the score depends on.
    """
    known_audio = {entry.get("path") for entry in fixtures}
    count = 0

    for label_path in sorted(PACK_DIR.glob("*.onsets.txt")):
        count += 1
        audio = label_path.name.removesuffix(".onsets.txt")
        if not any(other.startswith(audio + ".") for other in known_audio if other):
            errors.append(
                f"{label_path.name}: labels audio {audio!r}, which is not in the manifest"
            )

        previous = -1.0
        for number, raw in enumerate(label_path.read_text().splitlines(), start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            try:
                value = float(line)
            except ValueError:
                errors.append(f"{label_path.name}:{number}: {line!r} is not a number")
                continue
            if value < 0.0:
                errors.append(f"{label_path.name}:{number}: negative time {value}")
            if value <= previous:
                errors.append(
                    f"{label_path.name}:{number}: {value} is not after the previous label"
                )
            previous = value

    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--require-present",
        action="store_true",
        help="fail if a manifest fixture has not been fetched (used by e2e jobs)",
    )
    args = parser.parse_args()

    if not MANIFEST.is_file():
        print(f"error: missing manifest {MANIFEST}", file=sys.stderr)
        return 1

    with MANIFEST.open("rb") as handle:
        manifest = tomllib.load(handle)

    fixtures = manifest.get("fixture", [])
    if not fixtures:
        print("fixtures: manifest is empty (expected before M1) — nothing to verify")
        return 0

    errors: list[str] = []
    checked = missing = derived = 0

    for entry in fixtures:
        path_value = entry.get("path")
        if not path_value:
            errors.append("a fixture entry has no 'path'")
            continue

        target = PACK_DIR / path_value
        license_id = entry.get("license")
        if license_id not in ALLOWED_LICENSES:
            errors.append(
                f"{path_value}: license {license_id!r} is not allowed "
                f"(must be one of {sorted(ALLOWED_LICENSES)})"
            )

        if "derived_from" in entry:
            derived += 1
            if not entry.get("ffmpeg_args"):
                errors.append(f"{path_value}: derived fixture has no 'ffmpeg_args' recipe")

            # A list means "concatenate these, in this order". Every entry must
            # itself be a manifest fixture, because that is what carries the
            # licence and the checksum -- a derived file inherits its provenance
            # from its sources, so an unlisted source would be provenance
            # laundering rather than a transcode.
            sources = entry["derived_from"]
            if isinstance(sources, str):
                sources = [sources]
            known = {other.get("path") for other in fixtures}
            for source in sources:
                if source not in known:
                    errors.append(
                        f"{path_value}: derived from {source!r}, which is not in the manifest"
                    )
            if len(sources) > 1 and not entry.get("derived_duration_seconds"):
                errors.append(
                    f"{path_value}: a multi-source fixture needs 'derived_duration_seconds'"
                )
            continue

        if not entry.get("source_url"):
            errors.append(f"{path_value}: fetched fixture has no 'source_url'")

        expected = entry.get("sha256")
        if not expected:
            errors.append(f"{path_value}: fetched fixture has no 'sha256'")
            continue

        if not target.is_file():
            missing += 1
            if args.require_present:
                errors.append(f"{path_value}: not fetched (run scripts/fetch_starter_pack.sh)")
            continue

        actual = sha256_of(target)
        checked += 1
        if actual != expected:
            errors.append(
                f"{path_value}: CHECKSUM DRIFT\n"
                f"    manifest: {expected}\n"
                f"    on disk:  {actual}"
            )

    labels = check_onset_labels(fixtures, errors)

    for message in errors:
        print(f"error: {message}", file=sys.stderr)

    if labels:
        print(f"onset labels: {labels} file(s) well-formed")

    summary = f"fixtures: {checked} verified, {missing} not fetched, {derived} derived (not hash-enforced)"
    print(summary, file=sys.stderr if errors else sys.stdout)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
