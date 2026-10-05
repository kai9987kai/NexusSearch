# Search snapshots with NexusSearch 0.3

NexusSearch builds immutable snapshots from JSON Lines and searches them locally.
The default query engine provides exact BM25 text ranking and integer filters;
an optional prepared in-memory text index accelerates repeated terms, phrases,
prefixes and longer substrings without changing matches or scores. Vector ranking
still scores every eligible stored vector. No embedding model or remote service
is required; the optional local HTTP server provides the browser interface.

## First run

Build the project using the README, then run these commands from the project
folder in PowerShell:

```powershell
.\build\nexus.exe build examples\documents.jsonl build\example.nxs
.\build\nexus.exe stats build\example.nxs
.\build\nexus.exe search build\example.nxs 'search AND active:true AND year:>=2025'
.\build\nexus.exe search build\example.nxs 'title:prefix(sea)' --prepared
.\build\nexus.exe search build\example.nxs 'embedding:[1,0,0] AND active:true LIMIT 3'
.\build\nexus.exe explain build\example.nxs 'year:>=2025 AND active:true'
```

On Linux/macOS the corresponding executable path is `./build/nexus` (those
platforms require their own validation). Pass the entire query as one shell
argument. Windows command-line paths are converted from Unicode to UTF-8.

Commands:

| Command | Result |
| --- | --- |
| `build INPUT.jsonl OUTPUT.nxs` | Validate all documents, build and validate a snapshot, then atomically replace the output; report row/field/byte counts as JSON. |
| `update SNAPSHOT MUTATIONS.jsonl` | Apply an all-or-nothing upsert/delete batch by rebuilding one validated snapshot and atomically replacing it. |
| `search SNAPSHOT QUERY [--scan|--prepared]` | Search and emit JSON hits, total matches and execution counters. `--scan` selects the numeric filter oracle; `--prepared` builds a bounded exact text index for this process. |
| `explain SNAPSHOT QUERY` | Bind and validate the query without executing it. Pass a query without an `EXPLAIN` prefix. |
| `stats SNAPSHOT` | Validate the snapshot and return its row count and inferred field schema. |
| `serve SNAPSHOT [--host 127.0.0.1] [--port 8080]` | Serve the local browser UI and read-only HTTP API for this snapshot. |
| `repl SNAPSHOT` | Start an interactive query shell; use `:help` for inspection commands. |
| `mcp SNAPSHOT` | Start the experimental MCP stdio tool interface. |
| `--help`, `--version` | Show usage or version. |

Successful `build`, `search`, `explain` and `stats` commands write one JSON value
to stdout. Diagnostics go to
stderr; exit status is 0 for success, 1 for a runtime/input error, and 2 for
incorrect command-line usage. Snapshot validation fails before results are
published. Build reads at most 64 MiB of JSONL, and commands open at most 256 MiB
of snapshot data. A failed ingestion never replaces an existing output. An I/O
error during final replacement may still mean the rename occurred; inspect the
output before retrying. This is a single-file snapshot workflow, not a
multi-file transactional store.

## Documents and inferred types

Use one JSON object per line. Every document needs a unique string `_id`.
Strings are text; booleans stay boolean; integers retain exact signed 64-bit
values; fractional numbers are float64; numeric arrays are float32 vectors of
a consistent dimension. Do not mix incompatible field types. Unsupported
values, duplicate object keys, invalid UTF-8, invalid vectors and duplicate
IDs fail ingestion explicitly.

Missing fields remain missing and do not satisfy ordinary comparisons.
`field:*` checks presence. Keep original JSON separately from analyzed text so
`=` can mean exact original bytes. Field names are case-sensitive. Bare search
checks text fields except `_id`; search IDs explicitly with `_id="paper-01"`.
The example corpus includes a document with missing numeric/vector fields.

## Executed query syntax

| Query | Meaning |
| --- | --- |
| `search` | Search an analyzed term across default text fields. |
| `title:search` | Search a term in one text field. |
| `body:"document search"` | Match consecutive analyzed phrase terms. |
| `title="Local search in C"` | Exact original text, including case. |
| `year:>=2025` or `year>=2025` | Integer comparison; a four-digit value resolves as an integer for this schema. |
| `year:2024..2026` | Inclusive numeric range. |
| `created:2024` | Match ISO calendar dates in the whole UTC year on a text field. |
| `created:2024-02` | Match values in that UTC calendar month. |
| `created:>=2024-02-29` | Compare an ISO date/time text value with a calendar range. |
| `created:now-7d` or `created:today+1mo` | Resolve a relative UTC time once for the query. |
| `created:2024-02-01..2024-02-29` | Inclusive date range; partial endpoints include their whole calendar interval. |
| `rating:>4.5` | Float comparison. |
| `active:true` | Boolean equality. |
| `embedding:*` | Vector field is present. |
| `title:prefix(sea)` | Prefix match on analyzed terms. |
| `body:substr("immutable")` | Literal substring matching. |
| `title:fuzzy(serch,1)` | Bounded fuzzy matching of analyzed terms. |
| `body:/snapshots?/` | Bounded byte-regex matching. |
| `embedding:[1,0,0]` | Exhaustive cosine ranking over eligible stored vectors. |
| `search AND active:true` | Boolean intersection; adjacent clauses also mean AND. |
| `title:search OR title:storage` | Boolean union. |
| `NOT active:false` | Boolean negation. |
| `search SORT BY year DESC, _id ASC LIMIT 10 OFFSET 0` | Explicit ordered pagination. |

Text analysis uses the versioned ASCII/fullwidth profile from the leaf library:
ASCII case folding and fullwidth ASCII compatibility mapping. It is not full
Unicode normalization, stemming or general Unicode case folding. Regex works
on UTF-8 bytes, while fuzzy distance uses Unicode scalar values. Neither gives
grapheme-aware language analysis.

Calendar filters operate on **text** fields containing `YYYY-MM-DD` values,
optionally followed by `T` and `HH:MM[:SS[.fraction]]`, then `Z` or a numeric
`+/-HH:MM` offset. A date-only value is UTC midnight. A date/time without a zone
is also interpreted as UTC; write an explicit `Z` or offset when the source has
one. Years are `0001` through `9999`; seconds are `00` through `59`, and leap
seconds are not supported. Offsets are normalized to UTC before comparison.
Invalid values in a text column simply do not match; they do not invalidate the
snapshot. Partial query dates (`YYYY`, `YYYY-MM`, `YYYY-MM-DD`, and shorter
date-times) denote half-open UTC intervals. Thus `>=2024-02` includes February,
while `>2024-02` starts after the entire month. `<` and `<=` follow the same
whole-interval boundary rule. Relative `now`, `today`, and `yesterday` use UTC;
`now` is captured once per query, `today`/`yesterday` start at UTC midnight, and
`s`, `m`, `h`, `d`, `w`, `mo`, and `y` mean seconds, minutes, hours, days, weeks,
calendar months, and calendar years. Month/year arithmetic clamps to the last
valid day of the destination month. Integer fields retain four-digit-year
numeric behavior; there is no inferred `DATE` field type or date-specific index.

Type-checking is strict: an unknown field or unsupported type/operator pair is
an error, even when an earlier filter would find no matches. Limits default to
20 hits, with a maximum of 1000; at most eight explicit sort keys are accepted.
Missing sort values appear last for either direction. Equal final scores break
ties by bytewise `_id` order, independent of ingestion order.

## Scores and explanations

BM25 uses `k1=1.2`, `b=0.75`, exact token lengths and whole-snapshot per-field
statistics. It omits the optional `(k1+1)` multiplier. Structured filters do
not redefine the lexical corpus statistics. Querying a vector requires an
explicit numeric vector of the stored dimension; the engine does not turn text
into embeddings. Cosine scores are computed exhaustively after typed filtering.

When positive lexical and vector clauses coexist, ranking combines their
survivor rankings by reciprocal rank fusion with constant 60 and complete
survivor windows. A result's score is then a fusion score. None of these scores
is a calibrated relevance probability. The implementation and its independent
scalar filter mode should return the same hit IDs and scores.

`explain` (or a query starting with `EXPLAIN`) validates/binds without searching.
To execute and inspect measured work, use:

```powershell
.\build\nexus.exe search build\example.nxs 'EXPLAIN ANALYZE year:>=2025 AND active:true'
.\build\nexus.exe search build\example.nxs 'year:>=2025 AND active:true' --scan
.\build\nexus.exe search build\example.nxs 'title:search' --prepared
```

Execution counters distinguish numeric index use, text postings versus scan
work, scanned cells, scored vectors and bounded work. The optional prepared
index is immutable and safe for concurrent queries with separate result objects;
its table bytes must outlive it. It uses at most 256 MiB for tracked build/index
allocations, 64 MiB of source text and four million tokens. A single text value
is capped at 1 MiB. Regex, fuzzy queries and substrings shorter than three bytes
use the exact scan path. Phrase and substring candidates are checked against
normalized source text before acceptance. Preparation is process-local and is
rebuilt after snapshot updates or restart; it is not stored beside the snapshot.

The [prepared-text benchmark receipt](benchmarks/2026-10-05-prepared-text.json)
records 21 alternating warm query pairs for sparse/common terms, ordered
phrases and substrings on a deterministic 10,000-document synthetic corpus.
Each run checks membership against the generated rows and compares scan and
prepared IDs, ordering and BM25 scores. On this Windows ARM64 host running the
x64 build under emulation, median prepared-query times were 0.59 ms vs 8.54 ms
for the sparse term, 7.11 ms vs 16.08 ms for the common term, 3.77 ms vs 10.47 ms
for the phrase, and 0.45 ms vs 5.15 ms for the substring. Preparation took
about 134 ms and used about 3.46 MB in the measured build. This is one warm-cache
synthetic workload, not a general performance guarantee; timings vary by host,
corpus, term frequency, cache state and build.

Snapshot updates take a nonblocking operating-system writer lock on a persistent
`SNAPSHOT.nxs.lock` sidecar, validate every operation and the final whole-table
schema, then replace the snapshot as one file. Existing readers keep their old
mapping and must reopen to see updates. All writers must use this API and the
same path spelling; external replacement and path aliases do not participate in
the lock. The sidecar must not be deleted while an updater may be running. On an
I/O error during publication, reopen the snapshot before deciding whether to
retry. This is not a universal power-loss durability or multi-file transaction
claim.

## Browser and HTTP API

```powershell
.\build\nexus.exe serve build\example.nxs --host 127.0.0.1 --port 8080
```

Visit `http://127.0.0.1:8080`. The server prepares a bounded exact text index at
startup and falls back to scan search if preparation exceeds resource limits.
The interface offers query presets, results, schema
and explain views, a numeric scan toggle, execution counters and original JSON
for each result. The server opens one snapshot at startup; restart it to use a
changed collection and rebuild its in-memory preparation. It handles clients sequentially and has no
authentication or TLS. Keep this development interface on loopback. Browser requests
must come from the same origin; unrelated origins and unrecognized Host values
are rejected, and wildcard CORS access is disabled. Requests are bounded to
64 KiB, query strings to 2,047 UTF-8 bytes, with five-second socket I/O timeouts.
POST JSON supports escaped quotes and Unicode; malformed framing, NULs and
truncated query values are rejected.

| Route | Purpose |
| --- | --- |
| `GET /health` | Check that the process is serving requests. |
| `GET /api/stats` | Retrieve row count and field schema. |
| `GET /api/search?q=QUERY&scan=false` | Query the snapshot; URL-encode the query. |
| `POST /api/search` | Send JSON such as `{"query":"year:>=2025","scan":false}`. |
| `GET /api/explain?q=QUERY` or `POST /api/explain` | Validate/bind a query without running it; POST uses the same JSON shape. |
| `GET /api/doc?row=0` | Retrieve original JSON by the snapshot's zero-based row number. |
| `GET /api/stream?q=QUERY` or `POST /api/stream` | Deliver a completed search as SSE `start`, `hit`, `done` events. |

SSE delivery begins after the search has completed. It does not report provisional
hits or reduce the work needed to compute the final ranking. Row numbers refer
to this particular snapshot; use `_id` as the persistent document identity.

The `mcp` command exposes `nexus_search`, `nexus_stats`, `nexus_explain` and
`nexus_get_document` over stdin/stdout. This is an experimental protocol subset;
interoperability with a particular client requires a client-level check. It
advertises protocol version `2024-11-05`, uses newline-delimited frames up to
64 KiB, echoes numeric/string IDs without conversion, suppresses notification
responses, and reports malformed requests and unknown methods explicitly.
It does not implement arbitrary JSON-RPC batches or all current MCP features.

## Python

Build the shared library, then install the local wrapper or put it on your
Python import path:

```powershell
$env:PYTHONPATH = "$PWD\bindings\python"
$env:NEXUS_LIB_PATH = "$PWD\build\libnexus.dll"
```

```python
from nexus import Snapshot

with Snapshot.open("build/example.nxs") as snapshot:
    results = snapshot.search("search AND year:>=2025 LIMIT 3")
    print(results.total, results.work)
    for hit in results:
        print(hit.id, hit.score, hit.document)
    print(snapshot.stats())
    print(snapshot.explain("year:>=2025"))
    print(snapshot.get_document(0))

    # Keep an exact in-memory text preparation for repeated searches.
    with snapshot.prepare() as prepared:
        result = prepared.search("title:search AND year:>=2025")
        print(result.prepared_text, prepared.memory_bytes)

# Build from bytes and search without writing a file.
encoded = Snapshot.build(b'{"_id":"demo","title":"local search"}\n')
with Snapshot.open(encoded) as snapshot:
    assert snapshot.search("title:search").total == 1
```

The wrapper owns snapshot bytes in Python memory. Opening a path reads the whole
file; it does not use the C mapping helper. The `with` syntax is supported, but
snapshot memory follows Python object lifetime. `Snapshot.prepare()` creates a
bounded immutable C index that borrows the snapshot; keep the snapshot open and
close the prepared object with `with` after use. `search(scan=True)` selects the
numeric filter reference and disables prepared text. `limit` sets the allowed hit cap; use NexusQL `LIMIT` to request
a smaller page explicitly. Results include copied IDs/documents and the engine's
execution counters. `search` raises `NexusError` on query failures; `explain`
returns an error object for an invalid query.

`Snapshot.build(..., output_path=...)` writes a temporary file in the destination
directory, flushes and synchronizes it, closes it, then replaces the destination.
Pre-replacement failures preserve an existing snapshot. This does not synchronize
the containing directory or promise power-loss durability on every filesystem.
The wrapper reads source files before applying the C ingestion limits, so use
bounded inputs. See [Python wrapper documentation](../bindings/python/README.md).

## Boundaries and next work

`docs/QUERY_LANGUAGE.md` describes the wider language and roadmap. Parsing
syntax does not mean this snapshot engine executes it. `WATCH`, `SOURCE`,
`FACET`, field-declared numeric units, embedding inference and clause parameters
such as `semantic(k=...)` fail explicitly when requested. Calendar/relative
date filters are implemented only for ISO strings in inferred text fields;
there is no date field type, configurable time zone, date index or local-time
interpretation. Four-digit integer years remain numeric values.

The graph, RaBitQ-style quantization, sealed segments, WAL, store and compaction
remain separate experimental C modules. Snapshot search does not query those
multi-segment stores or apply approximate pruning. ANN recall and quantization
accuracy remain workload-specific; unit tests do not establish universal bounds
or production performance. Storage checksums and simulated recovery tests do
not establish power-loss durability of the multi-file store.

The [research update](RESEARCH_UPDATE_2026-10-03.md) records the sources behind
these implementation priorities. Sample vectors demonstrate geometry only;
they are not outputs of an embedding model or a relevance benchmark.

Run integration checks with Python 3 (standard library only):

```powershell
python tests\test_cli.py --exe build\nexus.exe
python tests\test_server.py --exe build\nexus.exe
python tests\test_mcp.py --exe build\nexus.exe
python tests\test_bindings.py
```

CLI and server tests use temporary snapshots. Binding tests use `build/example.nxs`, creating it from the example corpus when
it is absent, plus temporary directories for publication/failure checks.

The browser renderer also has focused Node.js checks: `node tests/test_web_ui.cjs`.
Regenerate the embedded page after editing its source with `python tools/gen_web_ui.py`.
