# NexusSearch continuation ledger

## Verified starting state (2026-10-02)

The task was opened in didactic-lamp, but the handoff's project is
`C:\Users\kai99\Desktop\NEXUS`. Work continues there. This directory has no
Git repository; existing files are preserved and no repository is initialized.

The prior workflow status `completed` does not mean the modules completed.
Layer 1's stored result has zero completed modules and no integration result.
Only the core, CRC32C implementation, AST header and Unicode generator survived.
Research notes 01-09 exist; synthesis and independent fact checks are incomplete.

## Execution decisions

- Complete and integrate the missing foundational modules before Layer 2.
  Existing architecture and language documents provide the implementation spec.
- Use independent module builds during parallel work and one final CMake build.
- Retain bounded scalar reference behavior where an optimized implementation
  has not been measured. Do not turn research proposals into performance claims.
- Add a physical arena rollback regression: oversized chunks inserted behind a
  mark must be released, not retained until reset. A failing regression reproduced
  growing live bytes; the empty-arena reset also retained its OOM flag.
- CRC table calls failed C11 pedantic compilation because nested-array const
  qualification is only implicitly allowed in newer C. Keep strict C11 flags.

## Completed foundation and recovery (2026-10-03)

- Core fixes, file/mmap, CRC tests, bitmap sets, regex, fuzzy matching and vector primitives.
- Strict UTF-8, documented normalization/tokenization, bounded JSON parser.
- NexusQL parser, canonical printer, structural roundtrip tests, year precision
  and locale-independent floating-point conversion.
- Static and shared library builds exercise the same 11 C test suites each.
- After accidental deletion of the complete project directory, recovered all
  66 source/document/tool/test files recorded in the original Claude session
  and the subsequent Codex edits. Fresh Debug and Release builds each pass all
  22 CTest entries, as does a forced-scalar Debug run with a second test seed.

See [recovery details](RECOVERY.md) and [recorded source hashes](RECOVERY_MANIFEST.json).
The hash manifest describes the reconstructed state before recovery documentation
and backup tooling were added; the backup ZIP has its own current-file manifest.

## Later roadmap (not yet implemented)

Dictionary, bit-sliced numeric index, columns/schema, BM25, trigram and HNSW;
segment builder/reader; binder/planner/executor/ranking; durable store/WAL/merge;
ingestors, watch/federation, CLI/HTTP/MCP/UI and Python bindings.
