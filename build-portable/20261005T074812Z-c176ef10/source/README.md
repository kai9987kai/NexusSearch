# NexusSearch 0.3

NexusSearch is a dependency-free C11 engine for searching local JSON documents.
Build an immutable snapshot, query it from the command line or Python, or explore
it in the included local browser interface. The active search path supports exact
integer filters, BM25 text ranking with optional prepared term/trigram indexes,
exhaustive cosine vector ranking and hybrid reciprocal rank fusion. Bounded batch
updates rebuild and atomically replace one validated snapshot.

## Build and test on this Windows machine

```powershell
Set-Location C:\Users\kai99\Desktop\NEXUS
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

CMake builds `nexus.exe`, `libnexus.a`, `libnexus.dll`, benchmarks, and tests for
both libraries. Keep the UCRT64 directory on PATH when using the DLL. No packages
are downloaded. `NX_BUILD_SHARED=OFF` disables the DLL and its test copies.
`NX_MEM_DEBUG=ON` enables guarded allocations; tests include leak checks and
allocation-failure sweeps. These checks do not replace ASan/UBSan/TSan, which
are unavailable in the installed GCC toolchain. Linux, macOS and other compiler
configurations require separate validation.

## Try a search

```powershell
.\build\nexus.exe build examples\documents.jsonl build\example.nxs
.\build\nexus.exe search build\example.nxs 'search AND active:true AND year:>=2025'
.\build\nexus.exe search build\example.nxs 'title:prefix(sea)' --prepared
.\build\nexus.exe search build\example.nxs 'embedding:[1,0,0] LIMIT 3'
.\build\nexus.exe explain build\example.nxs 'year:>=2025'
.\build\nexus.exe serve build\example.nxs --host 127.0.0.1 --port 8080
```

Open `http://127.0.0.1:8080` while the last command is running. The server reads
one fixed snapshot and handles local search and document inspection. It has no
authentication or TLS and is a development interface; keep it on loopback.
Use `repl SNAPSHOT` for an interactive terminal or `mcp SNAPSHOT` for the
experimental stdio tool interface.

The [search guide](docs/SEARCH.md) covers types, query syntax, scoring, the HTTP
API and Python examples. Vectors must be supplied by the caller; no embedding
model is included.

## Implementation status

| Area | Current behavior |
| --- | --- |
| Snapshot workflow | Bounded JSONL ingestion, unique IDs, inferred scalar/vector columns, original documents and integrity validation. CLI publication uses atomic file replacement. |
| Numeric search | Exact signed int64 bit-sliced comparisons, plus a scalar filter reference selected with `--scan`; float and Boolean filters are scanned. |
| Text search | Analyzed terms and phrases with corpus-wide BM25 statistics; exact original strings, prefix, bounded regex/fuzzy and substring matching. Optional exact postings/trigram preparation accelerates supported queries. |
| Vector and hybrid search | Every eligible stored vector receives an exact cosine score. Positive lexical/vector rankings combine using deterministic RRF. |
| Results | Stable ID tie breaking, explicit sort/pagination, binding-only explain and measured execution counters. |
| Interfaces | CLI, REPL, prepared local HTTP/browser and MCP search, Python ctypes bindings, and SSE delivery of completed results. |
| Snapshot updates | Bounded upsert/delete batches use an OS writer lock, final-schema validation and one-file atomic replacement. Readers reopen to observe the new snapshot. |
| Foundation | Guarded memory, bounded parsing, UTF-8/text profile, bitmap sets, CRC32C, file/mapping helpers and scalar/SIMD vector primitives. |
| Experimental modules | Standalone postings, trigram, proximity graph, RaBitQ-style quantization, sealed segments, WAL, persistent store and merge APIs. These are not used by the snapshot search command. |

The graph and quantization modules remain experimental, and snapshot queries do
not use ANN or approximate pruning. ANN recall and quantization error
need workload-specific evaluation against the exact engine. Checksums detect
corruption; they do not establish power-loss durability or transactional recovery
for the experimental multi-file store. Benchmark timings apply only to the
recorded machine, build, data and queries.

Useful test controls include `NX_TEST_SEED`, `NX_TEST_SCALE` and `NX_SIMD=scalar`.
Python integration checks use the standard library:

```powershell
python tests\test_cli.py --exe build\nexus.exe
python tests\test_server.py --exe build\nexus.exe
python tests\test_bindings.py
python tests\test_backup_source.py
```

See the [continuation ledger](docs/PROGRESS.md) for validation status and remaining
work, [module contracts](docs/LEAF_MODULES.md) for the foundation,
[language reference](docs/QUERY_LANGUAGE.md) and [architecture](docs/ARCHITECTURE.md)
for the wider roadmap, and [research update](docs/RESEARCH_UPDATE_2026-10-03.md) for
the sources informing the active engine. Roadmap syntax is broader than the
features currently executed. Older notes in `docs/research/` retain their original
evidence labels and have not been independently fact-checked as a set.

## Recovery and backups

The project was recovered from saved development records on 2026-10-03. All 66
recorded project files were reconstructed, including the later fixes; subsequent
development has added more files. See [recovery details](docs/RECOVERY.md).

```powershell
python tools/backup_source.py
```

Backups go to `Documents/NexusSearch Backups`, outside this project. Each ZIP
includes a file checksum manifest and is checked before completion. Build outputs
are excluded; rebuild them using the commands above. To recover a backup, extract
its `NEXUS` folder into an empty location. This is an on-demand local backup;
copy the ZIP to a separate drive or cloud storage if you need another copy.
