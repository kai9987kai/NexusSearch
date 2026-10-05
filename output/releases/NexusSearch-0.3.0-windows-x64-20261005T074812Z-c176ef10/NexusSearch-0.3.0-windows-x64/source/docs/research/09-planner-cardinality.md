# 09 - Cost-based planning and cardinality estimation (NexusSearch planner spec)

Research date: 2026-10-01/02. Scope: how an engine whose clauses are *exact Roaring bitmaps* should order,
combine and cost them; cardinality estimation for the clauses that are not bitmaps (ranges, text, regex, vectors);
a concrete per-operator cost model; EXPLAIN / EXPLAIN ANALYZE; calibration; caching.

Evidence legend (same as notes 01/05; obey it when implementing):
- [F] fetched and read this session (docs / source / abstract). Numbers quoted are from the source.
- [V] verified locally this session by running code (`scratchpad/calib2.c`, GCC 15.2 -O2 -mavx2 -mpopcnt, Windows 11, 8 cores,
  **machine was shared with other jobs, so every [V] timing is a pessimistic upper bound; min of 7 runs**).
- [S] seen only as a web-search result title/snippet, page NOT read. A lead, not a fact.
- [R] recall-only, not fetched, URL unverified. A lead, not a spec.
- [E] my own derivation / design proposal (derivation shown).

Search budget used: 13 fetch/search calls. Not fetched (so [R] only): Vespa threshold defaults (note 01 has them as [R]),
DuckDB per-operator field names (docs page fetched but only format/pragma names were returned), SQLite, Tantivy, Elasticsearch internals.

---------------------------------------------------------------------------------------------------

## 1. Landscape

| System | What it does for conjunction ordering / estimation | Source |
|---|---|---|
| **Lucene** (also ES, OpenSearch, Solr) | `ConjunctionDISI`: iterators sorted by `cost()` ascending, two leaders special-cased, leapfrog `advance`; two-phase iterators (cheap *approximation* + costly `matches()`) verified in ascending `matchCost()`; bitset-backed clauses with `cost() > minCost` merged into a bulk `BitSetConjunctionDISI` (`applyMask` when `lead.cost() < bitSet.length()`). `IndexOrDocValuesQuery` picks index structure (good *lead*, almost fully consumed) vs doc values (cheap init, good *verifier* when another clause leads). Filter-cache policy `UsageTrackingQueryCachingPolicy`. | [F] URLs in section 8 |
| **Qdrant** | Each filter condition returns `CardinalityEstimation{min, exp, max, primary_clauses}`; AND/OR/NOT combine them with closed formulas (below). `primary_clauses` = the lowest-cardinality indexed condition = the lead. Planner then picks payload-index scan vs filtered HNSW vs sampling (details note 01 s2.6). | [F] query_estimator.rs |
| **Vespa** | Evaluates the filter to a bit-vector, estimates hit ratio, selects exact / post-filter / pre-filter(filter-first) ANN via rank-profile `approximate-threshold` and `post-filter-threshold`; `geoLocation`/`predicate` operators always act as post-filters. Numeric defaults are only [R] (note 01). | [F] docs page (names only) |
| **PostgreSQL** | Cost = pages x `seq_page_cost` + rows x `cpu_tuple_cost` (+ operator terms); stats: per-column MCV + equi-depth histogram up to `default_statistics_target`=100 entries; independence assumption between columns unless `CREATE STATISTICS` (dependencies / ndistinct / mcv). `EXPLAIN` prints `cost=startup..total rows= width=`; `ANALYZE` adds `actual time=first..total rows= loops=`, Buffers, `Rows Removed by Filter`. | [F] docs |
| **DuckDB** | `EXPLAIN` / `EXPLAIN ANALYZE`, formats text/json/html/graphviz/mermaid (`EXPLAIN (FORMAT json) ...`), profiling pragmas `enable_profiling`, `profiling_mode`, `profiling_output`, `configure_profiling`. HLL distinct counts, DPhyp join order [R]. | [F] partially |
| **SQLite** NGQP, **Tantivy** (`size_hint`-ordered leapfrog intersection), **Elasticsearch** (profile API, query cache) | Same family: cost/`size_hint`-ordered leapfrog + optional filter cache. | [R] |
| **Research 2025-26 on filtered ANN planning** | pgvector's cost-based planner reportedly picks approximate index scans where an exact scan has equal latency [S arXiv 2602.11443]; learning-based pre/post-filter selection [S arXiv 2602.17914]; vecadvisor cost-based advisor for pgvector [S github.com/vecadvisor/vecadvisor]; plan regret concentrates at strategy phase boundaries [F abstract arXiv 2606.16341]. | see s2.7 |

Key structural conclusion: Lucene/Tantivy are *iterator* engines (cost() is an estimate, plans are shaped by leapfrog).
NexusSearch is a *bitmap* engine: leaf cardinalities are exact and free (`sum(card_m1+1)` over frozen NXR headers, note 05), pairwise
`and_cardinality` is O(#containers), and every operator boundary yields an exact cardinality. So the planner should be a
**greedy, exact-feedback, runtime-adaptive pipeline** (section 3.1), not a Cascades/DP optimizer, and estimation effort should go only to
the clauses that are *not* bitmaps.

---------------------------------------------------------------------------------------------------

## 2. Techniques

### 2.1 Cost-ordered leapfrog + two-phase verification (Lucene ConjunctionDISI) [F]
Source: https://raw.githubusercontent.com/apache/lucene/main/lucene/core/src/java/org/apache/lucene/search/ConjunctionDISI.java (read 2026-10).
```
create(iters, twoPhase):
  split bitset-backed iters with cost > minCost -> B (merged later);  sort the rest ascending by cost()
  disi = (|rest|==1) ? rest[0] : ConjunctionDISI(rest)          // lead1 = cheapest, lead2 = next, others[]
  if B: disi = BitSetConjunctionDISI(disi, B)                    // sparse lead iterated, dense bitsets probed (applyMask if lead.cost < bitset.length)
  if twoPhase: disi = TwoPhase(disi, verifiers sorted ascending matchCost())   // fail fast on the cheap verifier
doNext(doc): loop { d2 = lead2.advance(doc); if d2!=doc { doc = lead1.advance(d2); continue }
                    for o in others: if o.doc<doc { n=o.advance(doc); if n>doc { doc = lead1.advance(n); goto loop } }
                    return doc }
```
Data: iterator state only; no result materialisation. Defaults: none (order is purely `cost()`).
Pitfalls: ordering uses cost only, not selectivity - verifiers sorted by `matchCost()` ignore how many rows each rejects (section 2.5 shows the optimal key).
NexusSearch mapping: "approximation" = bitmap leaf; "verification" = ColumnProbe / regex verify / exact distance; the bitset merge is what Roaring does per container (array lead x bitset probe).

### 2.2 Late materialisation: index-or-doc-values (Lucene IndexOrDocValuesQuery) [F]
Source: https://lucene.apache.org/core/10_0_0/core/org/apache/lucene/search/IndexOrDocValuesQuery.html (read). The javadoc says: use the index structure "when we need a good lead iterator that will be almost entirely consumed"; use doc values when "another part of the query is already leading iteration but we still need the ability to verify". Index query: good iterator, costly scorer creation. DV query: cheap to initialise, slow to enumerate all matches. No numbers in the docs. (Mechanism: the lead cost is passed down when the clause's scorer is created - `ScorerSupplier.get(leadCost)` [R].)
NexusSearch mapping: `RangeScan(BSI/raw)` = index side, `ColumnProbe(F)` = doc-values side; decide per clause with the *exact* lead cardinality (section 3.3 gives break-evens, [V]).

### 2.3 Qdrant cardinality triple (min, exp, max) [F]
Source: https://github.com/qdrant/qdrant/blob/master/lib/segment/src/index/query_estimator.rs (raw file read). With total = N:
```
AND:  min = max(0, sum(min_i) - (n-1)*N)   max = min_i(max_i)    exp = N * prod(exp_i / N)      // Frechet lower bound, independence expectation
OR:   exp = N * (1 - prod(1 - exp_i/N))    min = max_i(min_i)    max = min(sum(max_i), N)
NOT:  min = N - max   exp = N - exp   max = N - min;  primary_clauses cleared
min_should(k): enumerate k-subsets, AND each, OR the results.  Invariant min <= exp <= max (debug_assert).
```
`primary_clauses` (AND keeps the cheapest indexable clause) = lead clause. If any OR-branch has no index, the whole OR loses indexing info.
Gain: none reported (source file only). Pitfall: `exp` is the independence estimate and is wrong under correlation; only `min`/`max` are guarantees.

### 2.4 PostgreSQL statistics and the independence failure [F]
Sources: https://www.postgresql.org/docs/current/planner-stats.html , https://www.postgresql.org/docs/current/using-explain.html .
- Per-column MCV list + equi-depth histogram, up to `default_statistics_target` = 100 entries (per-column override). ANALYZE statistics are random samples, so estimates vary run to run.
- Extended stats: `dependencies` (example coefficients `{"1 => 5": 1.0, "5 => 1": 0.4231}`), `ndistinct`, `mcv`. Reported anchor: a (city, state) pair with true frequency 0.347% vs 0.0027% from independent column stats = **~100x underestimate**.
- Limits: dependencies apply only to equality/IN against constants (not ranges, LIKE, expressions), and still make an independence assumption that returns non-zero for *incompatible* values (zip=90210 with city=San Francisco).
- Cost constants shown: `seq_page_cost` 1.0, `cpu_tuple_cost` 0.01; worked example `345 pages x 1.0 + 10000 rows x 0.01 = 445.00` matches `Seq Scan (cost=0.00..445.00 rows=10000 width=244)`. `cpu_operator_cost` 0.0025, `random_page_cost` 4.0, `cpu_index_tuple_cost` 0.005 are [R] (page only implied them).
- EXPLAIN anatomy: `(cost=startup..total rows=est width=w) (actual time=first..total rows=n loops=L)`, `Buffers: shared hit= read=`, `Rows Removed by Filter`, `Heap Blocks: exact=`, `Recheck Cond`. Caveats [F]: LIMIT makes a child's estimate look wrong when it is not; **BitmapAnd/BitmapOr nodes always report actual rows = 0** (a trap we must avoid: our bitmap nodes always know exact rows).
Lesson for us: implication pairs (`format:gguf` => `type:model`, `language:c` => `type:code`) are functional dependencies; independence under-estimates them by orders of magnitude.

### 2.5 Optimal ordering of independent conjuncts [E derivation; Hellerstein-Stonebraker predicate migration, SIGMOD 1993, R]
Stream of candidate rows through filters with per-row cost p_i and selectivity s_i. Expected cost per input row for order (1..n) is
`p_1 + s_1 p_2 + s_1 s_2 p_3 + ...`. Swapping adjacent i, j changes the cost from `p_i + s_i p_j` to `p_j + s_j p_i`; i should come first iff
**`p_i / (1 - s_i) < p_j / (1 - s_j)`**. So sort ascending by rank r_i = p_i/(1-s_i) (s_i = 1 -> r = inf, never worth applying early). Lucene sorts by `matchCost` only [F], which is the s-blind special case.
Whether to apply X *before* an expensive downstream operator D (per-row cost d): apply iff `cost(X | F) < (1 - s_X) * |F| * d` (section 3.4).

### 2.6 Independence-error bounds and propagation
- Hard bounds always valid for AND of two sets: `max(0, a+b-N) <= |A n B| <= min(a,b)` (Frechet); n-way via Bonferroni, as in Qdrant's `min`/`max` [F].
- Errors compound multiplicatively: n clauses each off by factor e give e^n (Ioannidis-Christodoulakis 1991; Leis et al. "How good are query optimizers, really?" PVLDB 9(3) 2015, https://www.vldb.org/pvldb/vol9/p204-leis.pdf [R, URL from memory]).
- **Plan regret from estimation error is not uniform**: arXiv 2606.16341 (Jun 2026) models filtered-ANN planning as an argmax over strategy "phases"; boundaries at post-filter cliff s ~ k/K and in-filter cliff s_c ~ 0.83/M (graph degree M); regret is a wedge of log-width = multiplicative error eps around each boundary, **~290x more regret at a boundary than away from it**; persistent miscalibration comes from a *biased cost model*, which better estimates cannot fix [F abstract only; synthetic + SIFT1M]. Implication: (a) spend estimation effort only when the estimate lies within a factor eps of a break-even boundary; (b) calibrate constants (section 2.9) - bias there is not curable by statistics.

### 2.7 Learned cardinality estimators - evaluated, rejected [F abstract]
Han et al., "Cardinality Estimation in DBMS: A Comprehensive Benchmark Evaluation", arXiv 2109.05877 (Sep 2021, plugged into PostgreSQL, STATS dataset + STATS-CEB workload): Q-error "cannot distinguish the importance of different sub-plan queries" so they propose P-error (plan-quality based); they compare inference latency, model size, training time, update efficiency, accuracy. Abstract gives no numbers. Takeaway: judge estimators by *plan regret*, not Q-error; models need retraining on data change - bad fit for immutable segments that are merged continuously.
Other 2026 leads (snippets only [S]): arXiv 2602.17914 "Efficient Filtered-ANN via Learning-based Query Planning", arXiv 2602.11443 (system design + performance analysis of filtered ANN), arXiv 2601.01291 (Curator, low-selectivity filters), arXiv 2603.23710 (filter-agnostic vector search study).

### 2.8 Sampling-based *conditional* estimation (index-based sampling) [E; Leis et al. CIDR 2017 "Cardinality Estimation Done Right: Index-Based Join Sampling", R]
Problem: the histogram/MCV estimate of `parameters:<10B` is *marginal*; the query needs it *conditional on the lead bitmap F* (`type:model`). Fix, cheap because F is an exact Roaring bitmap:
```
cond_selectivity(F, X):                       // X = predicate with a per-row probe (range, regex-literal, ...)
  n = clamp( ceil(25*(1-s0)/s0), 256, 4096 )  // s0 = histogram prior; gives ~20% relative std-err at s0   (rel.err = sqrt((1-s)/(n s)))
  draw n sorted uniform ranks r_j in [0,|F|), walk containers with cumulative cardinalities, select(rank) inside container
       (array: index; bitset: pdep/tzcnt select-in-word with BMI2; run: arithmetic)      // ~40 ns per sample [E]
  x = #{ j : X(row_j) } ;  s_hat = x/n
  interval: Wilson, z=2.576 (99%):  (s + z^2/2n +- z*sqrt(s(1-s)/n + z^2/4n^2)) / (1 + z^2/n)
  return {lo*|F|, s_hat*|F|, hi*|F|}          // lo/hi become min/max of the estimate triple
```
Cost: n x (40 ns select + ~12 ns probe, section 3.2) = ~27 us at n=512. Captures *any* correlation with F (including functional dependencies); gives a statistical, not just Frechet, bound.
For bitmap-backed X skip sampling entirely: `and_cardinality(F, X)` is exact (cost <= nk(F) container ops). Star estimator for a bitmap conjunction with lead L: `est = |L| * prod_i ( |L n X_i| / |L| )` (naive-Bayes-on-the-lead; exact for any dependency between L and each X_i, independence only between the X_i given L). Pitfall: systematic sampling (every c/n-th rank) aliases with docid-ordered data (timestamps) - use random sorted ranks.

### 2.9 Calibrating cost constants [E; Wu et al., "Predicting query execution time: are optimizer cost models really unusable?", ICDE 2013, R]
Postgres ships hand-set unit costs (seq_page 1.0, cpu_tuple 0.01 [F]); Wu et al. re-fit the *units* with micro-benchmarks and got usable time predictions [R]. Our cost is linear in constants: `T = sum_j theta_j * f_j` where f_j are *operation counts* (containers ANDed, probes, slices x chunks, postings decoded, vectors scored). So:
1. `nexus calibrate` (<= 300 ms at first open per CPU model; stored as `calibration.json` keyed by cpuid features + build id) runs the micro-benchmarks of section 3.2.
2. Online: EXPLAIN ANALYZE records (f_j vector, measured us) per operator instance; keep a 256-entry ring per operator kind; every 64 samples solve NNLS (`theta >= 0`) and blend `theta <- 0.8 theta + 0.2 theta_fit`, clamped to [theta0/4, 4 theta0].
3. Drift guard: if median(actual/est) per operator leaves [0.5, 2] for 64 samples, flag in `stats()`.
Pitfall: [V] timings below varied ~2x between my two runs because other processes were running; calibrate with min-of-N and pin to an idle core.

### 2.10 Adaptive / mid-query re-optimisation and budgeted switching [R + E]
Prior art (recall-only): Kabra-DeWitt SIGMOD 1998 (checkpoint re-optimisation), Markl et al. POP SIGMOD 2004 (validity ranges), Zhu et al. VLDB 2017 "Looking ahead makes query plans robust" (lookahead info passing). For NexusSearch the re-optimisation point is free: after every operator the exact `|F|` is known, so the *remaining* plan is re-chosen in microseconds (section 3.1). For operators whose cost is uncertain (filtered HNSW visits, WAND postings touched) use **budgeted dual execution** (ski-rental, competitive ratio <= 2): run plan A with a work budget equal to the *estimated full cost of plan B*; if A is not done, abort and run B; total <= cost(B) + cost(B) = 2 x optimal when A's true cost > B's, else A's true cost. Concretely: HNSW budget V_max = T_brute(F) / C_node.

### 2.11 Filter-bitmap / plan caching (Lucene) [F]
Source: https://raw.githubusercontent.com/apache/lucene/main/lucene/core/src/java/org/apache/lucene/search/UsageTrackingQueryCachingPolicy.java :
min frequency to cache = **2** for costly queries (MultiTermQuery, point/range, TermInSet), **5** otherwise, **4** for BooleanQuery/DisjunctionMaxQuery (`minFrequency--`); history = ring buffer of **256** query *hash codes*; never cached: TermQuery, FieldExistsQuery, MatchAllDocs, MatchNoDocs, empty Boolean/DisMax. `shouldCache = frequency >= minFrequency`. (Cache size/segment-size gates - 10,000-doc minimum, 3% ratio, ES 10% heap - are [R].)
Per-segment immutability makes the cache trivially correct (a cached bitmap is valid until its segment dies; tombstones applied afterwards).

### 2.12 EXPLAIN formats
Postgres text format and ANALYZE extras: see 2.4 [F]. DuckDB [F]: `EXPLAIN` and `EXPLAIN ANALYZE`; `FORMAT` in {text (default), json, html, graphviz, mermaid}; `enable_profiling` (default format `query_tree`), `profiling_mode` (`standard`), `profiling_output`, `configure_profiling` (JSON metric selection); v1.5.0+ `CALL enable_profiling(format:=, save_location:=, coverage:=, mode:=)`. Field names (EC / actual rows / timing) [R].

---------------------------------------------------------------------------------------------------

## 3. The NexusSearch cost model (spec)

### 3.1 Planner shape: greedy, exact-feedback, per segment
Per segment, per snapshot (tens of microseconds budget; cardinalities come from headers):
```
plan_and_run(seg, q):
  F = live(seg) ANDNOT-applied lazily; conj = flatten_AND(q.filter)
  # P0 classify: EXACT = bitmap-backed (term, enum, bool, exists, OR-of-enum); LAZY = range, regex/trigram, text, fuzzy, semantic
  # P1 cheap exact phase
  sort EXACT ascending by card;  F = (smallest EXACT) ANDNOT tombstones          # tombstones folded into the lead: exact cards downstream
  for X in rest(EXACT): if |F| == 0 return EMPTY;  F = F AND X                    # array x bitset ~2 ns/elem; empty-lead early exit
  # P2 greedy LAZY phase with exact |F| at each step
  while LAZY:
     for X in LAZY:  (cost_X, s_X) = best_strategy(X, F)       # section 3.3; s_X from cond_selectivity (2.8) or histogram
                     rank_X = cost_X / (1 - s_X)                # 2.5
     X* = argmin rank;  if cost_X* >= (1 - s_X*) * |F| * d_down:  defer X* to PostFilter on the top-K' stream   # 3.4
     else F = apply(X*, F)                                      # exact |F| now known -> next iteration re-plans
  # P3 ranking operator with exact |F|: VectorBruteForce | VectorHNSW(filtered) | TextTopK(F) ... (3.5)
```
Ties/degenerate: any LAZY X whose estimated hi < 1 row => run it first (cheap kill); if `max` bound of a clause = 0 return EMPTY before touching data.

### 3.2 Constants (ns, single thread, defaults = measured [V] unless tagged; all overridable by `calibration.json`)
| Const | Default | Meaning | Status |
|---|---|---|---|
| C_KEY | 15 | per container header open/key-compare | [E] |
| C_AND_BB | 1,100 per 8 KB pair | bitset AND/OR/ANDNOT + store + popcount (measured 1,050 L1-resident; 1,260 DRAM-streaming = 13 GB/s read) | [V]; idle machine likely 400-600 |
| C_AND_AB | 2.0 / array elem | array element vs bitset container bit test | [E] |
| C_MERGE | 5.4 / input elem | array x array merge intersect, branchy (5.4-6.6 ns measured) | [V]; branchless/SIMD target 1.5 [E] |
| C_GALLOP | 3.0 x (1 + log2(b/a)) / small elem | galloping; measured 20.9 ns at a=64,b=4096 | [V] |
| C_OR_ARR / C_POP | 1.5 / elem ; 500 / container | lazy OR into scratch bitset; final popcount | [E] |
| C_SCAN32 / C_SCAN64 | 0.25 / 0.55 per row (AVX2 compare->bitmap) | measured 0.248 / 0.551; scalar 1.39. Equivalent to 16 / 14.5 GB/s | [V] |
| C_BYTE | 0.069 per byte | single-thread DRAM stream (14.5 GB/s) | [V] |
| C_BSI | 600 per slice per 65,536-row chunk | = max(8 KB x C_BYTE = 565, ~3 vector ops x ~150) | [V]+[E] |
| C_PROBE(k) | 6.3 + 10/(1 + k/2000) per probe | sorted ids, 128 MB int64 column; fits measured 16 (k=100), 12.3 (1e3), 8.3 (1e4), 7.9 (1e5), 6.3 (1e6) | [V] fit |
| C_ITER_PROBE | 5.4 per set bit | iterate 10%-dense bitmap and probe | [V] |
| C_SELECT | 40 | rank->docid inside a container | [E] |
| C_NEXTGEQ | 150 | postings skip + block decode (BP128) | [E] |
| C_POST | 0.6 / posting | SIMD-BP128 decode | [E] |
| C_SCORE | 6 / doc | BM25 contribution with precomputed idf/norm | [E] |
| C_VERIFY | 80 + 1 x bytes | regex/substring verification of one candidate | [E] |
| C_VMISS | 40 | random vector start (first cache-line miss, prefetched) | [E] |
| C_NODE | 30 | HNSW neighbour list + visited-set bookkeeping per node | [E] |
`nk(B)` = number of containers of B (exact, header); `N` = live docs of the segment; chunk = 65,536 rows; `d_v` = vector bytes.

### 3.3 Operator formulas
Every operator returns `{card: (lo, exp, hi), cost_ns, bytes}`; leaf bitmap cards are exact (`lo = exp = hi`).

**BitmapScan(term/enum/bool)**: `cost = 50 + C_KEY * nk`; card = `sum(cardm1+1)` over headers (exact, zero payload reads); frozen NXR is used in place (no copy).

**Intersect(A, B)** (bitmap x bitmap): per key present in both (upper bound `min(nkA, nkB)`):
`bitset x bitset: C_AND_BB;  array(a) x bitset: C_AND_AB * a;  array(a) x array(b): min(C_MERGE*(a+b), C_GALLOP(a,b));  run x any: ~8 ns per run [E]`; plus `C_KEY * (nkA + nkB)` key merge.
`and_cardinality` is the same without the store (cost within 10%, [V] 1,173 vs 1,073 ns, i.e. no saving - budget it as a full op).
Card: `(max(0, a+b-N), N*sA*sB or star estimate, min(a,b))`. Order k children by ascending card; chain cost = `sum_i cost(R_{i-1}, X_i)` with `|R_i|` exact after each step.

**Union(A_1..A_m)** (lazy OR, scratch bitset per key): `sum_i nk_i*C_KEY + sum over array elems C_OR_ARR + sum over bitset containers C_AND_BB/2 + C_POP * nk_out`. Card: `exp = N(1 - prod(1 - s_i))`, `hi = min(sum a_i, N)`, `lo = max a_i` [F Qdrant]. **If the field is single-valued (enum), the branches are disjoint: card = sum a_i exactly** (e.g. `format:(onnx OR gguf)`).

**AndNot(A, B)**: cost as Intersect; card `lo = max(0, a-b)`, `exp = a(1 - sB)` (or exact `a - and_card` when computed), `hi = a`. `NOT B` alone = `live ANDNOT B`, `nk` ops, exact card `N - |B|`.

**RangeScan** (two physical operators; choose min):
- `RangeBSI(F)`: chunks to touch `n_ch = nk(F)` minus chunks pruned by zone maps (NONE) or short-cut (ALL); `cost = n_ch * s_eff * C_BSI` with `s_eff = k - ctz(c)` slices ([05] fused GE/LT). Example: k=24 slices, 16 M rows (256 chunks) = 3.7 ms (cold).
- `RangeRaw(F)`: `n_ch * 65536 * w * C_BYTE` where w = column width in bytes (int32 -> 16 us/chunk = 4.1 ms per 16 M rows [V 0.248 ns/row]; int64 8.8 ms). BSI beats raw by ~ `8w / k` (memory-bound); parity for int32 at k = 32. => **raw-scan first, BSI is an optimisation** (finding [V]).
- card: histogram (note 05; error < 2/64) or `cond_selectivity` (2.8) when |F| < N/2.

**ColumnProbe(F, X)**: `cost = 50 + |F| * C_PROBE(|F|)` (raw column, row-major, narrowest width; dictionary-coded strings +3 ns). **Never probe a BSI-only column**: reconstructing a value costs k scattered cache misses (~k x 10 ns) - keep a RAW column beside the BSI (8N bytes at most).
card = `|F| * s_X` with s_X from 2.8.

**Per-chunk dispatch (exact cost from headers)**: for each chunk j of F, `n_j = card(F_j)` is O(1); inside RangeScan choose per chunk: ZM says NONE -> skip, ALL -> take `F_j`; else probe iff `n_j * C_PROBE_chunk < s_eff * C_BSI` i.e. **`n_j < ~60 * s_eff`** (with C_PROBE ~ 10 ns): k=24 -> 1,440 per 65,536 (2.2% density); raw int32 -> ~1,600 (2.4%). Operator cost = `sum_j min(...)`, computed exactly in O(nk) before running.

**TextTopK(q, F, k)** (BM25, block-max WAND/MaxScore): two physical plans, pick the min:
- `WAND_full`: `phi * sum_i df_i * (C_POST + sigma*C_SCORE)` with phi = fraction of postings touched (default 0.2 at k=10, 2-4 terms; sigma ~ 0.5 = fraction fully scored) and a shared-threshold bonus across segments; F applied as a required filter (bitmap `contains` ~5 ns per surfaced doc). phi, sigma are **learned from EXPLAIN ANALYZE** per (n_terms, log10 k) bucket (EWMA).
- `F_driven`: `|F| * q * C_NEXTGEQ + (docs with >=1 hit) * C_SCORE` (forward-only advance over F).
Example [E]: q=2, df=1e6 each, phi=0.2: WAND ~0.72 ms; F_driven wins when |F| < ~2,400.
card of the text clause = `df` (single term, exact); AND of terms `(max(0,sum df - (q-1)N), N*prod(df/N), min df)`; **phrase** `"a b"`: `hi = min(df_a, df_b)`, `exp = hi * 0.1` (default, [E]) then replaced by sampled positional verification of ~256 docs of the term intersection.

**TrigramScan(literal/regex, F)**: choose the m <= 8 rarest required trigrams; `cost = sum df_i*C_POST + intersect(dense: bitmap AND, sparse: gallop) + cand * C_VERIFY(avg_len)`; `cand` hi = `min df_i`, `exp = min_df * 0.35^(m-1)` ([E]: trigrams of one string are strongly positively correlated, independence would be absurdly low), then **sample-verify** 256 candidates to get the true false-positive rate and rescale. Alternative `VerifyOnF`: `|F| * C_VERIFY` - choose it when `|F| <= cand` (analogue of ColumnProbe for text). Literals < 3 bytes: no trigram, VerifyOnF only.

**VectorBruteForce(F)**: per vector `c_vec = max(d*0.06 [FMA floor, f32], d_v * C_BYTE) (+ C_VMISS if F is sparse: avg gap > 1 vector)`; `cost = |F| * c_vec + |F| * 1 (heap)`. d=384: f32 106-146 ns, SQ8 26-66 ns, binary ~5 ns + rerank.

**VectorHNSW(filtered, F)**: `cost = V * (c_vec_rand + C_NODE)`, `V ~ 1,000-3,000` visited at s ~ 1 (note 01: Lucene 1,286 nodes for recall 0.98 at 5% selectivity [F]); in-filter traversal visits `V(s) = ef * (1 + 0.25*(1-s)/s)` [E, placeholder, replace by measured V per (s-bucket, ef) from ANALYZE]; mind the in-filter percolation cliff s_c ~ 0.83/M [F abstract]. **Rule (competitive): use brute force iff `|F| * c_vec <= V_max * (c_vec_rand + C_NODE)`**; d=384 f32, V_max = 2,000-6,000 => crossover |F| ~ 2.4-3.3k (V=2,000) up to ~8-10k (V=6,000), consistent with note 01's 3-10k. HNSW run with budget `V_max` and fallback (2.10).

**PostFilter(stream, X, k)**: ranked operator emits K' candidates, then `K' * (C_PROBE + c_pred)`. Choose `K' = ceil(1.5 * k / max(s_X, k/K_cap))`, expand x2 up to K_cap (default 20x, as Vespa's target-hits-max-adjustment-factor 20 [R, note 01]) until k survive. Use only when `cost(X|F) >= (1 - s_X) * |F| * d_down` (3.4) or X is a `matchCost`-expensive verifier.

### 3.4 Pushdown / deferral rule (one inequality)
Let `d_down` = per-row cost of the ranking operator downstream (`c_vec`, or `C_SCORE+C_POST` for text). Apply filter X to F before ranking iff
`cost(X | F) + s_X*|F|*d_down < |F|*d_down`, i.e. `cost(X|F) < (1 - s_X) * |F| * d_down`. Example: |F|=1e5, vectors d=384 f32 (d_down=106 ns): ranking costs 10.6 ms; a probe predicate with s_X=0.3 costs `1e5 * 8 ns = 0.8 ms < 0.7*10.6 = 7.4 ms` -> apply. A regex VerifyOnF at 1 us/row costs 100 ms > 7.4 ms -> defer to PostFilter on the top-K'.

### 3.5 Break-even cheat sheet ([V] constants; used for unit tests of the planner)
| Decision | Rule |
|---|---|
| probe vs RangeBSI per 65,536-chunk | probe iff `n_j < 60 * s_eff` (~2.2% density at 24 slices) |
| probe vs raw int32 scan | probe iff `n_j < ~1,600` per chunk; vs raw int64 `< ~3,600` |
| bitmap vs array container | 4,096 (note 05); array x bitset 2 ns/elem vs 1.1 us/container => array wins below ~550 elems |
| brute-force vs HNSW (d=384) | `|F|` below ~2.4-3.3k (V=2,000) to ~8-10k (V=6,000) -> brute force |
| text F-driven vs WAND | `|F| < phi * sum df * (C_POST + sigma C_SCORE) / (q * C_NEXTGEQ)` ~ 2.4k in the example |
| gallop vs merge | gallop when b/a >= 64 (Roaring) |
| multi-thread | DRAM-bound operators scale until ~25-40 GB/s aggregate [E]; plan with `C_BYTE` x (threads active) beyond that |

---------------------------------------------------------------------------------------------------

## 4. Adopt for NexusSearch (ordered implementation list)

1. **`nx_card {lo, exp, hi}` + combinators** (AND/OR/NOT/min_should) exactly per Qdrant formulas [F], plus: disjoint-OR for single-valued enum fields, hard clamps `0 <= lo <= exp <= hi <= N_live`, `assert` the invariant in debug builds. ~150 lines. (core)
2. **Planner skeleton (3.1)** in `src/query/planner.c`: classify EXACT/LAZY, lead selection (cheapest EXACT, tombstones folded in), early-empty exits, greedy LAZY loop with rank `cost/(1-s)`. Per-segment, re-planned after each operator because `|F|` is exact. (core)
3. **Operator cost functions (3.3)** as pure functions `double nx_cost_X(const nx_cost_consts*, features...)`; constants struct with the [V] defaults of 3.2; no globals. Costs return a feature vector too (op counts), so calibration (item 9) is linear. (core)
4. **ColumnProbe + RangeRaw + RangeBSI with per-chunk dispatch (3.3)**, raw column kept beside BSI (narrowest width 1/2/4/8 B). (core)
5. **Conditional sampling estimator (2.8)**: Roaring `select(rank)` with BMI2 `pdep`, n = clamp(25(1-s0)/s0, 256, 4096), Wilson 99% interval -> estimate triple. Only invoked when the histogram estimate lies within a factor 4 of a break-even boundary (2.6) or |F| < N/2 with a non-bitmap clause. (core)
6. **EXPLAIN / EXPLAIN ANALYZE (section 3.6)**: text + JSON (+ Mermaid for the UI later); per node est/actual rows with `[lo..hi]`, self/total time, `loops`, containers touched/skipped, slices touched, bytes, `strategy_reason`, rejected alternatives with costs, `q_error = max(est/act, act/est)`; unlike Postgres BitmapAnd, *never print actual rows = 0 for bitmap nodes*. (core)
7. **Ranking-operator choice (3.3/3.4)**: brute force vs HNSW with `V_max` budget and fallback; WAND_full vs F_driven; trigram vs VerifyOnF; PostFilter oversampling K' = 1.5k/s expand x2 to 20x. (important)
8. **Filter-bitmap cache (2.11)** per segment: key `(seg_id, clause_hash)`; admit at frequency >= 2 for costly clauses (range, regex/trigram, OR of >= 4 terms, semantic pre-filter), >= 5 otherwise, >= 4 for compound; never cache single-term/exists/match-all; 256-entry hash ring history; 32 MiB budget via `nx_mem`, entries <= 4 MiB frozen NXR; stored *before* tombstones. Do **not** build a plan cache in v1: per-segment planning is O(#clauses) arithmetic over header data (~microseconds) - cache *feedback* instead (item 9). (important)
9. **Calibration (2.9)**: `nexus calibrate` micro-bench (the kernels in `scratchpad/calib2.c`: AND/popcnt 8 KB, merge, gallop, scan32/64, probe sweep, vector stream); then online NNLS from EXPLAIN ANALYZE samples with clamp x/÷4; `phi`/`sigma`/`V(s)` learned as EWMA tables keyed by buckets; plus a feedback cache `(clause_sig, seg_gen) -> observed selectivity` used as the prior `s0` for 2.8 (invalidated by snapshot generation). (important)
10. **Star estimator for bitmap conjunctions** (2.8 end): `and_cardinality(L, X_i)` with the lead, only when a LAZY clause's cost depends on the intermediate size and execution has not yet produced it. (important)
11. **Learned models / adaptive indexing hooks** (stretch): log per-clause `(signature, est, act)` to a ring so the adaptive indexer can promote hot ranges to BSI/filter-cache entries. (stretch)

---------------------------------------------------------------------------------------------------

## 5. Skip and why
- **Learned cardinality estimators** (Han et al. [F]): training/update cost and inference latency are overhead for immutable, continuously merged segments; Q-error is a poor proxy for plan quality. Exact bitmaps + sampling cover our cases.
- **HyperLogLog / Count-Min for filter selectivity**: bitmaps give exact df/cardinality. Keep HLL only for NDV of numeric/keyword columns (precision p=12 -> 4 KB, std-err 1.04/sqrt(4096) = 1.6% [R]; mergeable across segments by register-max) and facets; CMS not needed.
- **Cascades / Volcano / DP join enumeration (DPhyp)**: no joins, <= ~12 operators; greedy with exact feedback is optimal-ish and keeps planning in microseconds. (Revisit for federation joins.)
- **2-D/multi-column histograms and extended-statistics objects (Postgres `CREATE STATISTICS`)** [F]: need user DDL and cover only equality; and_cardinality + sampling captures correlation without stored joint stats.
- **Plan-template cache**: nothing to save (see Adopt 8); per-segment plans depend on segment cards anyway.
- **Mid-query re-optimisation with materialisation checkpoints**: free here (exact |F| at every boundary), no checkpoint machinery needed.
- **Trusting pgvector-style stats-only planners for vector search** [S]: the reported failure mode (approximate plan chosen when an exact scan equals it) is avoided by the exact-cardinality competitive rule.
- **Postgres-style fixed cost units (page-oriented)**: our unit is ns on this CPU, linear in operation counts, calibrated.

---------------------------------------------------------------------------------------------------

## 6. Novel ideas specific to NexusSearch
1. **Exact-lead conditional sampling** (2.8): because every lead is an exact Roaring bitmap with O(#containers) rank-select, conditional selectivity with a 99% CI costs ~30 us. The CI directly gives the (lo, hi) of the estimate triple, so `min/max` stop being vacuous Frechet bounds. No engine surveyed here does this (as far as I verified).
2. **Exact cost from container headers**: with per-chunk dispatch the cost of a late-materialised range is `sum_j min(probe_j, bsi_j)` where `n_j` is read from F's headers - the *cost model is evaluated exactly*, only selectivity is estimated.
3. **Roofline cost vectors**: operators return `(cpu_ns, dram_bytes)`; predicted time under N parallel segment tasks = `max(cpu, bytes / (BW / active_threads))`; avoids picking bandwidth-hungry scans when 8 workers run concurrently ([V] single-thread 14.5 GB/s).
4. **Budgeted dual execution** (2.10) generalised as an operator wrapper `nx_op_budgeted(A, budget=cost(B), fallback=B)`: bounded 2x regret for HNSW-vs-brute-force, WAND-vs-F-driven, trigram-vs-VerifyOnF, independent of estimate quality (regret cannot concentrate at a boundary [F 2606.16341]).
5. **Counterfactual EXPLAIN**: print costs of rejected alternatives and, in ANALYZE, whether the winner was actually faster when the executor ran the budgeted fallback; this both debugs the planner and feeds calibration.
6. **Selectivity-aware two-phase ordering** (2.5) instead of Lucene's `matchCost`-only order for verifiers.
7. **WATCH (percolator) planning**: a standing query is planned once as ColumnProbe-only predicates sorted by `p/(1-s)` and evaluated on each new doc (|F| = 1, so probes always win).
8. **Implication mining for free**: if `and_card(A, B) == |A|` at segment build for enum pairs, record `A => B` in `stats`; the planner then drops B from AND-lists and returns exact estimates.

---------------------------------------------------------------------------------------------------

## 7. Validation and test ideas
1. **Differential oracle** (architecture principle 7): random ASTs (1-12 clauses, AND/OR/NOT nesting) vs a naive row-at-a-time evaluator, with *every* strategy forced by hint (`force=probe|bsi|raw|walk|brute|hnsw|fdriven`): results must be identical sets.
2. **Estimate invariants** (property tests): `lo <= exp <= hi`; actual in `[lo, hi]` 100% for the Frechet/Bonferroni triple; sampled Wilson 99% intervals cover the truth >= 98% over 10k trials on synthetic data with correlation rho in {0, 0.5, 0.9, 1.0} and functional dependency A => B; A = B and A n B = empty adversaries.
3. **Estimator accuracy**: Q-error median <= 1.5 and p95 <= 4 for 2-4 clause conjunctions with correlated fields (independence estimator will fail this - keep it as the control to prove the estimator matters).
4. **Plan regret grid** (inspired by [F 2606.16341]): sweep selectivity `s` on a log grid 1e-5..1 x {1,2,4 clauses} x {range, regex, text, vector}; for each cell run all feasible plans, report `regret = chosen/best`. Gate: median <= 1.3, p99 <= 2.0 (targets [E]); regret plot must be wedge-shaped near break-evens only; budgeted execution must cap at 2x.
5. **Cost-model fit**: 1,000 randomised operator runs; NNLS fit `R^2 >= 0.9`; predicted within 2x of actual for >= 90% of instances per operator; recalibration must converge from constants scaled by 0.25x and 4x within 512 samples.
6. **Break-even tests (3.5)**: construct F with density just below/above `60*s_eff` per chunk and assert the dispatch flips; same for brute-force-vs-HNSW at the calibrated crossover and for F_driven-vs-WAND.
7. **Bitmap-node honesty test**: EXPLAIN ANALYZE of every bitmap node reports non-zero actual rows whenever the result is non-empty (avoid the Postgres BitmapAnd trap [F]).
8. **Cache correctness**: results identical with filter cache cold/warm; after tombstone changes and merges; admission policy matches `[2,5,4]` thresholds and 256-hash history; memory returns to baseline (`nx_mem` leak counters).
9. **Permutation invariance**: permuting AND/OR children yields identical results and, modulo ties, identical plan and EXPLAIN.
10. **Golden EXPLAIN** files with time fields masked; JSON schema validated.
11. **Hostile-input bounds**: >= 1,000 clauses, deeply nested NOT, huge OR lists: planner time <= O(n log n), memory bounded (coding standard).

---------------------------------------------------------------------------------------------------

## 8. EXPLAIN / EXPLAIN ANALYZE output (spec)
Text (Postgres-like header, DuckDB-like tree), JSON mirrors the same fields; illustrative values [E]:
```
EXPLAIN ANALYZE type:model parameters:<10B format:(onnx OR gguf) semantic:"small neural nets for local use"
Segment seg-0007  N=1,204,331 live=1,203,990   plan=4.1us exec=1.9ms
VectorBrute(F, k=10, sq8, d=384)     (cost=0.18ms rows=10) (actual 0.21ms rows=10 loops=1 scored=2,912)  why: |F|=2,912 < V_max crossover 3,300
  -> ColumnProbe parameters:<10B     (cost=0.04ms rows=est 2,900 [2,100..3,900]) (actual 0.04ms rows=2,912 probes=14,300 q_err=1.00)
       why: n_j<=1,440 in 5 of 5 chunks (probe 0.04ms vs bsi 0.85ms vs raw 1.9ms)  sampled n=512 s_hat=0.20 CI99=[0.15,0.26]
     -> Intersect                    (cost=0.02ms rows=14,300 exact)  (actual 0.02ms rows=14,300)
          BitmapScan type:model      (rows=88,412 exact, containers=22)
          Union disjoint             (rows=51,031 exact)  [BitmapScan format:onnx 40,112 + format:gguf 10,919]
alternatives rejected: VectorHNSW(filtered) est 0.41ms
```
Per-node JSON keys: `op, est:{lo,exp,hi,cost_ns,bytes}, act:{rows,self_ns,total_ns,loops,containers,skipped_zm,slices,bytes,visited}, why, alts:[{op,cost_ns}], q_error`.

---------------------------------------------------------------------------------------------------

## 9. References
Fetched [F]:
- Lucene ConjunctionDISI: https://raw.githubusercontent.com/apache/lucene/main/lucene/core/src/java/org/apache/lucene/search/ConjunctionDISI.java
- Lucene IndexOrDocValuesQuery: https://lucene.apache.org/core/10_0_0/core/org/apache/lucene/search/IndexOrDocValuesQuery.html
- Lucene UsageTrackingQueryCachingPolicy: https://raw.githubusercontent.com/apache/lucene/main/lucene/core/src/java/org/apache/lucene/search/UsageTrackingQueryCachingPolicy.java
- Qdrant query estimator: https://github.com/qdrant/qdrant/blob/master/lib/segment/src/index/query_estimator.rs
- Vespa ANN/HNSW docs (names only, no defaults): https://docs.vespa.ai/en/querying/approximate-nn-hnsw.html
- PostgreSQL planner statistics: https://www.postgresql.org/docs/current/planner-stats.html ; using EXPLAIN: https://www.postgresql.org/docs/current/using-explain.html
- DuckDB profiling / EXPLAIN: https://duckdb.org/docs/current/dev/profiling.html
- Han et al., Cardinality Estimation in DBMS: A Comprehensive Benchmark Evaluation, arXiv 2109.05877 (abstract): https://arxiv.org/abs/2109.05877
- Mandarapu, Kunkunuru, Filtered ANN as a Phase Transition: When Selectivity-Estimation Error Causes Plan Regret, arXiv 2606.16341 (abstract): https://arxiv.org/abs/2606.16341
Seen only as search results [S]: arXiv 2602.11443, 2602.17914, 2601.01291, 2603.23710 (https://arxiv.org/abs/<id>), https://github.com/vecadvisor/vecadvisor.
Recall-only [R] (verify before citing): Hellerstein-Stonebraker predicate migration SIGMOD 1993; Leis et al. PVLDB 9(3) 2015 (https://www.vldb.org/pvldb/vol9/p204-leis.pdf); Leis et al. CIDR 2017 index-based join sampling; Ioannidis-Christodoulakis SIGMOD 1991; Kabra-DeWitt SIGMOD 1998; Markl et al. POP SIGMOD 2004; Zhu et al. VLDB 2017; Wu et al. ICDE 2013; Flajolet et al. HLL 2007 / Heule et al. HLL++ EDBT 2013; Ding-Suel Block-Max WAND SIGIR 2011; Broder et al. WAND CIKM 2003.
Local [V]: `C:\Users\kai99\AppData\Local\Temp\claude\C--Users-kai99-Desktop-NEXUS\30691b56-3f04-4eae-bc73-16aa14fcdb85\scratchpad\calib2.c` (micro-benchmark source; results quoted in 3.2).
Cross-refs: docs/research/01-filtered-ann.md (planner for vectors, Qdrant/Vespa thresholds), docs/research/05-bitmaps-columns-dicts.md (cardinalities, zone maps, histograms, BSI).
