# Search snapshots with NexusSearch 0.2

NexusSearch can build an immutable snapshot from JSON Lines and search it
locally. Integer filtering uses a bit-sliced index. Text ranking currently scans
stored text with BM25; vector ranking scores every eligible stored vector.
These are exact, inspectable baselines. There is no embedding model or network
service to configure.

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
| `--help`, `--version` | Show usage or version. |

Successful data commands write one JSON value to stdout. Diagnostics go to
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
and bounded work. Text retrieval is currently a corpus scan, so larger corpora
need the planned postings/dictionary layer for speed.

## Boundaries and next work

`docs/QUERY_LANGUAGE.md` describes the wider language roadmap. Parsing syntax
does not mean this snapshot engine executes it. `WATCH`, `SOURCE`, `FACET`,
units, calendar/relative dates, embedding inference and clause parameters
such as `semantic(k=...)` fail explicitly when requested. No live updates,
multi-segment store, WAL, merges, HNSW, network server or browser UI is included
in this milestone. Four-digit integer years are supported as numeric values;
calendar date interpretation remains deferred.

The [research update](RESEARCH_UPDATE_2026-10-03.md) records the sources behind
these implementation priorities. The sample vectors demonstrate geometry only;
they are not outputs of an embedding model or a relevance benchmark.

Run CLI integration checks with Python 3 (standard library only):

```powershell
python tests\test_cli.py --exe build\nexus.exe
```

Tests use a temporary directory and never replace a user snapshot.
