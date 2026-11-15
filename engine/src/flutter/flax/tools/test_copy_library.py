"""Keep packaged libraries independent from verified SDK inputs."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest


class CopyLibraryTest(unittest.TestCase):

  def test_atomic_copy_detaches_existing_hardlink(self):
    script = Path(__file__).with_name("copy_library.py")
    with tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"]) as directory:
      root = Path(directory)
      source = root / "sdk/libv8.dylib"
      source.parent.mkdir()
      source.write_bytes(b"verified SDK library")
      source.chmod(0o755)
      output = root / "framework/Libraries/libv8.dylib"
      output.parent.mkdir(parents=True)
      os.link(source, output)
      self.assertTrue(os.path.samefile(source, output))

      for _ in range(2):
        result = subprocess.run([sys.executable, str(script), str(source), str(output)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(output.read_bytes(), source.read_bytes())
        self.assertFalse(os.path.samefile(source, output))
        self.assertEqual(output.stat().st_mode & 0o777, 0o755)
      output.write_bytes(b"signed or stripped delivery")
      self.assertEqual(source.read_bytes(), b"verified SDK library")
      self.assertEqual(list(output.parent.iterdir()), [output])

  def test_missing_input_preserves_existing_output(self):
    script = Path(__file__).with_name("copy_library.py")
    with tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"]) as directory:
      root = Path(directory)
      output = root / "library"
      output.write_bytes(b"existing output")
      result = subprocess.run([sys.executable, str(script), str(root / "missing"), str(output)],
                              capture_output=True, text=True)
      self.assertNotEqual(result.returncode, 0)
      self.assertEqual(output.read_bytes(), b"existing output")
      self.assertEqual(list(root.iterdir()), [output])


if __name__ == "__main__":
  unittest.main()
