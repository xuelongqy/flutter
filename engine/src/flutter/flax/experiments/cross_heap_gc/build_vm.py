#!/usr/bin/env python3
"""Build only the macOS arm64 experimental VM, without changing toolchain locks."""
import os
from pathlib import Path
import subprocess
import sys

here = Path(__file__).resolve().parent
source = here.parents[3]
optimized = "--optimized" in sys.argv[1:]
output = source / "out" / ("flax_gc_optimized" if optimized else "flax_gc_debug")
env = os.environ.copy()
env["PATH"] = "/opt/homebrew/bin:/usr/bin:/bin:" + str(
    source.parents[1] / ".local/depot_tools"
) + os.pathsep + env["PATH"]
env["DEPOT_TOOLS_UPDATE"] = "0"
env["VPYTHON_BYPASS"] = "manually managed python not supported by chrome operations"
command = [
    sys.executable, "flutter/tools/gn", "--target-os", "mac", "--mac-cpu", "arm64",
    "--runtime-mode", "debug", "--no-lto", "--no-prebuilt-dart-sdk",
    "--no-full-dart-sdk", "--enable-unittests", "--target-dir", output.name,
    "--gn-args", "dart_lib_export_symbols=true",
]
if not optimized:
    command += ["--unoptimized", "--dart-debug"]
subprocess.run(command, cwd=source, env=env, check=True)

# ponytail: local experiment only. The pinned LLVM linker cannot read Xcode 27
# arm64e.x1 TAPI entries; use the installed Apple linker in generated build files.
# GN regeneration overwrites this override, so apply it after every generation.
linker = subprocess.check_output(["xcrun", "--find", "ld"], text=True).strip()
driver = "../../flutter/buildtools/mac-arm64/clang/bin/clang++"
for path in output.rglob("toolchain.ninja"):
    lines = path.read_text().splitlines(keepends=True)
    for i, line in enumerate(lines):
        if "command =" in line and "-Wl,-filelist," in line:
            lines[i] = line.replace(driver, driver + " --ld-path=" + linker)
    path.write_text("".join(lines))
subprocess.run(["ninja", "-C", str(output), "-j", "6", "dartvm"],
               cwd=source, env=env, check=True)
