# 01 - Filtered / hybrid approximate nearest neighbour search (research note for NexusSearch)

Status: research spec, written 2026-10-02. Source tags used throughout:
- **[F]** = I fetched and read the primary source (or a tool summary of it) in this session.
- **[S]** = only seen in a search-result snippet/title; not read.
- **[R]** = recall-only (from memory; could not fetch). Treat numbers tagged [R] as unverified.
- **[D]** = my own derivation / proposal, not from any source. Calibrate before trusting.

Scope reminder: NexusSearch evaluates every structured clause to a Roaring bitmap and then combines
bitmaps with set algebra. The vector clause (`semantic:"..."`) must therefore run **filtered by an
already-computed bitmap `B`** whose **exact cardinality `c = |B|` is known for free**. That single fact
(exact selectivity, unlike Qdrant/ACORN which must *estimate* it) drives most of the design below.

---------------------------------------------------------------------------------------------------

## 1. Landscape

### 1.1 Three families (taxonomy used by the 2025 benchmark paper [F])
The unified FANNS benchmark (arXiv 2509.07789, Sept 2025) classifies methods as:
1. **filter-then-search** (pre-filter): shrink the candidate set first, then ANN or brute force. Brute force
   over survivors, ACORN variants and UNG are placed here.
2. **search-then-filter** (post-filter): unconstrained ANN, then drop non-matching. HNSW post-filter, IVFPQ post-filter.
3. **hybrid-search / in-filter**: filter evaluated *during* traversal; Filtered-DiskANN, Stitched-DiskANN, NHQ, CAPS.
The benchmark covers 10 algorithms, label predicates (containment / equality / overlap), tunes 41,000+ parameter combinations on subsets,
and reports **no numeric selectivity crossover** (only qualitative: at very low selectivity filter-then-search,
including plain pre-filter brute force, keeps candidates small; UNG best for containment/equality; DiskANN variants and ACORN
for overlap). It explicitly excludes range filters. => crossovers must be measured by us (section 6).

### 1.2 Who does what (engine survey)
| System | Mechanism | Thresholds / defaults | Tag |
|---|---|---|---|
| **pgvector** | post-filter on approximate index; "iterative index scan" refills; partial indexes / partitions as the manual escape | `hnsw.ef_search`=40; filter matching 10% of rows => ~4 results on average; `hnsw.iterative_scan` = `strict_order`/`relaxed_order`; `hnsw.max_scan_tuples`=20000; `hnsw.scan_mem_multiplier`=1; `ivfflat.probes`=1, `ivfflat.max_probes` | [F] https://github.com/pgvector/pgvector |
| **Qdrant** | filterable HNSW (extra graph edges per indexed payload value) + **query planner** using payload-index cardinality estimate choosing among: payload-index scan (very selective filter), full scan, HNSW-with-filter | `m`=16, `ef_construct`=100, `full_scan_threshold`=10000 **KB** ("size below which full scan preferred over HNSW"); payload index should be created before ingest | [F] https://qdrant.tech/documentation/concepts/indexing/ ; the filtrable-HNSW article fetch failed => exact edge-building rules [R] |
| **Weaviate** | two strategies: *sweeping* (traverse unfiltered, check filter before adding to results) and *ACORN* (two-hop expansion on filtered-out neighbours, applied conditionally); `flatSearchCutoff` switches to brute-force over the allow-list | sweeping better at 50% selectivity; ACORN ~2x throughput at 20%; ~10x at very low query-filter correlation; since v1.27 ACORN auto-falls back to sweeping when the filter is non-restrictive; `flatSearchCutoff` default 40,000 objects [R] | [F] https://weaviate.io/blog/speed-up-filtered-vector-search |
| **Lucene / Elasticsearch** | allow-list (bitset) filtered HNSW; ES 9.1 / Lucene PR #14160 adds an "ACORN-esque" heuristic; brute-force (exact) fallback when the filter is tiny or traversal exceeds a visit limit [R] | ACORN-style only engaged when filter matches < 60% of vectors (default threshold); two-hop expansion only if < 90% of the current neighbourhood matches; extended expansion capped by a formula `neighborCount / (1 - neighborFilterRatio)` [S]; reported ~5x speedup | [F] PR page https://github.com/apache/lucene/pull/14160 ; [S] https://www.elastic.co/search-labs/blog/elasticsearch-9-1-bbq-acorn-vector-search |
| **Vespa** | pre-filter evaluated to bit-vector; planner picks exact vs approximate vs filter-first by estimated "hit ratio" | `approximate-threshold` 0.05 (exact search if hit ratio below it), `post-filter-threshold` 1.0, `filter-first-threshold` 0.0, `filter-first-exploration` 0.3, `target-hits-max-adjustment-factor` 20 | [R] medium confidence |
| **Milvus (Knowhere)** | bitset allow-list; brute-force when almost everything is filtered out | HNSW brute-force trigger roughly when >93% filtered out (s < 7%) or k/c large (0.5) | [R] low-medium confidence |
| **LanceDB** | pre-filter by default using scalar indexes (bitmap/BTree), then vector search over the allow-list | n/a | [R] |
| **turbopuffer** | centroid (SPFresh-like) index with attribute-aware filtering; object-store native | n/a | [R] |

Convergent industry lesson: **small absolute survivor count => exact scan; large => graph with the filter inside the traversal;
post-filter only when the filter is nearly non-restrictive**. Qdrant and Weaviate use *absolute* cutoffs (bytes / object count), not percentages. Percentages
(Lucene 60%, Weaviate 50%, Vespa 5%) only govern which *graph* mode to use.

### 1.3 Research lines (2023-2026)
- **ACORN** (Patel, Kraft, Guestrin, Zaharia; SIGMOD 2024, arXiv Mar 2024) - predicate-agnostic filtered HNSW. [F] https://arxiv.org/abs/2403.04871
- **Filtered-DiskANN** (Gollapudi et al., WWW 2023) - FilteredVamana / StitchedVamana, label-aware pruning, per-label start nodes; equality/label predicates only. Details beyond what ACORN's paper reports about it [R]. Listing: [S] https://www.researchgate.net/publication/370413370
- **NHQ** (Wang et al., 2022/23) - "fusion distance" mixing vector and attribute distance; equality-only, one attribute per entity (as characterised by ACORN section 8) [F via ACORN]; formula [R].
- **iRangeGraph** (SIGMOD 2024) - range-filter ANN with a segment-tree-like family of graphs, "elastic" per-range subgraph assembly; built sequentially (single-threaded build per the 2025 benchmark listing [S]); details [R].
- **UNG** (Unified Navigating Graph; Label Navigating Graph over label-containment lattice; best for containment/equality [F via benchmark]); venue/year [R].
- **Curator** (multi-tenant vector index: clustering tree shared across tenants with per-tenant bitmaps, arXiv 2401.x) [R]; **RWalks** (random-walk "attribute diffusion" to make graphs attribute-aware) [R, low confidence about details - not fetched].
- **SIEVE** (Li, Huang, Ding, Park, Chen; PVLDB 18(11):4723-4736, 2025) - collection of indexes chosen by a 3-way cost model. [F] https://arxiv.org/abs/2507.11907
- **RACORN-1** (Kim, Choe; arXiv 2607.00768, July 2026) - fixes ACORN-1 recall collapse at very low selectivity. [F] https://arxiv.org/abs/2607.00768
- 2025-26 titles seen in search but **not read**: survey 2505.06501 (filtered ANN over vector-scalar hybrid data); 2508.16263 (attribute filtering in ANN); 2605.26474 (generalized range filtering: containment and overlap); 2606.00734 (EMA: general attribute filtering + dynamic updates); 2607.00727 (graph range filters); 2608.27663 (versioned unified graph index, timestamp-aware). All [S]. Worth a second research pass if range filters (`modified:>...`) become a hotspot.

---------------------------------------------------------------------------------------------------

## 2. Techniques

### 2.1 Baselines: pre-, post-, in-filter
- **Pre-filter + brute force**: cost `O(c)` distance computations, recall = 1.0. ACORN's pre-filter baseline is FAISS brute force over the
  survivor list. Competitive at low selectivity ("pre-filtering is most competitive for low selectivity predicates" [F]).
- **Post-filter**: ANN with `K/s` candidates then filter. ACORN's fair baseline over-searches `K/s` (earlier work used only K => unfairly bad) [F].
  Cost to reach recall 0.8 (distance computations, avg selectivity 0.083, SIFT1M / Paper): oracle-partition 398 / 281, **post-filter 1837.8 / 1425.5 (4.6x / 5.1x)** [F, Table 3].
  pgvector shows the failure mode: ef_search=40 and a 10% filter => ~4 hits [F].
- **In-filter**: filter inside traversal; naive "only score matching nodes" **disconnects** the graph at low selectivity
  (HNSW is sparse; matching nodes' induced subgraph is not navigable) [F: Lucene/Elastic and ACORN agree].
- **Oracle partition** (an HNSW per predicate value): the QPS gold standard (ACORN uses it as the ideal); infeasible for arbitrary predicates but
  *feasible for low-cardinality fields such as `type`* (see section 4).

### 2.2 ACORN (SIGMOD 2024) [F: abstract page + full PDF text]
Core idea: make **any predicate subgraph look like an HNSW built over the survivors**, without building one per predicate.

*ACORN-gamma (construction)*: parameters `M` (traversal degree bound), `efc`, `gamma` (neighbour-list expansion), `Mbeta` (compression).
1. During insertion collect `M*gamma` candidate edges (instead of `M`) using a metadata-agnostic search over the existing graph (which, when
   traversed, uses only the first `M` neighbours of each node).
2. **Predicate-agnostic compression, level 0 only**: keep the nearest `Mbeta` candidates verbatim. For the rest iterate in order with a set `H` of two-hop neighbours:
   drop candidate `c` if `c in H`, otherwise keep `c` and add `N(c)` to `H`; stop when `|H| + kept > M*gamma`. Dropped nodes remain reachable
   through the two-hop expansion done at search time.
3. Why HNSW's RNG pruning fails: edge v-b is pruned because a closer a exists; if `a` fails the predicate the path v->a->b does not exist in the predicate
   subgraph. Filtered-DiskANN fixes it with metadata-aware pruning (needs known predicates); ACORN avoids pruning by metadata and instead over-provisions edges.

*Search* (per visited node `c` at level `l`, then standard HNSW greedy/beam with `ef`):
```
GET_NEIGHBORS(c, l, pred):            // ACORN-gamma, compression-aware
    out = []
    for v in N_l(c)[0 : Mbeta]:        if pred(v): out.push(v)           // exact first Mbeta
    for v in N_l(c)[Mbeta : end]:      for w in N_l(v): if pred(w): out.push(w)   // 2-hop for pruned region
    return first M of out
GET_NEIGHBORS_ACORN1(c, l, pred):     // gamma=1, Mbeta=M, expansion at search time
    out = [w for v in N_l(c) for w in ({v} U N_l(v)) if pred(w)]; return first M of out
```
Predicate evaluated per neighbour; the beam, entry point at top level and heap logic are unchanged. Search is predicate-subgraph traversal; they note truncation to `M` keeps distance computations bounded.

*Configuration rule*: `gamma = 1 / s_min` where `s_min` = lowest selectivity you intend to serve with the graph; **below `s_min` pre-filter instead** ("if estimated selectivity > 1/gamma
search ACORN, else pre-filter") - a deliberately simple cost model. A wrong estimate costs efficiency, never recall (pre-filter is exact; ACORN on a >1/gamma predicate is just slower than it could be).

*Parameters actually used*: FAISS defaults `M=32, efc=40` (SIFT1M, Paper, LAION); TripClick tuned by grid to `M=128, efc=200`. `Mbeta` in {M, 2M}: 32 (LAION), 64 (SIFT1M, Paper), 128 (TripClick); performance "relatively insensitive" to `Mbeta`. `gamma` = 12 (SIFT1M/Paper; 12 equality classes), 30 (LAION), 80 (TripClick; 1/80 = 0.0125 ~ the 1st-percentile query selectivity 0.0127). Recall-QPS swept via `efs` 10..800.

*Reported gains*: 2-1,000x QPS at fixed recall vs prior methods (abstract); 2-10x vs Filtered-DiskANN/NHQ even on their home turf (equality, low-cardinality); 30-50x at recall 0.9 on TripClick and LAION-1M (high-cardinality, regex/contains/between predicates which the specialised indexes cannot express); >1000x at LAION-25M; robust to negative query-filter correlation (28-100x vs best baseline on LAION-1M). Distance computations to reach recall 0.8: oracle 398, **ACORN-gamma 611 (+54%)**, **ACORN-1 999.6 (+151%)**, post-filter 1837.8 (SIFT1M; similar on Paper).
*Costs*: ACORN-gamma time-to-index up to 11x HNSW (TripClick 9,902 s vs 891 s; LAION-25M 38,008 s vs 1,147 s); size up to 1.3x HNSW (TripClick 4.9 vs 4.1 GB); level >=1 lists reach `M*gamma` (TripClick: 10,240 entries per node!). **ACORN-1**: no build change (= HNSW, gamma=1, Mbeta=M), TTI 9-53x lower than ACORN-gamma, size <= 1.25x HNSW, QPS 1.5-5x lower than ACORN-gamma.
*Pitfalls*: (a) gamma is a global commitment made at build time; (b) two-hop expansion is a memory-latency cost - the paper notes the filter step over neighbour lists is relatively costlier on low-dimensional (SIFT 128-d) than on higher-dimensional data; (c) ACORN-1 degrades once `s * (M+1)` drops below ~1 (see 2.4 and 2.5).

### 2.3 Lucene / Elasticsearch ACORN-esque heuristic [F PR; S ES blog]
Only matching docs are scored/explored; two-hop expansion is applied **only when the immediate neighbourhood is mostly filtered**: expand if fewer than 90% of the
current neighbours match (i.e. more than 10% filtered); expansion bounded by roughly `neighborCount / (1 - neighborFilterRatio)` scored candidates; whole heuristic is
used only when the filter matches < 60% of vectors, else plain filtered HNSW. Benchmarks on an 8M-vector nightly dataset (PR text): at 5% selectivity **0.980 recall with 1,286 visited vectors versus baseline 0.924 recall with 53,003 visited**; at the "90%" setting recall fell to 0.904 (ambiguous in the summary which end this is; re-check in the PR). Reviewer caveat: "increasing k doesn't monotonically increase recall" in filtered mode - add a test for it. Elastic claims ~5x typical speedup [S]. Important design hint: **adaptive** expansion (only when the local neighbourhood fails the filter) avoids paying the `M^2` neighbour-list reads where unnecessary.

### 2.4 RACORN-1 (2026) [F abstract]
Extends ACORN-1 with (i) **Adaptive Search Fallback**: failed-filter nodes are reused as temporary bridges to route around broken connectivity; (ii) **Adaptive Exact Fallback**: switch to linear scan at ultra-low selectivity.
Numbers: 9-26x lower latency vs HNSW at 1%-0.3% selectivity; recall at 1% selectivity 0.45-0.72 -> 0.70-0.96; at 0.3% 0.03-0.10 -> 0.77-0.98; <=0.1% selectivity (1M vectors) 20-75x with perfect recall; 0.01% at 40M scale 13x with perfect recall; adversarial recall 0.80-0.98 vs ACORN-1's 0.08-0.41. Message: **ACORN-1 is not safe below ~1% selectivity; always have an exact fallback.** Our Roaring-based planner gets the "exact fallback" for free (section 3).

### 2.5 SIEVE (PVLDB 2025) [F abstract only]
Instead of changing traversal, build a **collection of specialised indexes** for predicate patterns, chosen by a 3-D analytic model (index size vs search time vs recall) that is workload-aware, plus a query-time router. Reported: up to 8.06x speedup vs baselines; build-time overhead as low as 1% vs other indexes; memory < 2.15x a plain HNSW. Take-away for us: **sub-indexes for hot predicates with a memory budget (<= ~2x base)** - section 4 stretch item.

### 2.6 Qdrant planner [F docs; mechanisms R]
Three strategies chosen from a payload-index cardinality estimate; defaults `full_scan_threshold`=10,000 KB [F]. [R] The planner takes cardinality *bounds* (min/max); if max estimated cardinality is below the threshold it iterates the payload-index id list and scores exactly; otherwise it traverses HNSW with the filter, whose graph has extra edges built per indexed payload value so each value's subgraph stays connected. Since the threshold is in **kilobytes of vector data**, it automatically scales with dimension/quantisation (10 MB of f32 d=768 = ~3,300 vectors; of f32 d=128 = ~20,000).

### 2.7 pgvector iterative scan [F]
Post-filter loop: scan index in batches until enough rows survive or `hnsw.max_scan_tuples` (20,000) is hit; `strict_order` keeps exact distance order, `relaxed_order` favours recall. Mitigations: B-tree index on the filter column for very selective filters, partial HNSW indexes per category, list partitioning. Take-away: "refill until k survivors or budget" is the minimum viable post-filter loop; we use it only in the SWEEP mode (section 3).

### 2.8 Filtered-DiskANN / NHQ / UNG / iRangeGraph / Curator / RWalks (what we need from them)
From ACORN's experimental section [F]: FilteredVamana used `L=90, R=96`; StitchedVamana `R_small=32, L_small=100, R_stitched=64, alpha=1.2`; both need equality (low-cardinality) label predicates; ACORN reports that they cannot run on high-cardinality / regex / range predicates. NHQ needs one equality attribute per entity. These indexes are *build-time-predicate-aware* and so are poor fits for a query language where predicates are arbitrary boolean algebra over typed fields. Per-label **start nodes** (medoids) are the idea worth keeping (entry points must lie inside the survivor set) - section 4.

---------------------------------------------------------------------------------------------------

## 3. Decision rule for Roaring-bitmap survivor sets (the planner)

Inputs: query vector `q`, `k`, base `ef`, segment-local `B` (already `AND`-ed with the segment's live/tombstone mask), `N` = live docs in segment, `M` of its graph.
Known exactly: `c = card(B)` (Roaring cardinality is O(#containers)), `s = c/N`. Cardinality of a *not-yet-materialised* AND/OR is available as `and_cardinality`/`or_cardinality` - use them to cost clause orders before materialising.

All numeric thresholds below are **[D] defaults justified by the cited data and meant to be re-calibrated by the micro-benchmark in 3.3**.

### 3.1 Cost model (units = one scored vector in the scan representation)
```
t_scan  : ns to score one survivor in a gather-scan over sorted ids (SQ8 codes or f32), measured
t_visit : ns per *scored* node during graph traversal incl. its neighbour-list reads and bit tests, measured  (rho = t_visit/t_scan, expect 2..5)
C_exact(c)    = c * t_scan  (+ rerank)                       // parallelisable across workers when c >= 65,536
V_est(ef, s)  = 8 * ef * g(s)      // prior; g = 1.0 (s>=0.5, SWEEP), 2.5 (ACORN-1 two-hop; ACORN paper Table 3: ACORN-1 = 2.0-2.5x oracle distance comps), +25% per halving of s below 3%
C_graph(ef,s) = V_est * t_visit
```
Data anchors: Lucene reaches recall 0.98 at 5% selectivity with 1,286 scored nodes [F]; ACORN-1 needs ~1,000 comps for recall 0.8 [F]. So graph cost is **~1-3k scored nodes, independent of N**, which implies a crossover at an *absolute* survivor count `c* ~ rho * V_est ~ 3-10k` (consistent with Qdrant's 10 MB cutoff and Weaviate's flat cutoff).

### 3.2 The rule (exact, pseudo-C)
```
plan = EMPTY | EXACT | EXACT_PAR | SWEEP | ACORN1 | ACORN1_BUDGETED
if c == 0                                  return EMPTY
if c <= max(k, 256)                        return EXACT          // trivially score everything
bytes = c * code_bytes_per_vec             // SQ8: d ; f32: 4d
if bytes <= T_SCAN (10 MiB)  or  c * t_scan <= C_graph(ef, s_eff)   return EXACT   // absolute crossover
s_loc = local_selectivity(q, B)            // 3.4, optional; else s_loc = s
s_eff = min(s, s_loc)                      // pessimistic: either view being bad => treat as low selectivity
if s_eff >= S_SWEEP (0.50)                 return SWEEP          // traverse unfiltered, collect only matches; ef unchanged
if s_eff >= S_LO = 1.5/(M+1)               return ACORN1         // 4.5% at M=32, 8.8% at M=16
// low selectivity but too many survivors for the tiny-scan path
if bytes <= T_BIG (256 MiB) / budget_ms*bw return EXACT_PAR      // ~ <=30 ms on 8 cores at ~10 GB/s
return ACORN1_BUDGETED                     // ef*=2, entry seeds from B, hard visit budget, exact fallback
```
Graph modes always run with a **visit budget** `V_max = c * t_scan / t_visit` (the number of graph visits whose cost equals an exact scan). If the budget is exhausted before the beam terminates,
abort and run EXACT over `B`. Worst-case total cost <= 2x the cheaper strategy (ski-rental argument [D]) and recall never falls below the exact scan's - this is the Roaring-specific
advantage: ACORN/Qdrant must trust an estimate; we know `c`.

*Why `S_LO = 1.5/(M+1)`* [D, consistent with sources]: the two-hop frontier of a node contains `~M*(M+1)` candidates; at selectivity `s` the expected number of passing candidates is `s*M*(M+1)`,
which reaches the truncation target `M` only when `s >= 1/(M+1)` (3.0% for M=32; 5.9% for M=16). ACORN's `gamma = 1/s_min` is the same relation with `M*gamma` stored edges. Consistency checks: TripClick 1st-percentile
selectivity 0.0127 worked for ACORN-1 at `M=128` (1/(M+1)=0.0078) [F]; RACORN-1 reports ACORN-1 recall 0.45-0.72 at 1% and 0.03-0.10 at 0.3% selectivity [F]. The 1.5x factor is a safety margin for neighbour-set correlation.
*Why `S_SWEEP=0.5`*: Weaviate measured sweeping better at 50%, ACORN better at 20% [F]; Lucene gates its ACORN heuristic at 60% [F].

### 3.3 Calibration (do at index open, 10-50 ms, cache in segment meta)
- `bw`: streaming read GB/s of a 64 MB buffer on N threads; `t_scan` per vector in the real kernel on 1,000 random ids sorted ascending.
- `t_visit`: run 32 unfiltered queries with the real HNSW code, divide time by scored nodes.
- Persist `{t_scan, t_visit, bw}` and EMA-update from EXPLAIN ANALYZE (actual visited count vs `V_est`) per `(selectivity bucket, mode)`; bucket = floor(log2(s)).

### 3.4 Local (query-conditional) selectivity - Roaring-native correlation estimate [D]
ACORN and Weaviate both show that **query-filter correlation** changes cost by 10-100x at identical global selectivity (LAION pos/none/neg-correlation workloads [F]). Keep a coarse quantiser
(e.g. 256-1024 k-means centroids over a sample; ~N/1000 docs per cell) and a **Roaring bitmap per cell**. At plan time compute `|B ∩ cell_i|` via `and_cardinality` for the `n_probe=8` cells nearest `q`; then
`s_loc = sum |B ∩ cell_i| / sum |cell_i|`. Cost: 8 small AND-cardinalities (~tens of microseconds). Use `s_eff = min(s, s_loc)`; if `s_loc < s/4` flag "negative correlation" (EXPLAIN shows it) and double `ef` or prefer exact scan up to `3*C_FLAT`.
The same per-cell intersections give **survivor-seeded entry points** (section 4).

### 3.5 ef and k handling
- SWEEP: result list `W` counts only matching nodes, so `ef` is unchanged; candidate list traverses everything; worst case visits ~ `ef/s <= 2*ef`.
- ACORN1: predicate subgraph behaves like an HNSW over `c` nodes, so **do not scale ef by 1/s** (that is the post-filter mistake); use `ef = max(ef_base, 2k)`; x1.5 when `s_eff < 2*S_LO`.
- Never request `k_vec < k_final * oversample` when the vector list feeds hybrid fusion: ask each lexical/vector leg for `max(3k, 100)`.
- Per segment: independent plan per segment (`c_seg` differs wildly after merges/tombstones), then merge top-k; pass the running global k-th best distance as the initial pruning bound to later segments (Lucene shares a minimum competitive score across segments [R]).

---------------------------------------------------------------------------------------------------

## 4. Adopt for NexusSearch (ordered)

Machine reality [D]: <= ~10M vectors in 15 GB RAM means SQ8 codes (384 B at d=384) for the traversal and f32 for rerank. At `s < 3%` the survivor set is `<= 300k` vectors =
`<= ~115 MB` of SQ8 codes => ~10-30 ms across 8 threads at 10-25 GB/s. **Therefore ACORN-gamma is not needed at local scale**: below the ACORN-1 floor the right answer is a parallel exact scan.

**P0 (core) - exact survivor scan kernel** `vec_scan_bitmap(seg, B, q, k, out)`:
- iterate `B` in ascending order via batched container walk (array/bitmap/run containers; run containers => contiguous slab, no gather);
- vectors stored in segment-local docid order, 64-byte aligned rows, stride padded to 32 B (SQ8) / 8 floats (f32); prefetch 8 rows ahead;
- AVX2 `_mm256_fmadd_ps` L2/IP for f32, `_mm256_maddubs_epi16`/`_mm256_madd_epi16` for SQ8; 4 queries-by-1 doc not needed (one query);
- keep top `k*R` by code distance (rerank factor R=4..10, default 5) then rescore with f32; bounded heap;
- parallel split by container ranges when `c >= 65,536`, merge heaps.
- Acceptance: byte-identical to a naive f32 brute force oracle when R covers the oracle's top-k (recall 1.0 with rerank).

**P0 - planner** exactly as 3.2 with calibration 3.3; emit the plan, `c`, `s`, `s_loc`, `V_est`, `V_budget` in EXPLAIN; emit `V_act`, distance evals, fallback-taken, wall time in EXPLAIN ANALYZE.

**P1 (core) - HNSW with bitmap hook**: per-segment immutable HNSW; **dense bitset snapshot of B** (`N/64` uint64 words; for N=10M = 1.25 MB, filled in <1 ms from Roaring; keep Roaring for algebra, use the dense bitset inside the traversal - never call Roaring `contains` in the inner loop). Params: `M=32` (level 0 degree `2M` stored, traversal truncated to first `M`), `efc=200`, `ef` query default `max(64, 2k)`, `mL=1/ln(M)`. Visited set = per-thread epoch-stamped `uint16` array. Modes:
```
search0(q, Bd, seeds, ef, M, mode, Vmax):
  C = min-heap(cand); W = max-heap(ef) of matching nodes only; scored = 0
  for s in seeds: C.push(s, d(s)); if Bd[s]: W.push(s, d(s)); scored++
  while C not empty:
     c = C.pop_min()
     if |W|>=ef and d(c) > W.worst(): break
     if mode == SWEEP: nbrs = unseen N0(c)[0:M]   // score all, collect only matching into W; all into C
     else:  // ACORN1 adaptive (Lucene-style trigger)
        nbrs = [v in N0(c)[0:M] if Bd[v] and unseen(v)]
        if |nbrs| < 0.9*|N0(c)[0:M]|:                     // >10% of 1-hop failed the filter
            for v in N0(c)[0:M] where !Bd[v] and !expanded(v):  expanded(v)=1
                prefetch N0(v)
                for w in N0(v)[0:M]: if Bd[w] and unseen(w): nbrs.push(w); if |nbrs| >= M_target: break
     score nbrs in a SIMD batch; scored += |nbrs|
     if scored > Vmax: return FALLBACK_EXACT
     for v in nbrs: if |W|<ef or d(v)<W.worst(): C.push(v); if Bd[v]: W.push(v); trim W to ef
  return W
```
Note: with ACORN1, `nbrs` contains only matching nodes, so `W.push` is unconditional; `M_target = M` (ACORN's truncation); optionally test Lucene's `neighborCount/(1-ratio)` cap as an A/B.
Level descent: greedy unfiltered descent to level 1 (non-matching entry points still seed routing), then level 0 as above. Seeds: best `min(ef,16)` visited level-1 nodes + up to 8 survivor seeds (section 4 P2).

**P1 - integration**: tombstones are part of `B` (live mask `AND`) and treated as non-matching routing nodes; segment merge when tombstone ratio > 25% [D] (dead hubs hurt two-hop yield). Planner runs per segment. Filtered result with `semantic:` under `NOT` is rejected (not boolean); `semantic:` under `OR` becomes "union of boolean-set and top-K_sem set" with `K_sem = max(3k,100)`.

**P1 - partition by (type, embedding space)**: one graph per `type` x embedding model/dimension. Required anyway (different models have different dims and metrics) and it is the cheap form of ACORN's *oracle partition* for the most common predicate (`type:`). A `type:` filter then costs zero.

**P2 (important)** - local-selectivity module (3.4), survivor-seeded entry points (nearest cells with survivors; choose the survivor in the best cells closest to q by centroid distance), **shadow recall monitor** (1 in 200 queries also runs EXACT asynchronously on the same `B`, logs recall@k per mode/bucket; if rolling recall < target 0.95 raise `ef` multiplicatively and, if still failing, shrink `S_LO` boundary => more exact), and iterative "refill" for SWEEP (continue with `ef += ef/2` until `|W|>=k` or budget).

**P3 (stretch)**:
- *SIEVE-lite hot-predicate sub-index*: key = canonical hash of the structured clause; when a key repeats `>= 20` times, has `C_graph(ef,s) < C_exact(c)` failing (i.e. ACORN1_BUDGETED/EXACT_PAR path) *and* `c in [20k, 0.3 N]`, build a dedicated HNSW over `B` (cost `O(c log c)` with `M=16`), cache in an LRU with a global budget of <= 1x base-graph memory (SIEVE reports < 2.15x total [F]); invalidate on segment generation change.
- *ACORN-gamma-lite* only if a deployment needs `N >= 50M` with `s in [0.1%, 3%]` (build 9-11x slower [F]).
- *RACORN-1 ASF* ("failed nodes as bridges") - only if the shadow monitor shows ACORN1 recall < 0.9 at `s >= S_LO`.
- *Docid-sorted segments by a primary range attribute* (`modified`): a range predicate becomes one Roaring **run container** (contiguous docid interval) => exact scan streams linearly. See section 6.

Default-parameter summary:
| Param | Default | Source |
|---|---|---|
| M / efc / ef | 32 / 200 / max(64,2k) | ACORN FAISS defaults M=32 [F]; efc raised (ACORN tuned efc=200, M=128 for hard data) |
| T_SCAN | 10 MiB of scanned codes | Qdrant `full_scan_threshold` 10,000 KB [F] |
| c_min exact | max(k,256) | [D] |
| S_SWEEP | 0.50 | Weaviate 50% [F], Lucene 60% [F] |
| S_LO | 1.5/(M+1) | ACORN gamma=1/s_min [F], RACORN-1 [F] |
| two-hop trigger | < 90% of 1-hop neighbours match | Lucene [F] |
| M_target (expansion cap) | M | ACORN-1 [F] |
| T_BIG | 256 MiB / latency 30 ms | [D] |
| rerank R | 5 | [D] |
| tombstone merge | > 25% | [D] |
| shadow-recall sample | 1/200 | [D] |

---------------------------------------------------------------------------------------------------

## 5. Skip and why
- **Filtered-DiskANN / StitchedVamana / NHQ / CAPS**: equality/label predicates only, build-time predicate knowledge, filter cardinality constraints, one attribute per entity (NHQ); ACORN reports 2-10x higher QPS than them on *their* workloads and they cannot express regex/range/AND-OR trees [F]. Disk-oriented layout irrelevant to an mmap-in-RAM engine.
- **ACORN-gamma full build** (TTI up to 11x, M*gamma edge lists up to 10,240/node on TripClick [F]) - exact scan covers its target regime at local scale (section 4 note). Keep as P3.
- **UNG / label-lattice indexes**: only containment/equality label sets; complexity vs query language that is arbitrary boolean algebra.
- **pgvector-style partial indexes / table partitioning per predicate value**: a manual DBA workaround [F]; we get the useful part via `type` partitioning + hot-predicate cache.
- **Pure post-filter**: recall collapses (pgvector 10% => ~4 results at ef=40 [F]; ACORN shows post-filter needs `K/s` over-search and still misses recall 0.9 on low-correlation data [F]).
- **iRangeGraph and the 2025-26 range-graph papers**: valuable for numeric/date ranges at scale but [R]/[S] only; for local scale a docid-sorted segment + run containers gives similar benefit at ~zero index cost (unverified).
- **Curator / RWalks**: multi-tenant ACL scenario and an unread random-walk approach; revisit if per-user ACL bitmaps become first-class.

## 6. Validation and test ideas
Datasets (no downloads): synthetic Gaussian mixtures, `N in {50k, 200k}`, `d in {64, 128, 384}`, 64 clusters; attributes: `cluster_id` (positive correlation filter), random int (no correlation), `far_cluster_id` (negative correlation), and a `ts` range attribute.
1. **Zero false positives**: every returned id must satisfy `B` (assert in all modes, 100% of queries, incl. tombstoned ids).
2. **Oracle recall grid**: `s in {1e-4, 1e-3, 1e-2, 3e-2, 0.1, 0.3, 0.6, 0.9}` x correlation `{pos, none, neg}` x `k in {1,10,100}`; recall@k vs exact. Floors: EXACT = 1.0 exactly; SWEEP/ACORN1 >= 0.95 at default ef for `s >= S_LO`; planner total recall >= 0.97 everywhere on the grid.
3. **Planner regret**: for each grid point run all strategies, record latency; the planner's pick must be within 1.5x of the best strategy for >= 90% of points and within 3x for all. Plot crossover `c*` vs `N`; verify it is ~N-independent (the key claim of 3.1).
4. **Fallback invariants**: force `Vmax=1` => result must equal EXACT output. Disconnected-subgraph adversary: `B` = a far cluster only (RACORN-1's adversarial case: ACORN-1 recall 0.08-0.41 [F]); planner must return recall >= 0.95.
5. **Non-monotone k** (Lucene reviewer caveat): recall@k must not drop by > 0.02 when k doubles at fixed ef.
6. **Roaring equivalence**: random mixes of array/bitmap/run containers; dense-bitset snapshot equals Roaring membership for all ids; `and_cardinality == |and|`; cardinality after tombstone `ANDNOT` is exact.
7. **Local-selectivity estimator**: on pos/neg workloads compare `s_loc` to true selectivity among the 1,000 true nearest neighbours of q (should correlate; negative workloads must trigger the flag).
8. **Segment/tombstone churn**: insert/delete 30% in rounds; recall and p99 latency per round; confirm merge trigger restores recall.
9. **Calibration drift test**: perturb `t_visit` x3 and check the planner still meets the regret bound after EMA updates in 200 queries.
10. **EXPLAIN contract**: golden-file test for plan fields; `V_act <= V_max`; `fallback=true` implies exact result.
11. **Hybrid**: lexical (BM25) and vector legs under the same `B`; RRF result must be a subset of `B`.

## 7. Novel ideas specific to NexusSearch [D unless noted]
1. **Exact-cardinality competitive planner** - graph search with `V_max` equal to the exact-scan cost, then switch: bounded 2x regret, recall-safe, no cardinality estimation error (unlike ACORN's s_min rule, Qdrant bounds, Vespa hit-ratio).
2. **Roaring-native correlation estimate**: per-centroid-cell bitmaps and `and_cardinality` give a query-conditional selectivity in tens of microseconds; no engine in section 1.2 does this (as far as I could verify).
3. **Survivor-seeded entry points** from the same cell bitmaps (Filtered-DiskANN-style per-label start nodes, but for arbitrary predicates).
4. **Type x embedding-space partitioned graphs** as the default (oracle partition for the most common predicate) + **hot-predicate sub-index cache** (SIEVE-lite) driven by EXPLAIN ANALYZE statistics = the "adaptive indexing" requirement.
5. **Docid-sorted segments on a primary range attribute** so range predicates become single run containers; exact scan degenerates to a linear stream (no gather).
6. **Lexical-first rerank path**: if a BM25/phrase/structured clause makes `c <= C_FLAT`, skip the vector index entirely and score vectors only for those survivors (natural hybrid; planner emits `EXACT` with source `lexical`).
7. **Shadow exact-recall monitor** on 0.5% of live queries giving continuously measured per-mode recall that auto-tunes `ef` and `S_LO` (a self-tuning feedback loop uncommon in engines).
8. **WATCH/percolator semantics**: for live subscriptions with `semantic:` + filters, test the new document against each subscription's *filter bitmap membership* first (cheap), then compute similarity only for passing subscriptions; no ANN needed because the document is the query side.
9. **Per-segment plans with shared top-k bound** across segments (cheap early termination).

## 8. References
Fetched / read in this session [F]
- ACORN (Patel, Kraft, Guestrin, Zaharia, 2024): https://arxiv.org/abs/2403.04871 (PDF: https://arxiv.org/pdf/2403.04871; full text read: algorithms 2, gamma/Mbeta, Tables 3-6, setup)
- SIEVE (Li, Huang, Ding, Park, Chen, PVLDB 18(11) 2025): https://arxiv.org/abs/2507.11907
- RACORN-1 (Kim, Choe, July 2026): https://arxiv.org/abs/2607.00768
- Filtered ANN benchmark / experimental study (Sept 2025): https://arxiv.org/abs/2509.07789 (read HTML v1: https://arxiv.org/html/2509.07789v1)
- Lucene PR "Add new Acorn-esque filtered HNSW search heuristic" #14160: https://github.com/apache/lucene/pull/14160
- Weaviate, filtered search speed-up / ACORN: https://weaviate.io/blog/speed-up-filtered-vector-search
- pgvector README: https://github.com/pgvector/pgvector
- Qdrant indexing docs: https://qdrant.tech/documentation/concepts/indexing/

Seen in search results only [S] (not read)
- Elasticsearch 9.1 BBQ + ACORN blog: https://www.elastic.co/search-labs/blog/elasticsearch-9-1-bbq-acorn-vector-search
- Elastic "Filtered HNSW & kNN search in Lucene and Elasticsearch": https://www.elastic.co/search-labs/blog/filtered-hnsw-knn-search
- Survey of filtered ANN over vector-scalar hybrid data: https://arxiv.org/abs/2505.06501
- Attribute filtering in ANN search: https://arxiv.org/abs/2508.16263
- Generalized range filtering ANN (containment and overlap): https://arxiv.org/abs/2605.26474
- EMA (general attribute filtering + dynamic updates): https://arxiv.org/abs/2606.00734
- ANN with graph range filters: https://arxiv.org/abs/2607.00727
- Versioned unified graph index (timestamp-aware): https://arxiv.org/abs/2608.27663
- Making HNSW work with WHERE clauses (DuckDB, ACORN): https://cigrainger.com/blog/duckdb-hnsw-acorn/
- Filtered-DiskANN listing: https://www.researchgate.net/publication/370413370_Filtered-DiskANN_Graph_Algorithms_for_Approximate_Nearest_Neighbor_Search_with_Filters

Recall-only [R] (no URL fetched; verify before relying): Vespa `approximate-threshold` / `filter-first-*` defaults; Milvus Knowhere HNSW brute-force thresholds; Weaviate `flatSearchCutoff` default 40,000; Qdrant `payload_m` and cardinality-bounds planner details (the filtrable-HNSW article fetch failed); Filtered-DiskANN pruning rule and per-label medoid starts; NHQ fusion-distance formula; iRangeGraph structure; UNG venue; Curator; RWalks; Lucene cross-segment shared competitive score; Lucene exact-fallback visit limit.
