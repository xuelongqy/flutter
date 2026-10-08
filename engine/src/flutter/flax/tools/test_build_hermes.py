"""Verify source patches are applied inside an ignored parent Git checkout."""

import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

from build_hermes import digest, prepare


class HermesSourceTest(unittest.TestCase):
    def test_patches_in_ignored_cache_and_source_integrity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".gitignore").write_text(".local/\n")
            flax = root / "flax"
            patches = flax / "patches/hermes"
            patches.mkdir(parents=True)
            patch = patches / "joint.patch"
            patch.write_text("diff --git a/api.h b/api.h\n--- a/api.h\n+++ b/api.h\n"
                             "@@ -1 +1 @@\n-old\n+joint\n")
            archive = root / "source.tar.gz"
            with tarfile.open(archive, "w:gz") as tar:
                item = tarfile.TarInfo("hermes-revision/api.h")
                item.size = 4
                tar.addfile(item, io.BytesIO(b"old\n"))
            lock = {"revision": "revision", "sha256": digest(archive),
                    "patches": [{"file": patch.name, "sha256": digest(patch)}]}
            (flax / "source.lock.json").write_text(json.dumps({"hermes": lock}))
            cache = root / ".local/hermes"
            source = prepare(flax, cache, archive)
            self.assertEqual((source / "api.h").read_text(), "joint\n")
            self.assertEqual(prepare(flax, cache, archive), source)
            (source / "api.h").write_text("changed\n")
            with self.assertRaisesRegex(ValueError, "source changed"):
                prepare(flax, cache, archive)
            # Reject an old producer stamp that recorded silently skipped patches.
            (source / "api.h").write_text("old\n")
            stamp = source / ".flax-source.json"
            saved = json.loads(stamp.read_text())
            saved["files"]["api.h"] = digest(source / "api.h")
            stamp.write_text(json.dumps(saved))
            with self.assertRaisesRegex(ValueError, "patches are missing"):
                prepare(flax, cache, archive)


if __name__ == "__main__":
    unittest.main()
