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

    for message in errors:
        print(f"error: {message}", file=sys.stderr)

    summary = f"fixtures: {checked} verified, {missing} not fetched, {derived} derived (not hash-enforced)"
    print(summary, file=sys.stderr if errors else sys.stdout)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
