#!/usr/bin/env python3
"""Check fixed default entries with the ordinary built frontend and SDKs."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("debug", "release"), required=True)
    args = parser.parse_args()
    flax = Path(__file__).resolve().parents[1]
    source = flax.parents[1]
    checkout = source.parents[1]
    output = source / "out" / f"flax_mac_{args.mode}_arm64"
    scratch = checkout / ".local/default-call-tests"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=scratch) as temporary:
        work = Path(temporary)
        environment = dict(os.environ, TMPDIR=temporary, TMP=temporary, TEMP=temporary)
        library = work / "lib"
        library.mkdir()
        (library / "bindings.dart").write_text(
            "Object? flaxInvokeWithDefaults(Function target, List<Object?> slots, "
            "List<int> provided, List<String> layout) => "
            "throw StateError('Fixed entry was not compiled');\n"
        )
        packages = work / "package_config.json"
        packages.write_text(json.dumps({"configVersion": 2, "packages": [{
            "name": "flax", "rootUri": work.as_uri() + "/",
            "packageUri": "lib/", "languageVersion": "3.13",
        }]}))
        aot = args.mode == "release"
        platform = output / ("vm_platform_product.dill" if aot else "vm_platform.dill")
        frontend = [
            str(output / "dart-sdk/bin/dartaotruntime"),
            str(output / "frontend_server_aot.dart.snapshot"),
            "--target=vm", f"--sdk-root={output}/", f"--platform={platform}",
            f"--packages={packages}",
        ]
        if aot:
            frontend += ["--aot", "--tfa", "-Ddart.vm.product=true"]

        def compile_source(source, name, error=None):
            kernel = work / f"{name}.dill"
            result = subprocess.run(
                [*frontend, f"--output-dill={kernel}", str(source)],
                env=environment, capture_output=True, text=True,
            )
            text = result.stdout + result.stderr
            if error:
                if result.returncode == 0 or error not in text:
                    raise RuntimeError(f"{name} was not rejected correctly:\n{text}")
            elif result.returncode:
                raise RuntimeError(f"{name} compilation failed:\n{text}")
            return kernel

        results = {}
        for name in ("contract", "wide", "extension"):
            kernel = compile_source(flax / "tests/default_calls" / f"{name}.dart", name)
            if aot:
                image = work / f"{name}.aot"
                subprocess.run([
                    str(output / "gen_snapshot"), "--snapshot_kind=app-aot-elf",
                    f"--elf={image}", str(kernel),
                ], env=environment, check=True, capture_output=True)
                command = [str(output / "dart-sdk/bin/dartaotruntime"), str(image)]
            else:
                command = [str(output / "dartvm"), str(kernel)]
            run = subprocess.run(command, env=environment, check=True,
                                 capture_output=True, text=True)
            results[name] = json.loads(next(
                line for line in run.stdout.splitlines() if line.startswith("{")
            ))
            assert results[name]["passed"]

        for name, expression, error in (
            ("bad-layout", "flaxInvokeWithDefaults(print, [], [0], const [''])",
             "literal argument vector matching its layout"),
            ("dynamic-target", "flaxInvokeWithDefaults(target, [null], [0], const ['value'])",
             "statically known target"),
        ):
            fixture = work / f"{name}.dart"
            fixture.write_text(
                "import 'package:flax/bindings.dart';\n"
                f"Object? invoke(Function target) => {expression};\nvoid main() {{}}\n"
            )
            compile_source(fixture, name, error=error)
            results[name] = "rejected"
        print(json.dumps({"mode": args.mode, "passed": True, "results": results}))


if __name__ == "__main__":
    main()
