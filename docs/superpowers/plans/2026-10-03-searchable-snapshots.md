# Searchable snapshots implementation plan

> For agentic workers: use executing-plans for integration and independent module work.

**Goal:** Build, save and query a real typed local document collection from the CLI.

**Architecture:** Immutable borrowed column views with integrity validation;
bit-sliced integer filtering and a scan oracle; one query binder/executor shared
by the public C API and CLI. Full-corpus ranking precedes pagination.

**Tech stack:** C11, existing Nexus core, CMake/CTest and Python-only smoke tests.

**Spec:** ../specs/2026-10-03-searchable-snapshots-design.md

## Global constraints

- No new runtime dependencies. All C heap allocations use nx_mem.
- Exact int64, explicit missing values, stable bytewise ID tie breaking.
- Malformed files and unsupported query features fail closed.
- Work in Desktop/NEXUS; preserve the pre-upgrade backup and unrelated repositories.
- No performance claim without a reproducible comparison.

## Review focus

- Missing fields under != and NOT; pin semantics in engine differential tests.
- Four-digit numeric literals parsed as dates; bind back to exact integer for numeric fields.
- Empty/all-null datasets, duplicate IDs and nonfinite/dimension-mismatched vectors.
- Integer endpoints and malformed offsets/checksums in saved snapshots.
- Errors in unreachable OR/AND branches and unsupported trailing query clauses.

## Task 1: Exact index formats

Files: src/index/nx_dict.{h,c}, nx_bsi.{h,c}, tests/test_dict.c, test_bsi.c.
- [ ] Define borrowed immutable formats and transactional build contracts.
- [ ] Implement sorted dictionary lookup/prefix range and signed int64 BSI filters.
- [ ] Prove ordering, missing-value semantics, integer extremes, corrupt input and OOM against direct oracles.

## Task 2: Typed immutable table

Files: src/seg/nx_table.{h,c}, tests/test_table.c.
- [ ] Build bounded JSONL with unique IDs and consistent inferred scalar/vector field types.
- [ ] Persist typed columns, original documents and integer indexes with CRC/version validation.
- [ ] Expose field/cell/document access and BSI views with no borrowed lifetime ambiguity.
- [ ] Test roundtrip, schema rejection, corruption, truncation and allocation failures.

## Task 3: Bound query execution and ranking

Files: src/engine/nx_search.{h,c}, include/nexus/nexus.h, tests/test_search.c.
- [ ] Bind all clauses before execution; implement exact typed/missing-value semantics.
- [ ] Integrate numeric indexes and explicit scan oracle, text/vector ranking and deterministic result order.
- [ ] Implement useful explain metadata and explicit errors for remaining roadmap syntax.
- [ ] Compare result IDs/scores for random filters, complex Boolean expressions and edge conditions.

## Task 4: Usable local workflow

Files: src/cli/nexus.c, examples/, tests/test_cli.py, docs and CMake integration.
- [ ] Build/search/explain/stats commands with bounded I/O and JSON output.
- [ ] Exercise saved-file reopen, filtering, ranked text/vector results, bad queries and corrupted files.
- [ ] Run full Debug/Release/shared suites, review changes, document limitations and create a verified backup.
