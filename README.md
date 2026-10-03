# NexusSearch

Dependency-free C11 search-engine project, currently at the **leaf-library
milestone**. The parser and index primitives work; a complete search engine,
durable index store, CLI, HTTP/MCP server and web UI are not implemented yet.

## Build and test on this Windows machine

```powershell
Set-Location C:\Users\kai99\Desktop\NEXUS
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

CMake builds `libnexus.a`, `libnexus.dll`, and tests for both libraries. Keep
the UCRT64 directory on PATH when using the DLL. No packages are downloaded.
`NX_BUILD_SHARED=OFF` disables the DLL and its test copies. `NX_MEM_DEBUG=ON`
enables guarded allocations; tests include leak checks and allocation-failure
sweeps. These checks do not replace ASan/UBSan/TSan, which are unavailable in
the installed GCC toolchain. Windows atomic replacement requires the
FileRenameInfoEx operation supported by modern Windows/filesystems.

Useful reproducibility controls:

```powershell
$env:NX_TEST_SEED = "12345"
$env:NX_TEST_SCALE = "2"
$env:NX_SIMD = "scalar"
ctest --test-dir build --output-on-failure
Remove-Item Env:NX_TEST_SEED,Env:NX_TEST_SCALE,Env:NX_SIMD
```

## Current implementation

| Area | Available |
| --- | --- |
| Core | Guarded allocator, rollback arena, buffers/cursors, hash/RNG, threads/pool, runtime SIMD detection |
| Files | Bounded read-only mappings, atomic file replacement, CRC32C software/hardware/streaming/combine |
| Text | Strict UTF-8, explicit ASCII/fullwidth analysis profile, bounded JSON with exact numeric lexemes |
| Query | NexusQL parser, canonical printer, AST equality/dumps, spans and configurable limits |
| Sets | Immutable Roaring array/bitset portable format, membership, iteration, AND/OR/XOR/difference |
| Matching | Bounded Thompson byte-regex engine and Unicode-scalar Levenshtein distance |
| Vectors | Scalar/AVX2 dot, squared L2, cosine and per-vector uniform SQ8 |

These are low-level APIs in `src/core`, `src/index`, and `src/query`; they are
not a stable public engine API. Bitmap run containers, full Unicode normalization,
HNSW, RaBitQ, and ranked search are not implemented. Tests on this machine
exercise x64 code under Windows ARM64 emulation; native ARM64, Linux, macOS,
and MSVC execution still need validation.

See [module contracts](docs/LEAF_MODULES.md), [query language](docs/QUERY_LANGUAGE.md),
[continuation ledger](docs/PROGRESS.md), [architecture](docs/ARCHITECTURE.md),
and [next layer](docs/NEXT_LAYER.md). Research notes in `docs/research/` retain
their original evidence labels and are not independently fact-checked as a set.

## Recovery and backups

The project was recovered from its saved development records on 2026-10-03.
All 66 recorded project files were reconstructed, including the later fixes.
See [recovery details and verification](docs/RECOVERY.md).

Create a new source backup with Python 3:

```powershell
python tools/backup_source.py
```

Backups go to `Documents/NexusSearch Backups`, outside this project. Each ZIP
includes a file checksum manifest and is checked before completion. Build outputs
are excluded; rebuild them using the commands above. To recover a backup, extract
its `NEXUS` folder into an empty location. This is an on-demand local backup;
copy the ZIP to a separate drive or cloud storage if you need another copy.
