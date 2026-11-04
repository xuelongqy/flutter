"""Check engine configuration commands without SDK validation, patching, or builds."""

import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import build_engine


class BuildEngineTest(unittest.TestCase):

  def test_debug_optimization_is_macos_only(self):
    cases = (
        ("macos-arm64", "debug", "mac", False),
        ("macos-arm64", "profile", "mac", False),
        ("macos-arm64", "release", "mac", False),
        ("android-arm64", "debug", "android", True),
        ("android-arm64", "profile", "android", False),
        ("android-arm64", "release", "android", False),
        ("ios-simulator-arm64", "debug", "ios", True),
        ("ios-device-arm64", "release", "ios", False),
    )
    # Require the caller's workspace temp root instead of falling back to /tmp.
    with tempfile.TemporaryDirectory(
        prefix="flax-build-engine-", dir=os.environ["TMPDIR"]
    ) as directory:
      root = Path(directory).resolve()
      source = root / "engine/src"
      flax = source / "flutter/flax"
      script = flax / "tools/build_engine.py"
      sdk = root / "sdk"
      for target, mode, platform, debug_flags in cases:
        with self.subTest(target=target, mode=mode):
          simulator = target == "ios-simulator-arm64"
          output = source / "out" / f"flax_{platform}_{mode}_{'sim_' if simulator else ''}arm64"
          output.mkdir(parents=True)
          argv = [str(script), "--sdk", str(sdk), "--target", target,
                  "--mode", mode, "--configure-only"]
          # Python 3.11 is simulated only for Android command generation.
          with (
              mock.patch.object(build_engine, "__file__", str(script)),
              mock.patch.object(sys, "argv", argv),
              mock.patch.object(sys, "version_info", (3, 11, 0, "final", 0)),
              mock.patch.object(build_engine, "apply_all") as apply_all,
              mock.patch.object(build_engine.subprocess, "run") as run,
              mock.patch.object(build_engine.subprocess, "check_output", return_value="3.11\n"),
          ):
            build_engine.main()

          apply_all.assert_called_once_with(source / "flutter/third_party/dart", flax)
          self.assertEqual(run.call_count, 3)  # SDK check, GN, Ninja regeneration only.
          sdk_call, gn_call, ninja_call = run.call_args_list
          self.assertEqual(sdk_call.args[0],
                           [sys.executable, str(flax / "tools/sdk_info.py"), str(sdk), target])
          command = gn_call.args[0]
          self.assertEqual(command[:2], [sys.executable, "flutter/tools/gn"])
          self.assertEqual(gn_call.kwargs["cwd"], source)
          self.assertEqual(command[command.index("--target-os") + 1], platform)
          self.assertEqual(command[command.index("--runtime-mode") + 1], mode)
          self.assertEqual(command[command.index("--target-dir") + 1], output.name)
          self.assertEqual("--simulator" in command, simulator)
          self.assertEqual(ninja_call.args[0], ["ninja", "-C", str(output), "build.ninja"])
          self.assertEqual(ninja_call.kwargs["cwd"], source)
          flags = [flag for flag in command if flag in ("--unoptimized", "--dart-debug")]
          self.assertEqual(flags, ["--unoptimized", "--dart-debug"] if debug_flags else [])


if __name__ == "__main__":
  unittest.main()
