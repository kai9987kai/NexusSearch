# Search snapshots with NexusSearch 0.2

NexusSearch can build an immutable snapshot from JSON Lines and search it
locally. Integer filtering uses a bit-sliced index. Text ranking currently scans
stored text with BM25; vector ranking scores every eligible stored vector.
These are exact, inspectable baselines. No embedding model or remote service
is required; an optional local HTTP server provides the browser interface.

## First run

Build the project using the README, then run these commands from the project
folder in PowerShell:

```powershell
.\build\nexus.exe build examples\documents.jsonl build\example.nxs
.\build\nexus.exe stats build\example.nxs
.\build\nexus.exe search build\example.nxs 'search AND active:true AND year:>=2025'
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
| `search SNAPSHOT QUERY [--scan]` | Search and emit JSON hits, total matches and execution counters. `--scan` selects the independent numeric filter scan. |
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
```

Execution counters distinguish numeric index use, scanned cells, scored vectors
and bounded work. Text retrieval is currently a corpus scan. Standalone postings
and trigram modules exist, but still need integration with this binder and
ranking model before snapshot searches can use them.

## Browser and HTTP API

```powershell
.\build\nexus.exe serve build\example.nxs --host 127.0.0.1 --port 8080
```

Visit `http://127.0.0.1:8080`. The interface offers query presets, results, schema
and explain views, a numeric scan toggle, execution counters and original JSON
for each result. The server opens one snapshot at startup; rebuild and restart
to use a changed collection. It handles clients sequentially and has no
authentication or TLS. Keep this development interface on loopback.

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
interoperability with a particular client requires a client-level check.

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

# Build from bytes and search without writing a file.
encoded = Snapshot.build(b'{"_id":"demo","title":"local search"}\n')
with Snapshot.open(encoded) as snapshot:
    assert snapshot.search("title:search").total == 1
```

The wrapper owns snapshot bytes in Python memory. Opening a path reads the whole
file; it does not use the C mapping helper. The `with` syntax is supported, but
memory follows Python object lifetime. `search(scan=True)` selects the numeric
reference path. `limit` sets the allowed hit cap; use NexusQL `LIMIT` to request
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

`docs/QUERY_LANGUAGE.md` describes the wider language roadmap. Parsing syntax
does not mean this snapshot engine executes it. `WATCH`, `SOURCE`, `FACET`,
units, calendar/relative dates, embedding inference and clause parameters
such as `semantic(k=...)` fail explicitly when requested. Four-digit integer
years are supported as numeric values; calendar date interpretation remains
deferred.

Postings, trigrams, proximity graphs, RaBitQ-style quantization, sealed segments,
WAL, store and compaction exist as separate experimental C modules. The active
CLI, server and Python search use `nx_table` snapshots and `nx_search`, with
scanned text and exhaustive vectors. They do not query those multi-segment stores
or apply approximate pruning. Integration needs shared analysis semantics,
missing-value handling and comparisons with the exact engine. ANN recall and
quantization accuracy remain workload-specific; unit tests do not establish
universal bounds or production performance. Storage checksums and simulated
recovery tests do not establish power-loss durability of the multi-file store.

The [research update](RESEARCH_UPDATE_2026-10-03.md) records the sources behind
these implementation priorities. Sample vectors demonstrate geometry only;
they are not outputs of an embedding model or a relevance benchmark.

Run integration checks with Python 3 (standard library only):

```powershell
python tests\test_cli.py --exe build\nexus.exe
python tests\test_server.py --exe build\nexus.exe
python tests\test_bindings.py
```

CLI and server tests use temporary snapshots. Binding tests use `build/example.nxs`, creating it from the example corpus when
it is absent, plus temporary directories for publication/failure checks.
