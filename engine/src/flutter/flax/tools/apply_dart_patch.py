#!/usr/bin/env python3

"""Apply the maintained Flax VM patch without resetting local Dart changes."""

import argparse
from pathlib import Path
import subprocess


def apply(dart: Path, patch: Path, expected: str) -> None:
  actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=dart, text=True).strip()
  if actual != expected:
    raise RuntimeError(f"Dart revision mismatch: expected {expected}, got {actual}")
  reverse = subprocess.run(
      ["git", "apply", "--reverse", "--check", str(patch)],
      cwd=dart,
      stdout=subprocess.DEVNULL,
      stderr=subprocess.DEVNULL,
  )
  if reverse.returncode == 0:
    print("Flax Dart patch already applied")
    return
  # git apply is atomic. A partial patch or unrelated conflicting edit fails
  # here; never reset, stash, or overwrite the dependency's working tree.
  subprocess.run(["git", "apply", "--check", str(patch)], cwd=dart, check=True)
  subprocess.run(["git", "apply", str(patch)], cwd=dart, check=True)
  print("Flax Dart patch applied")


def apply_all(dart: Path, flax: Path) -> None:
  revision = (flax / "patches/dart_revision").read_text().strip()
  base = flax / "patches/dart.patch"
  upgrade = flax / "patches/dart-startup.patch"
  actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=dart, text=True).strip()
  # Upgrade an existing maintained checkout without reverting its local patches.
  # Fresh checkouts use the consolidated base; edited hunks still fail safely.
  if actual == revision and subprocess.run(
      ["git", "apply", "--reverse", "--check", str(base)],
      cwd=dart,
      stdout=subprocess.DEVNULL,
      stderr=subprocess.DEVNULL,
  ).returncode != 0 and subprocess.run(
      ["git", "apply", "--check", str(upgrade)],
      cwd=dart,
      stdout=subprocess.DEVNULL,
      stderr=subprocess.DEVNULL,
  ).returncode == 0:
    apply(dart, upgrade, revision)
  for name in ("dart.patch", "dart-ffi.patch"):
    apply(dart, flax / "patches" / name, revision)


def main() -> None:
  flax = Path(__file__).resolve().parents[1]
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--dart-source", type=Path, default=flax.parent / "third_party/dart")
  args = parser.parse_args()
  apply_all(args.dart_source, flax)


if __name__ == "__main__":
  main()
