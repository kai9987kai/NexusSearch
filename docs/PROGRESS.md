# NexusSearch continuation ledger

## Current status: verified snapshot workflow (2026-10-04)

The active project is `C:\Users\kai99\Desktop\NEXUS`. The chat's starting
`didactic-lamp` checkout is unrelated and remains separate. NEXUS has no Git
repository; no repository has been initialized and no changes have been published.

The recovered foundation now supports an end-to-end snapshot search workflow.
This ledger supersedes earlier broad performance, ANN, streaming and durability
claims. A source module, a passing unit test and integration into the active
search path are different milestones.

### Active snapshot search path

- `nx_table` builds bounded JSONL into typed columns with unique IDs, original
  documents, integer indexes and format/checksum validation. Opening a snapshot
  also checks that stored JSON agrees with its typed cells.
- `nx_dict` provides sorted byte-string dictionary lookup/prefix ranges;
  `nx_bsi` provides exact signed int64 comparisons and explicit presence bits.
- `nx_search` binds all branches before execution. It supports typed comparisons,
  ranges/any-of/presence, Boolean logic, analyzed text/phrases, exact strings,
  prefix, substring, bounded regex/fuzzy, exhaustive cosine ranking and RRF.
- Integer index comparisons have a selectable independent scan path. Text
  matching/BM25 and float/Boolean filters currently scan stored cells. Vector
  scoring is exhaustive over the eligible stored vectors.
- Ranking uses whole-snapshot per-field statistics before structured filtering,
  explicit missing-value behavior, stable bytewise ID ties and final pagination.
  Explain binds without execution; analyze returns measured work counters.
- The CLI builds, searches, explains and inspects snapshots. Input and snapshot
  sizes are bounded; build validates before atomic replacement of the output.
- Local HTTP/browser, REPL and Python wrappers share the same snapshot engine.
  HTTP SSE sends a completed result as events; it does not stream hits during
  search. The server handles clients sequentially and is intended for loopback.
- The MCP stdio adapter exposes search, explain, schema and document tools as an
  experimental protocol subset. Client interoperability needs separate checks.

The [search guide](SEARCH.md) documents the usable commands and boundaries.
The [research update](RESEARCH_UPDATE_2026-10-03.md) separates source findings
from local engineering choices and explains the exact baselines.

### Separate experimental modules

The following files are implemented and have dedicated tests, but are **not
selected by `nx_search` or the CLI/server/Python snapshot search path**:

| Module | Implemented surface | Remaining evidence/integration |
| --- | --- | --- |
| `nx_postings` | Term dictionary, document frequency, term frequency, lengths and BM25 scoring. | Shared analysis semantics and engine integration; result/score comparisons with the current scan. |
| `nx_trigram` | Trigram-to-document candidate sets. | Engine integration with mandatory exact verification of candidates. |
| `nx_graph` | Bounded-degree proximity graph and filtered approximate search. | Workload-specific recall, latency, memory/build measurements against exhaustive scoring. |
| `nx_rabitq` | Experimental rotated, centered binary quantization and estimated distances. | Independent accuracy evaluation and mathematical review of the implemented error estimate. No guaranteed lossless pruning is claimed. |
| `nx_seg_builder` | Sealed sectioned files containing table, postings, trigram and optional vector indexes. | Consistent missing-value/analyzer behavior and query integration across sections/segments. |
| `nx_wal`, `nx_store`, `nx_merge` | Framed mutation log, manifests, segment flush/reopen and compaction APIs. | Failure-injection/recovery coverage across publication stages, synchronization/error handling and coherent live-document query semantics. |

Checksums establish integrity checks, not power-loss durability. Simulated torn
frames and reopen tests do not establish crash safety for all filesystems or
multi-file publication steps. No production transaction or multiwriter guarantee
is made. Approximate indexes and paper-derived error estimates do not establish
universal recall or pruning guarantees. Existing benchmark programs are local
measurement tools, not universal sub-millisecond or throughput claims.

### Validation checkpoint: 2026-10-04

Verified locally on Windows with MSYS2 UCRT64 GCC 15.2 (x64 binaries on an ARM64
host). Debug uses guarded allocation; Release has `NX_MEM_DEBUG=OFF`.

- Full CTest: **46/46 Debug** and **46/46 Release**, covering 23 suites through
  static and shared libraries. The final server-origin change was followed by
  the focused shared/static server checks and HTTP regression suite.
- Release interfaces: **12 CLI**, **16 HTTP**, **7 MCP**, **14 Python-binding**
  tests passed. Backup tooling: **9 tests** passed.
- Node renderer checks passed for object documents, nested execution counters,
  escaped document fields and text-only errors.
- Live browser checks passed for numeric indexed/scan parity (the same three
  IDs, with 1 versus 0 indexes and 6 versus 12 scanned cells), original document
  previews, explain/schema, visible query errors and exact vector ordering.
- The paired [benchmark receipt](benchmarks/2026-10-04-snapshot-search.json)
  records all 96 oracle-validated searches, raw timings and configuration.
  On its synthetic 10,000-row warm-cache data, indexed/scan median times were
  1.129/1.941 ms for 85 hits and 7.526/8.277 ms for 6,722 hits. These results are
  specific to this emulated host and workload; work-unit counts are not CPU time.

Review fixes include exact-text ANYOF binding, exists-only hybrid ranking,
regex argument validation and measured VM work budgets, bounded KMP substring
matching, JSON error escaping, strict HTTP parsing, document row overflow,
MCP frame/ID/notification/argument handling, browser preview/counter/error
rendering, and atomic Python snapshot publication. HTTP rejects unrelated
Origins and rebinding Host values. The server is still a sequential local
development service, not a hardened public deployment.

No sanitizer run or other-platform certification is claimed. Source backup
receipts are stored alongside their ZIPs in `Documents/NexusSearch Backups`;
the final archive is created after this ledger and all source edits are saved.

## Next implementation priorities

1. Integrate postings/trigram acceleration while preserving the exact engine's
   analysis, missing values, whole-corpus statistics, IDs and scores.
2. Add reproducible ANN/quantization comparisons across selective filters,
   missing vectors and correlated workloads; adopt approximate paths only with
   explicit recall and resource evidence.
3. Harden experimental storage publication/recovery and define live-document
   statistics before exposing mutation or multi-segment search through the CLI.
4. Continue language features deliberately: calendar/relative dates, units,
   `FACET`, `WATCH`, federation and embedding inference remain deferred.
5. Validate other operating systems/toolchains and add available sanitizers.

## Historical foundation and recovery (2026-10-02 to 2026-10-03)

The initial handoff overstated progress: core, CRC32C, the AST header and Unicode
generator existed while foundational modules were incomplete. Work supplied
file/mapping support, bitmap sets, regex/fuzzy, vectors, strict UTF-8/analysis,
bounded JSON and NexusQL parser/printer behavior. Fixes included arena rollback,
C11 portability, exact integers and locale-independent number conversion.

After accidental deletion of the complete project directory, all 66 recorded
project files were reconstructed from the original session and subsequent edits.
Fresh Debug/Release builds each passed 22 CTest entries (11 static/shared pairs)
at that recovery checkpoint; a forced-scalar run used a second test seed.
Subsequent development has added modules and enlarged the test matrix.

See [recovery details](RECOVERY.md) and [recorded source hashes](RECOVERY_MANIFEST.json).
The recovery manifest describes the reconstructed state before later documentation,
backup tooling and engine development. Each source-backup ZIP has its own current
checksum manifest. Backups remain outside the project in
`Documents/NexusSearch Backups` and exclude generated build outputs.
