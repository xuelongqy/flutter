"""Build the frozen unlinkJoint JS inputs and their APK asset replacements."""

import argparse
import hashlib
import json
from pathlib import Path
import zipfile


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("template_apk", type=Path)
    parser.add_argument("output_dir", type=Path, help="new directory for generated files")
    args = parser.parse_args()

    with args.template_apk.open("rb") as apk:
        digest = hashlib.file_digest(apk, "sha256").hexdigest()
        if digest != "e87c2a9a9361bb3e5cc4a9d9c0aa64dfdf0302118eee698666faa035c1095496":
            parser.error("template_apk must be the frozen batch trial APK (see README)")
        with zipfile.ZipFile(apk) as template:
            root = "assets/flutter_assets/"
            manifest = json.loads(template.read(root + "assets/modules/modules.json"))
            [core] = [entry for entry in manifest["modules"] if entry["specifier"] == "@flax/core"]
            original_core = template.read(root + core["asset"])
            original_workload = template.read(root + "assets/workload.js")

    source = Path(__file__).resolve().parent
    unlink = (source / "unlink-joint.js").read_bytes()
    workload = (source / "peer-workload.js").read_bytes()
    prefix = b"globalThis.__flaxModules.define("
    separator = b", function(module, exports, require) {\n"
    suffix = b"\n});\n"
    header_bytes, _, body = original_core[len(prefix):-len(suffix)].partition(separator)
    header = json.loads(header_bytes)

    body += b"\n" + unlink
    raw_core = prefix + header_bytes + separator + body + suffix
    core["artifact"] = header["artifact"] = sha256(body)
    packed_core = prefix + json.dumps(header, separators=(",", ":")).encode() + separator + body + suffix
    packed_manifest = (json.dumps(manifest, indent=2) + "\n").encode()

    base = original_workload.replace(
        b"import {computed, signal} from '@flax/core';",
        b"import * as flax from '@flax/core';\nconst {computed, signal} = flax;",
        1,
    ) + b"\n" + workload
    manual = base.replace(b"const manualRelease = false;", b"const manualRelease = true;", 1)

    output = args.output_dir
    output.mkdir()
    (output / "assets/modules").mkdir(parents=True)
    (output / "core.js").write_bytes(raw_core)
    (output / "base.js").write_bytes(base)
    (output / "manual.js").write_bytes(manual)
    (output / core["asset"]).write_bytes(packed_core)
    (output / "assets/modules/modules.json").write_bytes(packed_manifest)


if __name__ == "__main__":
    main()
