#!/usr/bin/env python3
"""Build pinned Hermes with the joint-GC extension for iOS arm64."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def recipe_digest(lock: dict) -> str:
    return hashlib.sha256(json.dumps(lock, sort_keys=True).encode()).hexdigest()


def prepare(flax: Path, cache: Path, archive: Path | None) -> Path:
    lock = json.loads((flax / "source.lock.json").read_text())["hermes"]
    for patch in lock["patches"]:
        if digest(flax / "patches/hermes" / patch["file"]) != patch["sha256"]:
            raise ValueError("Hermes patch does not match the source lock: " + patch["file"])
    source = cache / ("hermes-" + lock["revision"])
    stamp = source / ".flax-source.json"
    recipe = recipe_digest(lock)
    # A parent Git checkout would silently skip patches in this ignored cache.
    env = {**os.environ, "GIT_CEILING_DIRECTORIES": str(cache.resolve())}
    if stamp.is_file():
        saved = json.loads(stamp.read_text())
        if saved["recipe"] != recipe:
            raise ValueError("Hermes recipe changed; use a fresh --cache directory")
        for name, expected in saved["files"].items():
            if digest(source / name) != expected:
                raise ValueError("Prepared Hermes source changed: " + name)
        # Later patches can change earlier hunks; unwind the stack in a disposable copy.
        with tempfile.TemporaryDirectory(prefix="verify-patches-", dir=cache) as directory:
            copied = Path(directory) / "source"
            shutil.copytree(source, copied, symlinks=True)
            for patch in reversed(lock["patches"]):
                result = subprocess.run(["git", "apply", "--reverse",
                                         str(flax / "patches/hermes" / patch["file"])],
                                        cwd=copied, env=env, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL)
                if result.returncode:
                    raise ValueError("Prepared Hermes patches are missing; use a fresh --cache directory")
        return source
    if source.exists():
        raise ValueError("Unstamped Hermes source exists; use a fresh --cache directory")
    cache.mkdir(parents=True, exist_ok=True)
    if archive is None:
        archive = cache / "source.tar.gz"
        if not archive.exists():
            partial = archive.with_suffix(".part")
            with urllib.request.urlopen(lock["archive"]) as response, partial.open("wb") as out:
                shutil.copyfileobj(response, out)
            if digest(partial) != lock["sha256"]:
                raise ValueError("Downloaded Hermes archive hash mismatch")
            partial.rename(archive)
    if digest(archive) != lock["sha256"]:
        raise ValueError("Hermes source archive does not match the source lock")
    with tarfile.open(archive) as tar:
        # Upstream sources include symlinks; reject traversal and escaping links.
        for member in tar.getmembers():
            path = (cache / member.name).resolve()
            if not path.is_relative_to(source) or member.isdev():
                raise ValueError("Unsafe Hermes archive member: " + member.name)
            if member.issym() or member.islnk():
                link = (path.parent / member.linkname if member.issym()
                        else cache / member.linkname).resolve()
                if not link.is_relative_to(source):
                    raise ValueError("Unsafe Hermes archive link: " + member.name)
        tar.extractall(cache, filter="data")
    for patch in lock["patches"]:
        subprocess.run(["git", "apply", str(flax / "patches/hermes" / patch["file"])],
                       cwd=source, env=env, check=True)
        subprocess.run(["git", "apply", "--reverse", "--check",
                        str(flax / "patches/hermes" / patch["file"])],
                       cwd=source, env=env, check=True)
    files = {str(p.relative_to(source)): digest(p) for p in source.rglob("*") if p.is_file()}
    stamp.write_text(json.dumps({"recipe": recipe, "files": files}, indent=2) + "\n")
    return source


def build(flax: Path, cache: Path, archive: Path | None, jobs: int,
          target: str = "ios-simulator-arm64") -> Path:
    if target not in ("ios-device-arm64", "ios-simulator-arm64"):
        raise ValueError("Expected an iOS arm64 target")
    source = prepare(flax, cache, archive)
    host = cache / "host-tools"
    lock = json.loads((flax / "source.lock.json").read_text())["hermes"]
    common = [f"-DHERMES_RELEASE_VERSION={lock['version']}", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
              "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DCMAKE_OSX_ARCHITECTURES=arm64"]
    subprocess.run(["cmake", "-S", str(flax / "native/hermes"), "-B", str(host),
                    f"-DFLAX_HERMES_SOURCE={source}", *common], check=True)
    subprocess.run(["cmake", "--build", str(host), "--target", "hermesc", "shermes",
                    "--parallel", str(jobs)], check=True)
    platform = "iphonesimulator" if target == "ios-simulator-arm64" else "iphoneos"
    sdk = subprocess.check_output(["xcrun", "--sdk", platform, "--show-sdk-path"],
                                  text=True).strip()
    output = cache / target
    subprocess.run(["cmake", "-S", str(flax / "native/hermes"), "-B", str(output),
                   f"-DFLAX_HERMES_SOURCE={source}", *common,
                   "-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_SYSTEM_PROCESSOR=arm64", f"-DCMAKE_OSX_SYSROOT={sdk}",
                   "-DCMAKE_OSX_DEPLOYMENT_TARGET=16.3",
                   f"-DHERMES_APPLE_TARGET_PLATFORM={platform}",
                   f"-DIMPORT_HOST_COMPILERS={host}/ImportHostCompilers.cmake"], check=True)
    subprocess.run(["cmake", "--build", str(output), "--target", "hermesvm_a", "jsi",
                    "--parallel", str(jobs)], check=True)
    lock = json.loads((flax / "source.lock.json").read_text())["hermes"]
    includes = [source / "public", source / "API", source / "API/jsi"]
    libraries = [output / "hermes/lib/libhermesvm_a.a", output / "hermes/jsi/libjsi.a",
                 output / "hermes/external/boost/boost_1_86_0/libs/context/libboost_context.a"]
    files = {str(p): digest(p) for d in includes for p in d.rglob("*.h")}
    files.update({str(p): digest(p) for p in libraries})
    (output / "flax-hermes.json").write_text(json.dumps({
        "engine": "hermes", "target": target, "jit": False,
        "revision": lock["revision"], "recipe": recipe_digest(lock), "gcExtension": 1,
        "libraries": [str(p) for p in libraries], "includeDirs": [str(p) for p in includes],
        "files": files,
    }, indent=2) + "\n")
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    flax = Path(__file__).resolve().parents[1]
    parser.add_argument("--cache", type=Path, default=flax.parents[3] / ".local/flax-hermes")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--target", choices=("ios-device-arm64", "ios-simulator-arm64"),
                        default="ios-simulator-arm64")
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    print(build(flax, args.cache.resolve(), args.archive, args.jobs, args.target))
