#!/usr/bin/env python3

"""Build matching arm64 V8 engines, using jitless on iOS."""

import argparse
import os
import shutil
from pathlib import Path
import subprocess
import sys

from apply_dart_patch import apply_all


def main() -> None:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--sdk", type=Path, required=True)
  parser.add_argument(
      "--target",
      choices=("macos-arm64", "android-arm64", "ios-device-arm64", "ios-simulator-arm64"),
      default="macos-arm64"
  )
  parser.add_argument("--mode", choices=("debug", "profile", "release"), default="debug")
  parser.add_argument("--jobs", type=int, default=6)
  parser.add_argument("--configure-only", action="store_true")
  args = parser.parse_args()
  simulator = args.target == "ios-simulator-arm64"
  ios = args.target in ("ios-device-arm64", "ios-simulator-arm64")
  if ios and args.mode != ("debug" if simulator else "release"):
    parser.error("iOS supports simulator debug or device release/AOT")
  if args.jobs < 1:
    parser.error("--jobs must be positive")
  if args.target == "android-arm64" and sys.version_info[:2] != (3, 11):
    parser.error("Android's depot_tools build scripts require Python 3.11")
  flax = Path(__file__).resolve().parents[1]
  source = flax.parents[1]
  checkout = source.parents[1]
  apply_all(flax.parent / "third_party/dart", flax)
  sdk = str(args.sdk.resolve())
  subprocess.run([sys.executable, str(flax / "tools/sdk_info.py"), sdk, args.target],
                 check=True,
                 stdout=subprocess.DEVNULL)
  platform = "ios" if ios else "mac" if args.target == "macos-arm64" else "android"
  output = source / "out" / f"flax_{platform}_{args.mode}_{'sim_' if simulator else ''}arm64"
  env = os.environ.copy()
  env["PATH"] += os.pathsep + "/opt/homebrew/bin:/usr/bin:/bin:" + str(
      checkout / ".local/depot_tools"
  )
  env["DEPOT_TOOLS_UPDATE"] = "0"
  env["VPYTHON_BYPASS"] = "manually managed python not supported by chrome operations"
  if args.target == "android-arm64":
    version = subprocess.check_output(
        ["python3", "-c", "import sys; print('%s.%s' % sys.version_info[:2])"],
        env=env,
        text=True,
    ).strip()
    if version != "3.11":
      parser.error("Put Python 3.11's python3 on PATH for Android build actions")
  if simulator:
    cpu = ["--simulator", "--simulator-cpu", "arm64"]
  elif ios:
    cpu = []  # Flutter's device target is arm64.
  else:
    cpu = [f"--{platform}-cpu", "arm64"]
  command = [
      sys.executable, "flutter/tools/gn", "--target-os", platform, *cpu, "--runtime-mode",
      args.mode, "--no-lto", "--no-prebuilt-dart-sdk", "--no-full-dart-sdk",
      "--no-enable-unittests", "--target-dir", output.name, "--gn-args",
      'flax_v8_sdk="' + sdk + '" flax_system_cxx=true mac_deployment_target="15.0.0"' +
      (' ios_deployment_target="16.3"' if ios else '')
  ]
  if args.mode == "debug":
    command += ["--unoptimized", "--dart-debug"]
  subprocess.run(command, cwd=source, env=env, check=True)
  # Ninja may regenerate GN output once more before reading compile commands.
  subprocess.run(["ninja", "-C", str(output), "build.ninja"], cwd=source, env=env, check=True)
  # Xcode 27's os_log macros carry an attribute ignored when stack protection
  # is forced. Keep protection and all other warning errors enabled.
  for path in output.rglob("*.ninja"):
    if path.name == "toolchain.ninja":
      continue
    source_text = path.read_text()
    if "-fstack-protector-all" in source_text and "-Wno-error=ignored-attributes" not in source_text:
      path.write_text(
          source_text.replace(
              "-fstack-protector-all", "-fstack-protector-all -Wno-error=ignored-attributes"
          )
      )
  if not args.configure_only:
    targets = ["flutter", "flutter_tester", "const_finder"] if platform == "mac" else ["flutter"]
    if platform == "android":
      # Flutter applications consume the packaged JAR, not the raw library.
      targets.append("flutter_jar_zip")
    subprocess.run(["ninja", "-C", str(output), "-j",
                    str(args.jobs), *targets],
                   cwd=source,
                   env=env,
                   check=True)
    # Pub/analyzer resolve dart:ui through this SDK package, not engine flags.
    shutil.copytree(
        output / "gen/dart-pkg/sky_engine",
        checkout / "bin/cache/pkg/sky_engine",
        dirs_exist_ok=True
    )
    frameworks = []
    if platform == "mac":
      frameworks = [
          output / "FlutterMacOS.framework",
          *(output / "FlutterMacOS.xcframework").glob("*/FlutterMacOS.framework")
      ]
    if ios:
      # Static linkage still needs the upstream notices in the shipped framework.
      frameworks = [
          output / "Flutter.framework",
          *(output / "Flutter.xcframework").glob("*/Flutter.framework")
      ]
      for framework in frameworks:
        notices = framework / "FlaxV8Notices"
        if notices.exists():
          shutil.rmtree(notices)
        shutil.copytree(Path(sdk) / "notices", notices)
        shutil.copy2(Path(sdk) / "manifest.json", notices / "sdk-manifest.json")
    if args.mode == "release":
      for framework in frameworks:
        binary = (
            framework / "Versions/A/FlutterMacOS" if platform == "mac" else framework / "Flutter"
        )
        # Keep global exports; full symbols remain in SDK inputs, link outputs and dSYMs.
        for library in [*binary.parent.glob("Libraries/*.dylib"), binary]:
          subprocess.run(["xcrun", "strip", "-S", "-x", str(library)], check=True)
          subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none",
                          str(library)],
                         check=True)
  host = output.name if platform == "mac" else f"flax_mac_{args.mode}_arm64"
  print(f"Local engine: --local-engine={output.name} --local-engine-host={host}")


if __name__ == "__main__":
  main()
