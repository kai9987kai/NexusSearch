#!/usr/bin/env python3
"""Create and verify a local NexusSearch source backup using only Python.

Run when edits have stopped. Files are checked again before completion to detect
changes during the backup; this is not a filesystem-level transactional snapshot.
Generated build trees, caches, and repository/conversation metadata are excluded.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
from datetime import datetime, timezone
import uuid
import zipfile


MANIFEST_NAME = "NEXUS/BACKUP_MANIFEST.json"
EXCLUDED_DIRECTORIES = {".git", ".codex", ".claude", "__pycache__", "output"}


class BackupError(RuntimeError):
    """The requested backup could not be completed safely."""


def _checked_stat(path: Path) -> os.stat_result:
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or (
        getattr(info, "st_file_attributes", 0)
        & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    ):
        raise BackupError(f"Linked or reparse entry is not supported: {path}")
    return info


def _inventory(root: Path) -> tuple[list[str], list[str]]:
    directories: list[str] = []
    files: list[str] = []

    def visit(directory: Path) -> None:
        if not stat.S_ISDIR(_checked_stat(directory).st_mode):
            raise BackupError(f"Expected a directory: {directory}")
        for path in sorted(directory.iterdir(), key=lambda item: item.name):
            name = path.name.casefold()
            # Excluded trees are never followed, including linked build/output trees.
            if name in EXCLUDED_DIRECTORIES or name == "build" or name.startswith("build-"):
                continue
            if name.endswith(".pyc"):
                continue
            info = _checked_stat(path)
            relative = path.relative_to(root).as_posix()
            if relative.casefold() == "backup_manifest.json":
                if not stat.S_ISREG(info.st_mode):
                    raise BackupError("BACKUP_MANIFEST.json must be a regular metadata file")
                # A restored archive contains the previous receipt. Replace it
                # with a fresh receipt instead of treating it as source content.
                continue
            if stat.S_ISDIR(info.st_mode):
                directories.append(relative)
                visit(path)
            elif stat.S_ISREG(info.st_mode):
                files.append(relative)
            else:
                raise BackupError(f"Unsupported source entry: {path}")

    visit(root)
    return sorted(directories), sorted(files)


def _signature(info: os.stat_result) -> tuple[int, ...]:
    # On this Windows runtime lstat/fstat disagree on ctime for unchanged,
    # newly created files. Compare identity, size and modification time here;
    # the completed archive is also checked against a second full content hash.
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns)


def _read_stable(path: Path) -> bytes:
    before = _checked_stat(path)
    if not stat.S_ISREG(before.st_mode):
        raise BackupError(f"Expected a regular source file: {path}")
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        if _signature(opened) != _signature(before):
            raise BackupError(f"Source changed while opening: {path}")
        data = stream.read()
        after_read = os.fstat(stream.fileno())
    after = _checked_stat(path)
    if (_signature(before) != _signature(after_read)
            or _signature(before) != _signature(after)
            or len(data) != before.st_size):
        raise BackupError(f"Source changed while reading: {path}")
    return data


def verify_backup(archive: Path) -> dict:
    """Verify the exact member list and every file against the embedded manifest."""
    with zipfile.ZipFile(archive, "r") as backup:
        names = backup.namelist()
        if len(names) != len(set(names)):
            raise BackupError("Backup contains duplicate archive entries")
        manifest = json.loads(backup.read(MANIFEST_NAME))
        if manifest.get("format_version") != 1:
            raise BackupError("Unsupported backup manifest version")
        expected = {"NEXUS/", MANIFEST_NAME}
        expected.update(f"NEXUS/{name}/" for name in manifest["directories"])
        expected.update(f"NEXUS/{name}" for name in manifest["files"])
        if set(names) != expected:
            raise BackupError("Backup member list does not match its manifest")
        for name, receipt in manifest["files"].items():
            data = backup.read(f"NEXUS/{name}")
            if len(data) != receipt["size"] or hashlib.sha256(data).hexdigest() != receipt["sha256"]:
                raise BackupError(f"Backup checksum mismatch: {name}")
        bad = backup.testzip()
        if bad is not None:
            raise BackupError(f"Backup ZIP integrity check failed: {bad}")
    return manifest


def create_backup(source_root: Path, output_dir: Path | None = None) -> Path:
    """Back up stable regular source files; return only after full verification.

    An incomplete archive is removed on failure. The archive name is exclusively
    created, so existing files are never replaced. The command reports completion
    only after archive integrity and a second source inventory/hash pass succeed.
    """
    source_root = Path(source_root).absolute()
    _checked_stat(source_root)
    source_root = source_root.resolve(strict=True)
    if output_dir is None:
        output_dir = Path.home() / "Documents" / "NexusSearch Backups"
    output_dir = Path(output_dir).absolute().resolve()
    if output_dir == source_root or source_root in output_dir.parents:
        raise BackupError("The backup output directory must be outside the source project")
    directories, files = _inventory(source_root)
    output_dir.mkdir(parents=True, exist_ok=True)
    created = datetime.now(timezone.utc)
    archive = output_dir / f"NexusSearch-source-{created:%Y%m%dT%H%M%S.%fZ}-{uuid.uuid4().hex}.zip"
    manifest = {
        "format_version": 1,
        "created_utc": created.isoformat(),
        "snapshot": "Files and inventory rechecked; not a transactional filesystem snapshot",
        "directories": directories,
        "files": {},
    }
    # Opening with x is essential: a collision must fail without touching an
    # existing backup. Cleanup starts only after this exclusive creation succeeds.
    with archive.open("xb") as destination:
        try:
            with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED) as backup:
                backup.writestr("NEXUS/", b"")
                for name in directories:
                    backup.writestr(f"NEXUS/{name}/", b"")
                for name in files:
                    data = _read_stable(source_root / name)
                    manifest["files"][name] = {
                        "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                    }
                    backup.writestr(f"NEXUS/{name}", data)
                backup.writestr(MANIFEST_NAME, json.dumps(manifest, indent=2, sort_keys=True) + "\n")
            destination.flush()
            os.fsync(destination.fileno())
        except BaseException:
            destination.close()
            archive.unlink(missing_ok=True)
            raise
    try:
        verified = verify_backup(archive)
        if verified != manifest:
            raise BackupError("Backup manifest changed during verification")
        if _inventory(source_root) != (directories, files):
            raise BackupError("Source inventory changed during backup; run again when edits stop")
        for name in files:
            data = _read_stable(source_root / name)
            if hashlib.sha256(data).hexdigest() != manifest["files"][name]["sha256"]:
                raise BackupError(f"Source changed during backup: {name}")
    except BaseException:
        archive.unlink(missing_ok=True)
        raise
    return archive


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path,
                        help="Backup directory outside the project (default: Documents/NexusSearch Backups)")
    args = parser.parse_args()
    try:
        archive = create_backup(Path(__file__).resolve().parent.parent, args.output_dir)
    except (BackupError, OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"Backup failed: {error}", file=sys.stderr)
        return 1
    print(f"Verified source backup: {archive}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
