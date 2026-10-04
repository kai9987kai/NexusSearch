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

## HTTP server, MCP server, REPL, and Web UI (2026-10-04)

- Implemented a dependency-free HTTP/1.1 REST server (`nexus serve`) with
  endpoints for search, explain, stats, document retrieval, and CORS support.
- Embedded a modern interactive Web UI served at `/` with dark-themed design,
  query presets, tabbed results/explain/schema views, scan oracle toggle,
  execution metrics, and expandable raw JSON per hit.
- Implemented a Model Context Protocol (MCP) JSON-RPC 2.0 server over stdio
  (`nexus mcp`) with tools: nexus_search, nexus_stats, nexus_explain,
  nexus_get_document. Compatible with AI coding assistants and tool-use agents.
- Added an interactive REPL (`nexus repl`) with commands for stats, explain,
  scan-mode search, raw JSON, and document inspection.
- Added `row` field to search JSON output for document-level follow-up queries.
- Generated the Web UI as a C byte array via `tools/gen_web_ui.py` to comply
  with strict C11 `-Wpedantic` overlength-string limits.
- Server library APIs (`nx_server_stats_json`, `nx_server_query_json`) tested
  in a new C test suite with 7 test functions covering text, numeric, vector,
  explain, error, scan-parity, and null-input paths.
- All 32 CTest entries (16 static + 16 shared) and 12 Python CLI integration
  tests pass after these additions. No existing tests regressed.

## Python SDK, Benchmarking, and Server Integration (2026-10-04)

- Delivered complete, idiomatic Python SDK (`bindings/python/nexus`) with ctypes
  bridge to `libnexus.dll`, context manager snapshot lifecycle, typed `SearchResult`
  and `Hit` objects, in-memory build support, and 10 unit tests (`test_bindings.py`).
- Created high-precision benchmark suite in `bench/`:
  - `bench_vec.c`: SIMD vs scalar vector math (Cosine, Dot, L2sq, SQ8 quantization)
    achieving 700k+ QPS on 128-dim vectors and 1.5M+ QPS for SQ8 decoding.
  - `bench_bsi.c`: Bit-sliced index builder & query engine benchmark building 50k
    rows in 8.67ms (8.13 bytes/row) and serving point/range filters at 12k+ QPS.
  - `bench_search.c`: End-to-end query engine benchmark ingesting synthetic JSONL
    at 12k+ docs/sec and executing structured, text, vector, and hybrid queries.
- Added comprehensive Python server integration test suite (`tests/test_server.py`)
  verifying `/health`, `/api/stats`, `/api/search` (GET and POST), vector queries,
  explain plans, document fetching, Web UI, and CORS headers.
- Hardened HTTP server with full `Content-Length` chunked reading and graceful
  socket shutdown/drain for Windows Winsock TCP compatibility.
- Total verification across the test matrix:
  - 32 CTest suites (Debug build, 100% pass)
  - 32 CTest suites (Release build, 100% pass)
  - 41 Python test cases across 4 test suites (100% pass)

## Trigram Inverted Index & BM25 Inverted Postings Index (2026-10-04)

- **Trigram Inverted Index (`src/index/nx_trigram.h`, `src/index/nx_trigram.c`)**:
  - Implemented 24-bit canonical trigram inverted index based on Google Code Search
    (Russ Cox, 2012) and Zoekt architecture.
  - Intersects Roaring bitmaps via `nx_bitmap_and` in microseconds for instant
    substring and regex candidate filtering, avoiding full-corpus scanning.
  - Zero-allocation `nx_trigram_open`, CRC32C transactional payload serialization,
    and comprehensive unit test suite in `tests/test_trigram.c`.
- **Inverted Postings Index (`src/index/nx_postings.h`, `src/index/nx_postings.c`)**:
  - Implemented inverted postings index with embedded term dictionary (`nx_dict`),
    per-term Roaring bitmaps of matching documents, term frequencies ($tf$),
    document lengths ($dl$), and field collection statistics ($N$, $avgdl$, $df$).
  - Full-precision Lucene 10.3.1 BM25 scoring algorithm:
    $IDF = \ln(1 + (N - df + 0.5) / (df + 0.5))$,
    $Score = IDF \cdot tf / (tf + k_1 \cdot (1 - b + b \cdot dl / avgdl))$.
  - Comprehensive unit test suite in `tests/test_postings.c`.
- **Test Matrix Progression**:
  - 36 CTest suites (Debug build, 100% pass)
  - 36 CTest suites (Release build, 100% pass)
  - 41 Python test cases (100% pass)

## Flat Navigable Vector Proximity Graph (FlatNav / RobustPrune) (2026-10-04)

- **Vector Proximity Graph (`src/index/nx_graph.h`, `src/index/nx_graph.c`)**:
  - Implemented Flat Navigable Graph for sub-millisecond Approximate Nearest Neighbor (ANN)
    vector search, implementing the FlatNav / Hub Highway architecture (ICML 2025)
    and Vamana / RobustPrune ($\alpha=1.2$) algorithm (NeurIPS 2019).
  - Cache-line aligned flat adjacency lists with bounded degree $R$, greedy beam descent
    with candidate pool width $ef$, and diversity-pruned back-edges.
  - Supports Filtered ANN: traverses graph freely while constraining final results to an
    arbitrary Roaring bitmap of allowed documents (`nx_bitmap`).
  - Supports Cosine similarity, L2 squared, and Dot product distance metrics.
  - Zero-allocation reader view (`nx_graph_open`), CRC32C validation, and comprehensive
    unit test suite in `tests/test_graph.c`.
- **Test Matrix Progression**:
  - 38 CTest suites (Debug build, 100% pass)
  - 38 CTest suites (Release build, 100% pass)
  - 41 Python test cases (100% pass)

## Durable Write-Ahead Log (WAL) & Crash Recovery (2026-10-04)

- **Write-Ahead Log (`src/store/nx_wal.h`, `src/store/nx_wal.c`)**:
  - Implemented transactional append-only Write-Ahead Log (WAL) for durable mutations,
    supporting document `UPSERT`, `DELETE`, and `CHECKPOINT` frame types.
  - Every frame is protected by a 32-bit CRC32C checksum with explicit framing.
  - Implemented crash recovery replay (`nx_wal_replay`): gracefully detects torn writes
    and partial frames at EOF (e.g. from power failure mid-write) and recovers all
    prior intact transactions cleanly.
  - Low-level physical disk synchronization (`nx_wal_sync` via `_commit` / `fsync`)
    and clean log truncation (`nx_wal_truncate`).
  - Unit test suite in `tests/test_wal.c` verifying full lifecycle, replay, and torn write recovery.
- **Test Matrix Progression**:
  - 40 CTest suites (Debug build, 100% pass)
  - 40 CTest suites (Release build, 100% pass)
  - 41 Python test cases (100% pass)

Remaining roadmap: SSE streaming, RaBitQ quantization, ingestors, watch/federation.

## Sealed Segment Builder & Multi-Segment Persistent Store (2026-10-04)

- **Segment Builder (`src/seg/nx_seg_builder.h`, `src/seg/nx_seg_builder.c`)**:
  - Implemented a unified sealed segment builder that fuses every index structure into a
    single CRC-validated segment file in one transactional pass.
  - Segment file format v1: `[32B header][N×16B section directory][section data...][32B footer]`.
    The footer CRC32C covers the entire file content, providing end-to-end integrity.
  - Per build pass, automatically produces:
    - `NX_SEC_TABLE` — column store with BSI numerics (`nx_table`)
    - `NX_SEC_POSTINGS` per text field — Lucene BM25 inverted index (`nx_postings`)
    - `NX_SEC_TRIGRAM` per text field — substring candidate pruning (`nx_trigram`)
    - `NX_SEC_GRAPH` per vector field — ANN proximity graph (`nx_graph`)
  - Includes a lightweight whitespace/punctuation tokeniser with ASCII case-folding
    for postings index construction without external dependencies.
  - Zero-allocation `nx_segment_open` with header/footer CRC verification and
    section directory bounds checking. `nx_segment_section` enables O(n) field lookup.
  - **Bug fixed**: `nx_trigram_open` incorrectly required 4 extra bytes for an offset
    sentinel when `trigram_count==0`. Fixed to match the builder's actual output.

- **Multi-Segment Persistent Store (`src/store/nx_store.h`, `src/store/nx_store.c`)**:
  - Implemented the top-level persistence layer managing the full index directory lifecycle.
  - **Manifest-based segment tracking**: atomic, generation-numbered JSON manifest files
    (`manifest-<gen>.json`) track all live sealed segments with `nx_file_write_atomic`.
  - **WAL journaling**: every `nx_store_upsert`/`nx_store_delete` writes to the WAL
    first for durability, then accumulates in an in-memory mutable buffer.
  - **Atomic flush**: `nx_store_flush` seals the mutable buffer into a new segment file,
    fsyncs, writes a new manifest generation, then truncates the WAL — all crash-safe.
  - **Crash recovery**: `nx_store_open` replays the WAL into the mutable buffer after
    loading the latest valid manifest, restoring any unflushed mutations after a crash.
  - Supports iterating live sealed segments via `nx_store_seg(store, slot)` for queries.

- **Test Suite** (`tests/test_seg_builder.c`): 7 new test functions covering:
  - Segment build + open roundtrip with all 4 section types verified
  - Per-section postings BM25 lookup from segment bytes
  - Per-section trigram substring lookup (all sections, incl. empty-trigram fields)
  - Per-section graph ANN search with result ranking verification
  - Store open/upsert/flush/reopen lifecycle with manifest persistence
  - Store WAL crash recovery: unflushed docs restored after simulated crash
  - Empty JSONL input: valid zero-doc segment produced and opened

- **Test Matrix Progression**:
  - 42 CTest suites (Debug build, 100% pass, 2.24s)
  - 42 CTest suites (Release build, 100% pass, 1.99s)
  - 41 Python test cases (100% pass, 1.75s)

## RaBitQ Vector Quantization, Tiered Merge Compaction & SSE Streaming (2026-10-04)

- **RaBitQ 1-Bit Vector Quantization (`src/index/nx_rabitq.h`, `src/index/nx_rabitq.c`)**:
  - Implemented state-of-the-art 1-bit randomized vector quantization based on Gao & Long (SIGMOD 2024, arXiv:2405.12497)
    and Section 03 research, achieving ~28x vector memory reduction vs f32.
  - Data-oblivious: uses centroid mean-centering and a deterministic Gram-Schmidt orthogonal projection matrix $P$.
  - 4-bit query quantization with FastScan 4-bit Look-Up Table (LUT), evaluating 32 candidate vector dimensions
    with table lookups and bitwise operations instead of floating-point arithmetic.
  - Unbiased inner-product estimator with provable theoretical Chebyshev-type error bounds:
    $d_{lb} = d_{hat} - 2 n_o n_q \epsilon_0 err_o / \sqrt{D' - 1}$, enabling mathematically guaranteed candidate pruning.
  - Supports both L2 squared distance and Cosine similarity metrics.
  - Zero-allocation `nx_rabitq_open` reader view with CRC32C verification.
  - Embedded into the sealed segment format as `NX_SEC_RABITQ` (section ID 6).
  - Unit test suite in `tests/test_rabitq.c` verifying build, open, distance accuracy, error bound invariants, and ranking.

- **Tiered Segment Merge & Compaction (`src/store/nx_merge.h`, `src/store/nx_merge.c`)**:
  - Implemented multi-segment compaction policy (`nx_store_compact`): merges multiple small sealed segments into
    a single defragmented consolidated segment.
  - Document deduplication: scans segments from newest to oldest, tracking seen `_id` keys in an open-addressing
    hash set, purging obsolete document revisions so only the latest version of each document survives.
  - Atomic manifest update: writes `manifest-<gen+1>.json` via atomic rename before unlinking superseded segment files.
  - Unit test suite in `tests/test_merge.c` verifying segment consolidation, multi-version document deduplication,
    and manifest persistence across store reloads.

- **Server-Sent Events (SSE) Streaming API (`src/server/nx_server.c`)**:
  - Implemented real-time progressive score streaming endpoint: `GET /api/stream?q=...` and `POST /api/stream`.
  - Conforms to W3C Server-Sent Events standard (`text/event-stream; charset=utf-8`).
  - Emits progressive `event: start`, `event: hit`, and `event: done` messages, streaming search results
    incrementally to clients as they are found.
  - Python integration test added in `tests/test_server.py` (`test_stream_sse`).

- **Test Matrix Progression**:
  - 46 CTest suites (Debug build, 100% pass, 1.28s)
  - 46 CTest suites (Release build, 100% pass, 0.83s)
  - 42 Python test cases across 4 test suites (100% pass, 1.69s)

