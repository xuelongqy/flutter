"""Verify exact-revision, idempotent, non-destructive patch application."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from apply_dart_patch import apply


class PatchTest(unittest.TestCase):
    def test_patch_boundaries(self):
        checkout = Path(__file__).resolve().parents[5]
        scratch = checkout / ".local/flax-patch-tests"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            root = Path(directory)
            repo = root / "dart"
            repo.mkdir()

            def git(*args):
                return subprocess.check_output(["git", *args], cwd=repo, text=True)

            git("init", "--quiet")
            git("config", "user.email", "patch-test@example.invalid")
            git("config", "user.name", "Patch test")
            source = repo / "source"
            source.write_text("before\n")
            git("add", "source")
            git("commit", "--quiet", "-m", "fixture")
            revision = git("rev-parse", "HEAD").strip()
            source.write_text("after\n")
            patch = root / "patch"
            patch.write_text(git("diff"))
            source.write_text("before\n")
            unrelated = repo / "unrelated"
            unrelated.write_text("keep\n")
            with self.assertRaisesRegex(RuntimeError, "revision mismatch"):
                apply(repo, patch, "0" * 40)
            self.assertEqual(source.read_text(), "before\n")
            apply(repo, patch, revision)
            apply(repo, patch, revision)
            self.assertEqual(source.read_text(), "after\n")
            self.assertEqual(unrelated.read_text(), "keep\n")
            source.write_text("conflicting edit\n")
            with self.assertRaises(subprocess.CalledProcessError):
                apply(repo, patch, revision)
            self.assertEqual(source.read_text(), "conflicting edit\n")
            self.assertEqual(unrelated.read_text(), "keep\n")


if __name__ == "__main__":
    unittest.main()
