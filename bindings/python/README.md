# NexusSearch Python SDK

High-performance, dependency-free embedded search engine written in C11.

## Installation

Install locally in development mode:

```bash
cd bindings/python
pip install -e .
```

Ensure `libnexus.dll` (Windows), `libnexus.so` (Linux), or `libnexus.dylib` (macOS) is built in `build/` or set `NEXUS_LIB_PATH`.

## Quickstart

```python
import nexus

# 1. Open an existing snapshot
with nexus.Snapshot.open("build/example.nxs") as snap:
    print(f"Loaded snapshot with {snap.rows} rows, {snap.fields_count} fields")

    # 2. Get snapshot schema & stats
    stats = snap.stats()
    for field in stats["fields"]:
        print(f"  Field: {field['name']} ({field['type']})")

    # 3. Search using NexusQL syntax
    results = snap.search("search AND year:>=2025")
    print(f"Found {len(results)} matches (total: {results.total})")

    for hit in results:
        print(f"#{hit.row} [{hit.score:.4f}] ID: {hit.id}")
        print(f"   Title: {hit.document.get('title')}")

    # 4. Vector similarity search
    vec_results = snap.search("embedding:[1,0,0] LIMIT 3")
    for hit in vec_results:
        print(f"Top vector match: {hit.id} (score: {hit.score:.4f})")

    # 5. Explain query execution plan
    plan = snap.explain("year:>=2025 AND active:true")
    print("Execution plan:", plan)
```

## Building Snapshots from Python

```python
import nexus

# Build from JSON Lines file or bytes
jsonl_data = b'''{"_id":"doc1","title":"Hello Nexus","year":2026,"active":true}
{"_id":"doc2","title":"High Performance Search","year":2025,"active":true}
'''

# Build to disk
nexus.Snapshot.build(jsonl_data, output_path="my_index.nxs")

# Or build in memory and query immediately
snap_bytes = nexus.Snapshot.build(jsonl_data)
with nexus.Snapshot.open(snap_bytes) as snap:
    res = snap.search("title:Nexus")
    assert len(res) == 1
```

## Features

- **Blazing Fast**: Sub-millisecond queries backed by C11 bit-sliced indexes (BSI) and SIMD vector kernels.
- **Dependency-Free**: Pure C core, wrapped with standard Python `ctypes`.
- **Hybrid Search**: Combines BM25 lexical ranking and Cosine vector similarity with Reciprocal Rank Fusion (RRF).
- **Exact Numeric Indexes**: Bit-sliced signed int64 filtering with zero index skew.
- **Interactive Web UI & REST**: Launch with `nexus serve snapshot.nxs --port 8080`.
