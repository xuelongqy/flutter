#!/usr/bin/env python3
"""Return GN inputs from the immutable, revision-locked V8 SDK."""

import hashlib
import json
import os
from pathlib import Path
import re
import sys

def sdk_inputs(sdk: Path, target: str, lock: dict) -> dict:
    sdk = sdk.resolve()
    pinned = lock["targets"][target]
    raw = (sdk / "manifest.json").read_bytes()
    if hashlib.sha256(raw).hexdigest() != pinned["sdkManifestSha256"]:
        raise ValueError("V8 SDK manifest does not match the Flax engine source lock")
    manifest = json.loads(raw)
    if (manifest["target"] != target or
            not manifest["metadata"].get("jit") or
            manifest["metadata"]["revision"] != lock["v8Revision"]):
        raise ValueError(f"Expected the pinned {target} V8 JIT SDK")
    libraries = {name: manifest["files"][name] for name in manifest["libraries"]}
    if libraries != pinned["libraries"] or manifest["cmakeConfig"] not in manifest["files"]:
        raise ValueError("V8 SDK library or CMake configuration differs from the source lock")
    for name, digest in manifest["files"].items():
        path = (sdk / name).resolve()
        if not path.is_relative_to(sdk) or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"V8 SDK file mismatch: {name}")
    config = (sdk / manifest["cmakeConfig"]).read_text()
    defines = re.search(r'INTERFACE_COMPILE_DEFINITIONS "([^"]+)"', config).group(1).split(";")
    defines += ['FLAX_FLUTTER_REVISION="' + lock['flutterRevision'] + '"',
                'FLAX_DART_REVISION="' + lock['dartRevision'] + '"']
    return {"defines": defines, "libraries": [str(sdk / x) for x in libraries]}


def android_cxx_headers(sdk: Path, android_sdk: Path) -> str:
    manifest = json.loads((sdk / "manifest.json").read_text())
    revision = manifest["metadata"]["hostTools"]["androidNdk"]
    ndk = android_sdk / "ndk" / revision
    properties = (ndk / "source.properties").read_text()
    if not re.search(r"^Pkg.Revision\s*=\s*" + re.escape(revision) + r"\s*$",
                     properties, re.MULTILINE):
        raise ValueError("Android C++ headers do not match the V8 SDK's NDK revision")
    sysroots = list((ndk / "toolchains/llvm/prebuilt").glob("*/sysroot"))
    if len(sysroots) != 1:
        raise ValueError("Expected one installed Android NDK host sysroot")
    sysroot = sysroots[0]
    headers = sysroot / "usr/include/c++/v1"
    if not (headers / "__atomic/atomic_ref.h").is_file():
        raise ValueError("The SDK's Android C++ headers must support std::atomic_ref")
    return str(headers.resolve())


if __name__ == "__main__":
    flax = Path(__file__).resolve().parents[1]
    lock = json.loads((flax / "source.lock.json").read_text())
    sdk = Path(sys.argv[1]).resolve()
    target = sys.argv[2]
    inputs = sdk_inputs(sdk, target, lock)
    if target == "android-arm64":
        android_sdk = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
        if not android_sdk:
            raise SystemExit("Set ANDROID_HOME to the SDK with the pinned V8 NDK installed")
        inputs["cxxHeaders"] = android_cxx_headers(sdk, Path(android_sdk))
    print(json.dumps(inputs))
