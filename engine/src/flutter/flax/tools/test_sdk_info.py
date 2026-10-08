"""Verify target selection and immutable SDK inputs without downloading an SDK."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from sdk_info import android_cxx_headers, sdk_inputs, hermes_inputs
from build_hermes import recipe_digest


class SdkTest(unittest.TestCase):
    def test_hermes_requires_the_joint_gc_recipe_and_unchanged_libraries(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve()
            libraries = [build / "hermes/lib/libhermesvm_a.a", build / "hermes/jsi/libjsi.a",
                         build / "hermes/external/boost/boost_1_86_0/libs/context/libboost_context.a"]
            for path in libraries:
                path.parent.mkdir(parents=True)
                path.write_bytes(b"fixture")
            lock = {"hermes": {"revision": "hermes", "patches": []},
                    "flutterRevision": "flutter", "dartRevision": "dart"}
            manifest = {"engine": "hermes", "target": "ios-simulator-arm64", "jit": False,
                        "gcExtension": 1, "revision": "hermes", "recipe": recipe_digest(lock["hermes"]),
                        "libraries": [str(p) for p in libraries], "includeDirs": [],
                        "files": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in libraries}}
            path = build / "flax-hermes.json"
            path.write_text(json.dumps(manifest))
            for target in ("ios-device-arm64", "ios-simulator-arm64"):
                manifest["target"] = target
                path.write_text(json.dumps(manifest))
                self.assertEqual(hermes_inputs(build, lock, target)["libraries"], manifest["libraries"])
                other = "ios-device-arm64" if target == "ios-simulator-arm64" else "ios-simulator-arm64"
                with self.assertRaisesRegex(ValueError, "joint-GC"):
                    hermes_inputs(build, lock, other)
            manifest["gcExtension"] = 0
            path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "joint-GC"):
                hermes_inputs(build, lock, "ios-simulator-arm64")
            manifest["gcExtension"] = 1
            path.write_text(json.dumps(manifest))
            libraries[0].write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "input changed"):
                hermes_inputs(build, lock, "ios-simulator-arm64")

    def test_android_headers_match_the_sdk_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            ndk = root / "ndk/30"
            sysroot = ndk / "toolchains/llvm/prebuilt/host/sysroot"
            headers = sysroot / "usr/include/c++/v1"
            atomic = headers / "__atomic/atomic_ref.h"
            atomic.parent.mkdir(parents=True)
            atomic.write_text("fixture")
            properties = ndk / "source.properties"
            properties.write_text("Pkg.Revision = 30\n")
            (root / "manifest.json").write_text(json.dumps({
                "metadata": {"hostTools": {"androidNdk": "30"}},
            }))
            self.assertEqual(android_cxx_headers(root, root), str(headers))
            properties.write_text("Pkg.Revision = 31\n")
            with self.assertRaisesRegex(ValueError, "NDK revision"):
                android_cxx_headers(root, root)
            properties.write_text("Pkg.Revision = 30\n")
            atomic.unlink()
            with self.assertRaisesRegex(ValueError, "atomic_ref"):
                android_cxx_headers(root, root)

    def test_target_and_integrity_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory).resolve()
            (sdk / "lib").mkdir()
            (sdk / "lib/v8").write_bytes(b"library")
            (sdk / "config").write_text('INTERFACE_COMPILE_DEFINITIONS "V8_SHARED;CPPGC"')
            for target in ("macos-arm64", "android-arm64"):
                manifest = {
                    "target": target,
                    "metadata": {"jit": True, "revision": "v8"},
                    "libraries": ["lib/v8"],
                    "cmakeConfig": "config",
                    "files": {name: hashlib.sha256((sdk / name).read_bytes()).hexdigest()
                              for name in ("lib/v8", "config")},
                }
                lock = {"v8Revision": "v8", "flutterRevision": "flutter", "dartRevision": "dart",
                        "targets": {target: {"libraries": {"lib/v8": manifest["files"]["lib/v8"]}}}}

                def pin_manifest():
                    raw = json.dumps(manifest).encode()
                    (sdk / "manifest.json").write_bytes(raw)
                    lock["targets"][target]["sdkManifestSha256"] = hashlib.sha256(raw).hexdigest()

                pin_manifest()
                inputs = sdk_inputs(sdk, target, lock)
                self.assertEqual(inputs["libraries"], [str(sdk / "lib/v8")])
                self.assertIn("CPPGC", inputs["defines"])
                (sdk / "manifest.json").write_text("changed")
                with self.assertRaisesRegex(ValueError, "manifest does not match"):
                    sdk_inputs(sdk, target, lock)
                pin_manifest()
                (sdk / "lib/v8").write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "file mismatch"):
                    sdk_inputs(sdk, target, lock)
                (sdk / "lib/v8").write_bytes(b"library")
                manifest["target"] = "wrong-target"
                pin_manifest()
                with self.assertRaisesRegex(ValueError, "Expected the pinned"):
                    sdk_inputs(sdk, target, lock)
                manifest["target"] = target
                manifest["files"]["../escape"] = "untrusted"
                pin_manifest()
                with self.assertRaisesRegex(ValueError, "file mismatch"):
                    sdk_inputs(sdk, target, lock)


if __name__ == "__main__":
    unittest.main()
