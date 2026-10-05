# NexusSearch architecture (v0 — refined by `docs/RESEARCH.md` decisions)

NexusSearch is an embeddable, dependency-free C11 engine that generalises the MTG search engine's idea —
*parse a query → turn each clause into an indexed result set → combine with set algebra* — to arbitrary typed objects,
and adds lexical ranking, fuzzy/regex/substring search, vector semantic search, a cost-based planner with `EXPLAIN`,
immutable mmap-able segments with incremental updates, live `WATCH` queries, federation, ingestors, an HTTP/SSE server,
an MCP server and bindings.

```
            ┌────────────── apps ──────────────┐
            │ nexus CLI │ HTTP+SSE+UI │ MCP │ py │
            └──────────────┬───────────────────┘
                           │ nexus.h (public C API, opaque handles)
 ┌─────────────────────────▼──────────────────────────────────────────────┐
 │ engine   nx_engine: open/close, add/update/delete docs, refresh/commit, │
 │          search(query)→hits, explain, watch, stats, federation          │
 ├──────────────┬─────────────────────────────┬────────────────────────────┤
 │ query        │ store                       │ ingest                     │
 │ lexer/parser │ memtable → segment builder  │ filesystem walker          │
 │ binder       │ segment set + tombstones    │ extractors (text/code/     │
 │ planner      │ snapshots (refcounted)      │ models/3d/img/pdf/json)    │
 │ executor     │ WAL + manifest + commit     │ embedder plugins           │
 │ ranker       │ merge policy, adaptive idx  │                            │
 ├──────────────┴─────────────────────────────┴────────────────────────────┤
 │ index structures (all: build → flat bytes; open(bytes) → read-only view)│
 │ roaring · bsi/columns · dict(FST/front-coded) · bm25 postings · trigram │
 │ regex · fuzzy · vec (flat/sq8/binary/HNSW)                              │
 ├─────────────────────────────────────────────────────────────────────────┤
 │ core: config mem arena buf hash/rng thread/pool simd file crc utf8 json │
 └─────────────────────────────────────────────────────────────────────────┘
```

## Principles

1. **One layout, two homes.** Every index structure has `*_build(...) → bytes` (into an `nx_buf`) and `*_open(slice) → view`
   that interprets the bytes **in place** (no deserialisation, offsets only, bounds-checked with `nx_cursor`).
   A segment is a single file made of such sections: heap-resident segments are the same bytes in an aligned heap block;
   on-disk segments are the same bytes `mmap`ed. There is exactly one reader code path.
2. **Immutable segments + overlays.** Segments never change after build. Deletes are per-segment tombstone bitmaps (copy-on-write
   generations). Updates = delete + add. Writes accumulate in an in-memory buffer; `refresh()` turns the buffer into a new small
   segment (near-real-time visibility); a tiered merge policy merges segments in the background.
3. **Snapshots.** A search pins an immutable `nx_snapshot` (refcounted list of segments + tombstone generations). Writers/mergers
   publish a new snapshot atomically; old segments are freed when their last snapshot is released. No reader ever takes a store lock
   while searching.
4. **Set algebra on bitmaps.** Each field index turns a clause into a Roaring bitmap (or a scored iterator for text/vector); the planner orders
   and combines them. Segment-local document ordinals are dense `uint32`.
5. **Plan, then run.** The planner (cost-based, per segment, using exact cardinalities from indexes + histograms) produces a physical plan tree;
   `EXPLAIN` prints it, `EXPLAIN ANALYZE` annotates actual rows/time. Semantic ranking runs only over the structured survivors, or filtered-HNSW
   when survivors are too many for brute force.
6. **Everything is bounded and hostile-input safe** (docs/CODING_STANDARD.md): files, JSON, HTTP bodies and queries.
7. **Deterministic and testable**: every structure has a brute-force oracle; the planner is differentially tested against a naive evaluator.

## Document model
A document is a JSON object with a unique string `_id`. A **schema** (`schema.json` in the index dir; inferred on first sight of a field if not declared)
maps field → type: `keyword`, `text`, `code` (text + trigram), `int`, `float`, `date`, `bool`, `size`/`count` (int with unit), `vector`, `stored` (JSON blob only).
Options per field: `index` (on by default), `store`, `multi`, `analyzer`, `unit`, `dims`/`metric`/`embedder` (vectors), `boost`, `fuzzy`.
The full original JSON is kept in the doc store. Reserved fields: `_id`, `_score`, `_seg`, `_ord`.

## Segment (one file, little-endian, section directory + CRC32C per section)
```
header(magic "NXSEG\0", fmt version, segment id, doc count, section count) │ section directory │ sections… │ footer(CRC of directory)
sections: ids(_id dict + ord→id) · docstore(offsets + json) · per field: dict, postings/bitmaps, columns(BSI/values), trigram, fuzzy aux, vectors, hnsw
          stats(per-field cardinalities, histograms, min/max, df tables) · optional sidecar indexes built later by the adaptive indexer
```
Index directory: `manifest-<gen>.json` (live segments + tombstone files + schema version), `seg-<id>.nxs`, `seg-<id>.del-<gen>` (roaring), `wal-<n>.log`, `schema.json`, `LOCK`.
Commit protocol: write new segment files → fsync → write manifest `gen+1` to temp → fsync → atomic rename → fsync dir; recovery picks the highest valid manifest and replays the WAL.

## Query pipeline
`text → parse (schema-free AST) → bind (schema: types, units, dates, expansion) → plan per segment (cardinality-ordered, cost-based)
→ execute (bitmaps / scored iterators; segments in parallel on nx_pool) → collect top-k per segment → merge → fuse/rank → hits (+explanations)`.
Global statistics (N, df, avg field length) are aggregated across the snapshot so BM25 scores are comparable across segments.

## Threading, ownership, lock order
* Engine handle is thread-safe. Lock order: `engine.write_mu` → `store.snapshot_mu` (tiny critical sections that swap a pointer/refcount) → `pool` internals.
* Searches: run on the caller thread + `nx_pool` workers (one task per segment; nested fan-out allowed — group wait helps).
* Background merge thread(s) and the adaptive indexer submit to a *separate* low-priority pool so they never starve queries.
* Callbacks (WATCH events, progress) are never invoked while holding any lock.

## Directory layout
```
include/nexus/nexus.h        public C API
src/core/                    foundation (done: config status mem arena buf hash thread simd; todo: file crc32c utf8 json)
src/index/                   roaring dict bsi columns bm25 trigram regex fuzzy vec/ (flat sq8 binary hnsw embed)
src/seg/                     schema, segment builder/reader, stats, tombstones
src/store/                   store, snapshot, wal, manifest, merge, adaptive
src/query/                   lexer/parser/ast, binder, planner, executor, ranker, explain
src/ingest/                  walker + extractors (+ plugin ABI)
src/engine/                  nx_engine (public API implementation), watch, federation
src/server/                  http, api routes, sse, mcp
src/cli/                     nexus command-line tool
ui/ bindings/python/ bench/ tests/ tools/ docs/
```
