# Research update: a trustworthy first search engine

Reviewed 2026-10-03. This note guides the next implementation milestone; it is
not a claim that the full architecture is implemented. Sources below were
opened during this review. Existing `docs/research/01` through `09` retain their
original evidence labels and have **not** been independently checked as a set.
Decisions and proposed acceptance tests below are NexusSearch engineering
choices, distinguished from the cited authors' results.

## 1. Freeze a transparent BM25 baseline

Use the existing lexical note's Lucene-style variant:

```text
idf = log(1 + (N - df + 0.5) / (df + 0.5))
term_score = boost * idf * tf / (tf + k1 * (1 - b + b * dl / avgdl))
```

There is no additional `(k1 + 1)` factor. Default `k1=1.2`, `b=0.75`; validate
finite nonnegative `k1` and boosts, and `0 <= b <= 1`. Return zero for absent
terms before dividing. Exact token counts and double arithmetic make the
first implementation inspectable; they do not promise bit-identical Lucene
scores, which use encoded norms and float operations. Store raw `tf` and lengths,
not scores frozen at ingestion. These choices follow the current
[Lucene 10.3.1 API](https://lucene.apache.org/core/10_3_1/core/org/apache/lucene/search/similarities/BM25Similarity.html)
and [versioned implementation](https://raw.githubusercontent.com/apache/lucene/releases/lucene/10.3.1/lucene/core/src/java/org/apache/lucene/search/similarities/BM25Similarity.java).
Conventional defaults are not evidence of optimal relevance on the user's data.

## 2. Make statistics and analysis part of the snapshot contract

Compute per-field `N`, `df`, and `avgdl` over the whole visible snapshot, before
the query's filter. Define `N` as documents with at least one indexed term in
that field; keep field presence separately for empty/missing-value semantics.
Never mix one segment's `df` with another population's `N`. Pin analyzer name
and version in stored metadata and use the same analyzer at indexing/query
time. The field-specific population is consistent with
[Lucene CollectionStatistics](https://lucene.apache.org/core/10_3_1/core/org/apache/lucene/search/CollectionStatistics.html).

Acceptance: hand-calculated sparse-field examples, absent/empty text, repeated
terms, and a metamorphic check that splitting identical live documents into
different segments does not change ranks or scores beyond documented tolerance.
Exact live-document statistics with tombstones are a later store requirement.

## 3. Implement exact filtered vector search first

Apply the complete typed filter to obtain survivors, then score every surviving
vector with the existing full-precision metric. Define dimension, finite-value,
zero-norm and missing-vector behavior explicitly. Return `min(k, eligible_count)`
hits with stable external-ID ties. This is the reference for future ANN recall;
it is exhaustive over stored vectors, not a claim that embeddings capture
semantic relevance perfectly.

The [2025 unified FANNS study](https://arxiv.org/html/2509.07789v1)
finds different winners across label predicates and notes that pre-filtered
brute force can work well when few points survive. Its study excludes range
filters and curates queries with sufficient ground truth: NexusSearch must
add empty and fewer-than-k cases, numerical ranges, and missing values.
Acceptance should compare complete survivor IDs and sorted distances against
an independent array scan, including negative query/filter correlation.

## 4. Measure the ANN crossover instead of copying thresholds

Record eligible count, dimensions, scored vectors, elapsed time, algorithm and
exact/approximate status in explanations. Candidate bytes and measured work
are more useful inputs than selectivity alone. Qdrant's documented
[`full_scan_threshold`](https://qdrant.tech/documentation/manage-data/indexing/)
is a vector-size threshold in kilobytes, not a universal row count.

[RACORN-1, July 2026](https://arxiv.org/html/2607.00768v1), combines graph
bridges with an exact fallback; its limitations explicitly leave automatic
bridge/fallback calibration open. It is a promising preprint to reproduce,
not a ready-made guarantee for this engine. [SIEVE, PVLDB 2025](https://arxiv.org/html/2507.11907v2)
uses a workload-aware collection of indexes and a cost/recall model. Preserve
room for multiple operators, but defer these structures until local workload
receipts show a benefit at matched recall and bounded memory/build cost.
Published speedups have not been reproduced on this Windows ARM64 host.

## 5. Keep hybrid fusion deterministic and explainable

Use RRF as an initial no-training baseline: sum `1 / (c + rank)` across input
lists, ranks starting at one, with configurable `c` (default 60). Require unique
document IDs within each list; missing documents contribute zero. Persist the
per-retriever candidate window as part of the request, and break equal final
scores by bytewise external `_id`. Fixed summation order avoids scheduling
changing floating-point results. The formula and conventional constant come
from the [original SIGIR 2009 paper](https://cormack.uwaterloo.ca/cormacksigir09-rrf.pdf).

Run all retrievers against one snapshot and the same hard filter. Document that
changing a candidate window can change order; this is also explicit in
[Elasticsearch's RRF documentation](https://www.elastic.co/docs/reference/elasticsearch/rest-apis/reciprocal-rank-fusion).
Keep lexical scores, vector distances and rank contributions in explanations.
RRF is neither a relevance probability nor a guarantee of better results.
Acceptance: manually computed lists, duplicates, disjoint lists, empty lists,
ties, and stable repeated execution. Learned or calibrated fusion needs judged
queries and a held-out relevance evaluation first.

## 6. Validate an immutable segment before publishing any view

Use explicit little-endian fields and offsets, checked arithmetic, a versioned
header, a bounded section directory, and CRC32C checks covering metadata and
payloads. Reject overlap, truncation, duplicate section identities, unknown
required sections, bad lengths and invalid flags. Only expose borrowed views
after all required validation succeeds; heap and mapped loading share this
path. Checksums detect accidental corruption, not deliberate forgery.

[Lucene CodecUtil](https://lucene.apache.org/core/10_3_1/core/org/apache/lucene/codecs/CodecUtil.html)
distinguishes header/version/identity validation, retrieving a checksum, and
actually verifying the file. That distinction matters here: reading a stored
CRC is not integrity validation. The exact Nexus layout remains our own design.
Acceptance: every truncation boundary, representative bit flips, overlapping
sections, incorrect counts, integer-overflow offsets and reordered directories.
An unsupported format must produce an explicit error, never a partial index.

## 7. Separate a valid file from a crash-durable store

Atomic replacement is useful for individual immutable outputs. A transaction
across segments, manifests, tombstones and WAL additionally needs a documented
publication/recovery protocol and fault injection. SQLite's
[atomic commit account](https://www.sqlite.org/atomiccommit.html), especially its
flush ordering and power-loss discussion, illustrates why this is separate work.
Do not advertise transactional durability merely because save/reopen passes.
Defer multi-file commits until tests interrupt every publication phase and
reopen to either the complete previous generation or the complete new one.

## 8. Make correctness evidence a product feature

Keep the naive evaluator independent of postings/bitmaps while sharing the
bound query contract. Compare IDs, ordering and numerical scores, not only hit
counts. Test allocation failure, malformed stored files and query limits along
with happy paths. Seed and record randomized tests. Separate correctness runs
from benchmarks, and save compiler, CPU/SIMD mode, dataset hash, seed, filter
distribution and recall target with timing results. This extends the
[2025 benchmark's](https://arxiv.org/html/2509.07789v1) emphasis on comparable
filtering and tuning into a practical local evidence policy.

The next useful deliverable is schema/document ingestion, immutable persistence,
typed filtering, BM25, exact vectors, deterministic ranking and a small usable
CLI with truthful capability errors. Defer ANN, WAND pruning, quantization-based
selection, automatic index selection, learned rankers, concurrent WAL/store
operations and network services until their exact baselines and lifecycle
tests exist. A newer paper alone is not a reason to add implementation risk.


## Integration follow-up: 2026-10-04

The bounded immutable snapshot baseline above is now implemented. The local
HTTP/browser and Python adapters have targeted integration checks; this does
not promote experimental ANN or multi-file storage to the active query path.
The MCP adapter uses the [JSON-RPC 2.0 specification](https://www.jsonrpc.org/specification)
for response identity, notifications and error envelopes, and the
[MCP stdio transport specification](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)
for newline-delimited messages. Its advertised protocol remains the narrower
2024-11-05 subset; no full current-client compatibility claim is made.
The paired numeric-filter benchmark records an independent generated-data
oracle and raw alternating indexed/scan timings in
[the benchmark receipt](benchmarks/2026-10-04-snapshot-search.json).
