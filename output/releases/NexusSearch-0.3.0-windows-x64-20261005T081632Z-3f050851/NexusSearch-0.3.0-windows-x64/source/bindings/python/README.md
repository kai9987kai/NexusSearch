# NexusSearch Python bindings

A standard-library `ctypes` wrapper for the local NexusSearch C11 snapshot engine.
It supports typed document snapshots, text and numeric queries, exhaustive vector
ranking, hybrid RRF ranking, schema inspection and query explanations.

## Setup

Build the shared C library first using the project README. From the project root,
use the source wrapper directly:

```powershell
$env:PYTHONPATH = "$PWD\bindings\python"
$env:NEXUS_LIB_PATH = "$PWD\build\libnexus.dll"
```

Alternatively, install the local package:

```powershell
python -m pip install -e bindings/python
```

This installs the wrapper, not a prebuilt native library. Keep the matching
shared library available through `NEXUS_LIB_PATH` or `build/`.
The platform library names are `libnexus.dll`, `libnexus.so` and `libnexus.dylib`;
non-Windows builds need their own validation. Use wrapper and library files from
the same source revision because the C structure layout is part of the interface.

## Open and query

Create the example snapshot with the CLI first:

```powershell
.\build\nexus.exe build examples\documents.jsonl build\example.nxs
```

```python
from nexus import Snapshot

with Snapshot.open("build/example.nxs") as snapshot:
    print(snapshot.rows, snapshot.fields_count)
    print(snapshot.stats())

    result = snapshot.search("search AND year:>=2025 LIMIT 3")
    print(result.total, result.work, result.numeric_indexes)
    for hit in result:
        print(hit.id, hit.row, hit.score, hit.document)

    vectors = snapshot.search("embedding:[1,0,0] LIMIT 3")
    print([(hit.id, hit.score) for hit in vectors])
    print(snapshot.explain("year:>=2025 AND active:true"))
    print(snapshot.get_document(0))

    # Reuse an exact in-memory term/trigram preparation across many queries.
    with snapshot.prepare() as prepared:
        print(prepared.memory_bytes)
        result = prepared.search("search AND year:>=2025 LIMIT 3")
        print(result.prepared_text)
```

`SearchResult` contains hits, total matches, returned count and execution counters:
`work`, `numeric_indexes`, `scanned_cells`, `vectors_scored`, `explain_only`,
`indexed`, `has_lexical`, `has_vector` and `prepared_text`. `Snapshot.prepare()`
builds a bounded, immutable in-memory term/trigram index; keep its `Snapshot`
alive for the lifetime of the prepared handle. It accelerates exact terms,
phrases, prefixes and longer substrings; regex, fuzzy matching and substrings
shorter than three bytes retain the reference scan path. Preparation has a
bounded memory profile and can fail explicitly when a snapshot exceeds it.
Each `Hit` contains copied `id`, `row`,
`score`, `raw_document` and lazily parsed `document` values. `row` identifies a
position in this particular snapshot; `_id` is the persistent document identity.

`search(scan=True)` uses the independent numeric filter reference. `limit` sets
the permitted hit cap (default 20); an explicit NexusQL `LIMIT` must fit that
cap. For example, use `search("* LIMIT 100", limit=100)` to request 100 hits.
The snapshot engine returns at most 20 hits when no query `LIMIT` is specified,
clamped by the wrapper's lower cap if supplied.

`search` raises `NexusError` for invalid queries. `explain` returns a JSON-decoded
object that can contain an `error` field. File operations may raise normal Python
I/O exceptions. See the [search guide](../../docs/SEARCH.md) for complete supported
syntax and score semantics.

## Build from JSON Lines

```python
from nexus import Snapshot

source = (
    b'{"_id":"one","title":"local search","year":2026}\n'
    b'{"_id":"two","title":"document storage","year":2025}\n'
)

# Returns serialized snapshot bytes, with no file needed.
encoded = Snapshot.build(source)
with Snapshot.open(encoded) as snapshot:
    assert snapshot.search("title:search").total == 1

# Paths are accepted as inputs; output publication is optional.
Snapshot.build(source, output_path="my_index.nxs")
```

Documents require unique string `_id` values. Inferred fields must have consistent
types. Missing/null values remain missing; invalid JSON, mixed types, unsupported
nested values and inconsistent vector dimensions fail explicitly. Vectors are
finite float32 arrays supplied by the caller. No embedding model is included.

When saving, the wrapper writes a same-directory temporary file, checks the write,
flushes and synchronizes it, closes it, then uses `os.replace`. Failures before
replacement preserve the previous destination and clean up the wrapper's temporary
file. The containing directory is not synchronized; this is not a guarantee of
power-loss durability on every platform/filesystem.

## Current limits

- Opening a path reads its complete bytes into Python memory before C validation;
  this wrapper does not use memory mapping. JSONL inputs are likewise read before
  applying the C ingestion limits, so use bounded source files.
- The `with` syntax is supported, but snapshot memory follows Python object
  lifetime. There is no persistent file handle to close at context exit.
- Integer comparisons can use BSI. Text retrieval scans stored cells with BM25,
  while vector queries exhaustively score eligible vectors. Performance depends
  on the corpus, query, dimensions, platform and build configuration.
- Standalone postings, trigram, ANN, quantization, WAL/store and merge modules are
  experimental C APIs; this Python snapshot interface does not use them.
- Snapshot checksums detect corruption. They do not establish semantic relevance,
  general crash durability or calibrated confidence in result scores.

Run the wrapper regression suite from the project root:

```powershell
python tests\test_bindings.py
```
