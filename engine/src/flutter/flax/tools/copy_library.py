#!/usr/bin/env python3
"""Atomically copy a packaged library without hardlinking its verified SDK."""

import argparse
import os
from pathlib import Path
import shutil
import tempfile


def copy_library(source: Path, output: Path) -> None:
  source = source.resolve(strict=True)
  output = output.absolute()
  if source == output.resolve():
    raise ValueError("SDK input and packaged output must be separate paths")
  output.parent.mkdir(parents=True, exist_ok=True)
  # GN's mac copy tool hardlinks regular files. Signing/stripping a delivered
  # library must never modify the verified input, including on incremental builds
  # that still have an old hardlinked output. Replace, do not truncate, that file.
  fd, temporary = tempfile.mkstemp(prefix=f".{output.name}.", dir=output.parent)
  temporary = Path(temporary)
  try:
    with os.fdopen(fd, "wb") as destination, source.open("rb") as origin:
      shutil.copyfileobj(origin, destination)
    shutil.copystat(source, temporary)
    os.replace(temporary, output)
  finally:
    temporary.unlink(missing_ok=True)


def main() -> None:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("source", type=Path)
  parser.add_argument("output", type=Path)
  args = parser.parse_args()
  copy_library(args.source, args.output)


if __name__ == "__main__":
  main()
