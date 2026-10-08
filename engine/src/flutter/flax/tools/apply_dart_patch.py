#!/usr/bin/env python3
"""Apply the maintained Flax VM patch without resetting local Dart changes."""

import argparse
from pathlib import Path
import subprocess


def apply(dart: Path, patch: Path, expected: str) -> None:
    actual = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=dart, text=True
    ).strip()
    if actual != expected:
        raise RuntimeError(f"Dart revision mismatch: expected {expected}, got {actual}")
    reverse = subprocess.run(
        ["git", "apply", "--reverse", "--check", str(patch)], cwd=dart,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if reverse.returncode == 0:
        print("Flax Dart patch already applied")
        return
    # git apply is atomic. A partial patch or unrelated conflicting edit fails
    # here; never reset, stash, or overwrite the dependency's working tree.
    subprocess.run(["git", "apply", "--check", str(patch)], cwd=dart, check=True)
    subprocess.run(["git", "apply", str(patch)], cwd=dart, check=True)
    print("Flax Dart patch applied")


def main() -> None:
    flax = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dart-source", type=Path,
                        default=flax.parent / "third_party/dart")
    args = parser.parse_args()
    apply(args.dart_source, flax / "patches/dart.patch",
          (flax / "patches/dart_revision").read_text().strip())


if __name__ == "__main__":
    main()
