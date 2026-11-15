#!/usr/bin/env python3
"""Apply only the experimental patch to the exact stable Flutter Dart checkout."""
from pathlib import Path
import subprocess

here = Path(__file__).resolve().parent
dart = here.parents[2] / "third_party/dart"
expected = (here / "dart_revision").read_text().strip()
actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=dart, text=True).strip()
if actual != expected:
    raise SystemExit(f"Dart revision mismatch: expected {expected}, got {actual}")
patch = str(here / "dart.patch")
if subprocess.run(["git", "apply", "--reverse", "--check", patch], cwd=dart,
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
    print("Experimental patch already applied")
else:
    subprocess.run(["git", "apply", "--check", patch], cwd=dart, check=True)
    subprocess.run(["git", "apply", patch], cwd=dart, check=True)
    print("Experimental patch applied")
