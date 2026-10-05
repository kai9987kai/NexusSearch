"""Focused source-backup regression tests; no compiled library is required."""

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile


SPEC = importlib.util.spec_from_file_location(
    "backup_source", Path(__file__).resolve().parents[1] / "tools" / "backup_source.py")
backup_source = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(backup_source)


class BackupSourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "project"
        self.output = Path(self.temporary.name) / "backups"
        self.root.mkdir()
        (self.root / "README.md").write_bytes(b"Restored source\r\n")
        (self.root / "src").mkdir()
        (self.root / "src" / "main.c").write_bytes(b"int main(void) { return 0; }\n")
        (self.root / "empty").mkdir()

    def test_content_and_checksum_parity(self):
        archive = backup_source.create_backup(self.root, self.output)
        manifest = backup_source.verify_backup(archive)
        self.assertEqual(set(manifest["files"]), {"README.md", "src/main.c"})
        self.assertEqual(manifest["directories"], ["empty", "src"])
        with zipfile.ZipFile(archive) as zipped:
            for name, receipt in manifest["files"].items():
                expected = (self.root / name).read_bytes()
                self.assertEqual(zipped.read(f"NEXUS/{name}"), expected)
                self.assertEqual(receipt["sha256"], hashlib.sha256(expected).hexdigest())
                self.assertEqual(receipt["size"], len(expected))
            self.assertIn("NEXUS/empty/", zipped.namelist())

    def test_exclusions(self):
        for name in ["build", "build-release", "output", ".git", ".codex", ".claude", "__pycache__"]:
            directory = self.root / name
            directory.mkdir()
            (directory / "excluded.txt").write_text("generated or metadata", encoding="utf-8")
        (self.root / "src" / "cached.pyc").write_bytes(b"cache")
        archive = backup_source.create_backup(self.root, self.output)
        manifest = backup_source.verify_backup(archive)
        self.assertEqual(set(manifest["files"]), {"README.md", "src/main.c"})
        self.assertEqual(manifest["directories"], ["empty", "src"])

    def test_archive_names_are_unique(self):
        first = backup_source.create_backup(self.root, self.output)
        original = first.read_bytes()
        second = backup_source.create_backup(self.root, self.output)
        self.assertNotEqual(first, second)
        self.assertEqual(first.read_bytes(), original)

    def test_restored_project_can_be_backed_up_again(self):
        first = backup_source.create_backup(self.root, self.output)
        restored_parent = Path(self.temporary.name) / "restored"
        with zipfile.ZipFile(first) as zipped:
            zipped.extractall(restored_parent)
        restored = restored_parent / "NEXUS"
        self.assertTrue((restored / "BACKUP_MANIFEST.json").is_file())
        (restored / "README.md").write_bytes(b"Updated after recovery\n")
        second = backup_source.create_backup(restored, self.output)
        manifest = backup_source.verify_backup(second)
        self.assertEqual(set(manifest["files"]), {"README.md", "src/main.c"})
        self.assertNotIn("BACKUP_MANIFEST.json", manifest["files"])
        with zipfile.ZipFile(second) as zipped:
            self.assertEqual(zipped.read("NEXUS/README.md"), b"Updated after recovery\n")
        self.assertEqual(backup_source.verify_backup(first)["files"]["README.md"]["sha256"],
                         hashlib.sha256(b"Restored source\r\n").hexdigest())

    def test_nested_output_is_rejected_before_creation(self):
        for output in [self.root, self.root / "new-backups"]:
            with self.subTest(output=output):
                with self.assertRaises(backup_source.BackupError):
                    backup_source.create_backup(self.root, output)
        self.assertFalse((self.root / "new-backups").exists())

    def test_linked_source_is_rejected(self):
        outside = Path(self.temporary.name) / "external"
        outside.mkdir()
        (outside / "private.txt").write_text("do not archive", encoding="utf-8")
        try:
            (self.root / "linked").symlink_to(outside, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Creating symlinks is unavailable: {error}")
        with self.assertRaises(backup_source.BackupError):
            backup_source.create_backup(self.root, self.output)
        self.assertFalse(self.output.exists())

    def test_changed_source_discards_incomplete_backup(self):
        original_verify = backup_source.verify_backup

        def verify_then_modify(archive):
            result = original_verify(archive)
            (self.root / "README.md").write_bytes(b"changed during backup")
            return result

        with mock.patch.object(backup_source, "verify_backup", side_effect=verify_then_modify):
            with self.assertRaisesRegex(backup_source.BackupError, "Source changed"):
                backup_source.create_backup(self.root, self.output)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_added_source_discards_incomplete_backup(self):
        original_verify = backup_source.verify_backup

        def verify_then_add(archive):
            result = original_verify(archive)
            (self.root / "added.txt").write_text("new", encoding="utf-8")
            return result

        with mock.patch.object(backup_source, "verify_backup", side_effect=verify_then_add):
            with self.assertRaisesRegex(backup_source.BackupError, "inventory changed"):
                backup_source.create_backup(self.root, self.output)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_corrupted_file_fails_hash_verification(self):
        archive = backup_source.create_backup(self.root, self.output)
        tampered = self.output / "tampered.zip"
        with zipfile.ZipFile(archive) as source, zipfile.ZipFile(tampered, "w") as target:
            for item in source.infolist():
                data = source.read(item.filename)
                if item.filename == "NEXUS/README.md":
                    data = b"tampered"
                target.writestr(item, data)
        with self.assertRaisesRegex(backup_source.BackupError, "checksum mismatch"):
            backup_source.verify_backup(tampered)


if __name__ == "__main__":
    unittest.main()
