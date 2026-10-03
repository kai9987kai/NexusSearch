# NexusSearch recovery - 2026-10-03

Restored location: `C:\Users\kai99\Desktop\NEXUS`.

## What was recovered

All **66 project files with recorded creation or edits** were reconstructed:
the C foundation, query parser and AST, file/JSON/text APIs, bitmap/regex/fuzzy/vector
modules, tests, build scripts, architecture/query contracts and research notes 01-09.
This includes the latest pre-deletion fixes rather than just the original handoff.

The original development logs contained 35 project files reconstructed through
55 successful operations. The continuation logs added or changed files through
25 successful patches and one literal function-name correction. Every original
edit matched its recorded starting text; all continuation patches replayed cleanly.
An independent log audit found no additional successful source/document writes.
Three failed historical patches were excluded.

Original empty directories were also recreated: `include/nexus`, `cmake`,
`bench`, `bindings/python`, `ui` and `plugins`.

The recovery reconstitutes recorded text. Original filesystem timestamps,
permissions, and exact encoding/newline bytes cannot all be proven from chat logs.
No matching copy was found in the Recycle Bin. Unrecorded files, if any existed,
cannot be certified recovered.

`RECOVERY_MANIFEST.json` lists hashes for the 66 reconstructed files before this
recovery documentation and backup utility were added. Subsequent source backups
contain an embedded manifest covering their own actual contents.

## Regenerated and omitted material

Compiled libraries, executables and build outputs are regenerated from source.
Two temporary intentionally failing test stubs under `build/agent-json` and old
build logs were not recreated. Historical commands were never executed during
text recovery. Private conversation logs are not included in the project or backup.

The following were planned, but had not been created before deletion: research
notes 10-17, independent fact checks and synthesis, generated full-Unicode data,
and the remaining engine/storage/application layers. The restored project is a
working foundation library, not a complete search application. The detailed
remaining work is in [NEXT_LAYER.md](NEXT_LAYER.md) and [PROGRESS.md](PROGRESS.md).

## Verification

- Fresh Debug build: 22/22 CTest entries passed (11 static, 11 shared).
- Fresh Release build with `NX_MEM_DEBUG=OFF`: 22/22 CTest entries passed.
- Debug with forced scalar execution and `NX_TEST_SEED=12345`: 22/22 passed.
- Backup utility: 9/9 Python tests passed, including archive content/hash parity,
  corruption detection, source-change detection, linked-path rejection and
  restoring an archive followed by backing up the restored project again.
- Compiler: MSYS2 UCRT64 GCC 15.2, C11, warnings treated as errors.
- Platform: x64 code under Windows ARM64 emulation. Linux/macOS, native ARM64,
  MSVC and sanitizer execution are not established by this recovery.

## Source backups

Run `python tools/backup_source.py` from the project directory. The default
destination is `C:\Users\kai99\Documents\NexusSearch Backups`. A completed ZIP
contains the `NEXUS` folder plus an embedded SHA256 file manifest. Build folders
and caches are omitted. The utility verifies archive contents before completing.
Its `Verified source backup` output identifies the completed archive. The prior
manifest inside an extracted backup is replaced with a fresh one on the next run.

Use `--output-dir <directory>` to choose another destination outside the project.
Backups run on demand; no scheduled task is installed. Restore by extracting
the ZIP into an empty directory, then follow the README build instructions.

Run the backup utility tests with:

```powershell
python -m unittest discover -s tests -p test_backup_source.py -v
```
