"""Verify exact-revision, idempotent, non-destructive patch application."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from apply_dart_patch import apply, apply_all


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

  def test_consolidated_and_existing_patch_upgrade(self):
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
      source, ffi = repo / "source", repo / "ffi"
      source.write_text("base\n")
      ffi.write_text("before\n")
      git("add", ".")
      git("commit", "--quiet", "-m", "fixture")
      patches = root / "patches"
      patches.mkdir()
      (patches / "dart_revision").write_text(git("rev-parse", "HEAD").strip())
      source.write_text("current\n")
      (patches / "dart.patch").write_text(git("diff", "--", "source"))
      ffi.write_text("after\n")
      (patches / "dart-ffi.patch").write_text(git("diff", "--", "ffi"))
      source.write_text("previous\n")
      git("add", "source")
      source.write_text("current\n")
      (patches / "dart-startup.patch").write_text(git("diff", "--", "source"))
      git("reset", "--quiet", "HEAD")
      for initial in ("base", "previous", "current"):
        source.write_text(initial + "\n")
        ffi.write_text("before\n")
        (repo / "unrelated").write_text("keep\n")
        apply_all(repo, root)
        apply_all(repo, root)
        self.assertEqual(source.read_text(), "current\n")
        self.assertEqual(ffi.read_text(), "after\n")
        self.assertEqual((repo / "unrelated").read_text(), "keep\n")


if __name__ == "__main__":
  unittest.main()
