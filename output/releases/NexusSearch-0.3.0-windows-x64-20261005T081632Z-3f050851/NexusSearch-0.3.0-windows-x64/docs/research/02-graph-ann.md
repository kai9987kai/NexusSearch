# 02 - Graph ANN: HNSW, Vamana/DiskANN, incremental maintenance (research note for NexusSearch)

Scope: the in-process graph index for ~1k-10M vectors per vector field (cosine / L2 / IP), living inside immutable
segments with tombstones, NRT refresh, merge and WAL. Filtering by structured bitmaps is covered in
`01-filtered-ann.md`; quantisation/SIMD kernels are other notes. Date of research: 2026-10.

Evidence tags: **[F]** fetched and read this session (arXiv PDF text / raw source file); **[F-src]** fetched source file, read through a
summarising fetch (not line-verified); **[R]** recall-only, not verified; **[D]** derived by me (arithmetic or engineering
judgement, not from a source). Anything with no tag is [F].

## 0. TL;DR (decisions)

1. Build **one flat navigable graph per sealed segment** (base layer only) with a small **entry table**; keep HNSW's upper layers only as an
   optional mode for low-dimensional vectors (d < ~32). Evidence: flat == hierarchical in p50/p99 latency and recall on 13 datasets, 1M-100M
   vectors, d >= 96 [4]; the hierarchy only helped on low-dim synthetic data (d < 32, secondhand via [4]).
2. Use **one pruning routine**: `robust_prune(p, C, alpha, R)`. HNSW's "heuristic" neighbour selection IS RobustPrune with alpha = 1 [1][5].
   Use alpha = 1.0 for the fast-growing segment, **alpha = 1.2 and two build passes for sealed segments and for any graph that will be consolidated** [5].
3. **Never mutate a sealed graph for deletes.** Tombstone bitmap; traverse *through* tombstoned nodes, exclude them from results [5][6]; physically
   remove them at merge time with FreshDiskANN's consolidation (Alg. 4) at alpha = 1.2; trigger compaction at ~20% tombstones [6].
4. **Do not copy hnswlib's `replace_deleted` / `updatePoint`.** It creates unreachable (in-degree-0) nodes: 3-4% of SIFT1M after ~3000 cycles of 5% churn,
   recall -3%, not recoverable by raising ef; replaced-update is 5-10x slower than a query on GIST/ImageNet [7].
5. Memory layout: pointer-free arrays, cache-line-aligned neighbour rows, **node ids reordered by RCM at seal time** (10% QPS at <1M, up to 40% at 100M) [8].
6. Visited set = bitset (1.25 MB at 10M nodes) with a dirty-word list [D]; neighbour expansion in two passes (collect+prefetch, then score) [D].

## 1. Landscape

| Line | Who | Core idea | Status for us |
|---|---|---|---|
| HNSW | Malkov & Yashunin, arXiv 2016, rev. 2018 [1]; hnswlib [2][3] | layered proximity graphs, greedy descent, diversity heuristic | classic; adopt base layer + heuristic |
| Vamana / DiskANN | Jayaram Subramanya et al., NeurIPS 2019 [R]; params via [5] | flat graph, RobustPrune with alpha > 1, dense-ish graphs, medoid entry | adopt alpha-pruning |
| FreshDiskANN | Singh, Subramanya, Krishnaswamy, Simhadri, arXiv 2105.09613, 2021 [5] | streaming updates: DeleteList + batch consolidation + StreamingMerge, SSD tier | adopt consolidation + LSM-of-graphs idea; skip SSD tier |
| IP-DiskANN | Xu, Manohar, Bernstein, Chandramouli, Wen, Simhadri, arXiv 2502.13826, 2025 [6] | in-place deletion w/o stored in-edges | optional for in-memory mutable mode |
| MN-RU | Xiao, Zhan, Xi, Hou, Liao, arXiv 2407.07871, 2024 [7] | documents HNSW "unreachable points"; faster replaced-update | adopt the diagnosis + health metric, not the patch |
| FlatNav / Hub Highway | Munyampirwa, Lakshman, Coleman, arXiv 2412.01940 (v3 Jul 2025, ICML-25 VecDB workshop) [4] | hierarchy is unnecessary in high-d because hubs form a "highway" | adopt flat default |
| Graph reordering | Coleman, Segarra, Smola, Shrivastava, arXiv 2104.03221, 2021 [8] | relabel nodes so co-visited nodes are adjacent in memory | adopt RCM at seal |
| Others (not fetched) | NSG, HCNNG, ParlayANN/Dobson et al. 2023, Lin & Zhao 2019, SPFresh (SOSP'23, IVF-style), Lucene HNSW merges | see [4][6] secondhand | [R] only |

## 2. Techniques

### T1. HNSW (Malkov & Yashunin) [1][2][3]
- **Level**: `l = floor(-ln(U) * mL)`, `mL = 1/ln(M)` (skip-list p = 1/M); hnswlib: `r = -log(U) * mult_`, `mult_ = 1/log(M)`. P(level >= l) = M^-l, so the
  hierarchy holds N/(M-1) upper nodes; top level at N = 10M, M = 16 is about ln(1e7)/ln(16) = 5.8 [D].
- **Insert** (Alg. 1): greedy descent with ef = 1 from top to l+1; from min(l, L) to 0: `W = SEARCH-LAYER(q, ep, efC, lc)`, `SELECT-NEIGHBORS(q, W, M, lc)`,
  add bidirectional links, shrink any neighbour whose list exceeds Mmax (Mmax0 at layer 0) with the same selection routine. Note the new node gets **M** forward edges;
  layer-0 lists can grow to Mmax0 = 2M through back-links.
- **SEARCH-LAYER** (Alg. 2): candidate min-heap C, result max-heap W of size ef, visited set; stop when `dist(c,q) > dist(furthest in W, q)`.
- **Heuristic** (Alg. 4): scan candidates nearest-first; keep e only if `dist(e,q) < dist(e,r)` for every already kept r. `extendCandidates` default false (helps
  only very clustered data), `keepPrunedConnections` optional fill-up. Biggest win on low-d data, at high recall on mid-d data, and clustered data; "little difference" for
  uniform very-high-d data [1].
- **Parameters** [1][2][3]: M in 5-48 (paper), 12-48 typical, 48-64 for very high-d embeddings (hnswlib docs); `Mmax0 = 2M` (Mmax0 = M gives a "very strong penalty at high recall";
  larger wastes memory); `mL = 1/ln M`; efC "large enough that K-ANNS recall during construction is close to unity (0.95 enough for most cases)"; hnswlib doc check: if recall of
  M-NN search at `ef = efC` is < 0.9 there is room for improvement. **hnswlib constructor defaults: M = 16, efC = 200 (clamped to >= M), search ef = 10**; ef must be >= k.
- **Memory**: `(Mmax0 + mL*Mmax) * bytes_per_link` per element = 60-450 B/object for M 6-48 (4-byte links) [1]; hnswlib docs: "M * 8-10 bytes per element" [2].
- **hnswlib layout** [3]: level-0 block per node = `[u32 header: count in low 16 bits, flag byte (DELETE_MARK=0x01) at byte 2][maxM0 x u32 ids][vector][label]`; upper levels in
  separately allocated lists; visited list = array of tags + epoch counter `curV` (no clearing between queries; pooled); per-node link-list locks + 65536-way striped label locks +
  global lock only while raising max level; prefetch of visited tag and vector of the next neighbour inside the expansion loop; query search skips the deletion check via a
  `bare_bone_search` template flag and accepts an `isIdAllowed` filter functor; deleted nodes stay navigable but are not returned.
  Save format = header fields (`offsetLevel0_, max_elements_, cur_element_count, size_data_per_element_, label_offset_, offsetData_, maxlevel_, enterpoint_node_, maxM_, maxM0_, M_, mult_, ef_construction_`), the level-0 blob, then per-element (size, upper-level list bytes) [3].
- **Pitfalls**: (a) the level-0 record is `4*(maxM0+1)` bytes + vector: 132 B for M = 16, not cache-line aligned [D]; (b) per-node mutex arrays are large at 10M nodes [D]; (c) the file is not mmap-able because of the
  variable upper-level section [D]; (d) replace_deleted/updatePoint, see T5.

### T2. "Flat is enough": Hub Highway Hypothesis [4]
- Flat graph = HNSW bottom layer (extracted from hnswlib, and separately built from scratch with *no hierarchy at all*, Appendix E: identical results). No consistent p50 or p99 latency gap on
  13 datasets, 1M-100M vectors (SIFT, GIST, Deep, SpaceV, Yandex-DEEP, GloVe, ...). Hierarchy "useful only for d < 32" (Lin & Zhao 2019, secondhand).
- Mechanism: in high-d, k-NN graphs have hubs (k-occurrence skewness); top-5%/top-1% most-visited nodes link to each other more than chance (Mann-Whitney p < 1e-5 on most L2/real sets at P99);
  queries spend most of the first 5-10% of steps on hubs. **Angular/cosine data has weaker hubness** (GloVe, cosine IID-normal); L2 hubness grows with d. MSMARCO MiniLM-L6 (384-d) embeddings show the same long-tailed node-access distribution [4, App. F].
- Preferential attachment (early inserts accumulate in-edges) explains only 0.1-0.5% of access variance on real sets (up to ~24% on angular IID-normal) [4, Table 7] -> insertion order is a minor factor; still shuffle bulk builds [D].
- Memory claim: peak construction memory BigANN-100M 183 -> 113 GB (-38%), SpaceV-100M 104 -> 85.5 GB (-18%), Yandex-DEEP-100M 100 -> 60.7 GB (-39%) at 32 cores - but compares a research prototype with hnswlib and the authors flag implementation differences [4].
  **My arithmetic [D]**: in a pointer-free layout the upper layers cost N/(M-1) nodes x (M+1) x 4 B = ~4.5 B/node at M = 16 vs 132 B/node for layer 0, i.e. ~0.3% of a 384-d fp32 node. So flattening is justified by simplicity (lock-free frozen CSR, trivial mmap, easy merge/reorder) and equal quality, not by memory.
- Params used there: m = 32, efC = 100, efS = 200, k = 100 [4, Table 1]. Their stated lesson: bigger wins now come from link pruning and search-algorithm design, and clever entry initialisation matters less in high-d.

### T3. Vamana RobustPrune, alpha-RNG, and FreshVamana updates [5]
- `RobustPrune(p, V, alpha, R)`: `V <- (V U N_out(p)) \ {p}`; repeat: `p* = argmin_{p' in V} d(p,p')`, add p* to N_out(p); stop at |N_out| = R; remove from V every p' with `alpha * d(p*, p') <= d(p, p')`.
  alpha = 1 is the HNSW heuristic; alpha > 1 keeps more edges (denser, fewer hops).
- `Insert(p)`: `GreedySearch(s, p, 1, L)` -> visited V; `N_out(p) = RobustPrune(p, V, alpha, R)`; for each j in N_out(p): if `|N_out(j) U {p}| > R` re-prune j else append (new node gets up to R edges immediately, unlike HNSW's M).
- **Why deletions break naive graphs**: with HNSW/Vamana/NSG and delete policies A (drop edges) or B (connect in-neighbours to out-neighbours), recall decays over 20 cycles of delete+reinsert of 5% of SIFT1M; cause = the graph **gets sparser** with aggressive pruning. Fix = alpha > 1 [5, Sec. 3.3-4].
- **Delete(LD, R, alpha)** (batch consolidation): for every live p with `N_out(p) cap LD != empty`: `D = N_out(p) cap LD; C = (N_out(p) \ D) U (U_{v in D} N_out(v)); C = C \ D; N_out(p) = RobustPrune(p, C, alpha, R)`. Needs no in-edge lists; one linear scan; distance computations only for affected nodes.
- Lazy semantics: deleted ids go in a DeleteList, the graph is untouched, searches *navigate* through them but filter them from results; consolidate after "1-10% of the index size" are deleted.
- **Parameters** [5]: R = 64, Lc = 75, alpha = 1.2 (billion-scale run, SIFT1B 800M); alpha in {1, 1.1, 1.2, 1.3} tested: recall stable over 40+ cycles for all alpha > 1, decays only at alpha = 1; static latency-at-recall improves 1 -> 1.2, no gain beyond; 1.2 chosen for best latency with lowest average degree. Search Ls tuned for 95% 5-recall@5; PQ 32 B/vector for the SSD tier.
  Static Vamana is built with **two refinement passes**, giving slightly better graphs than the streaming one (explains a small recall dip at cycle 0 of FreshVamana).
- Gains: FreshVamana build 1.5-1.8x faster than 2-pass Vamana (SIFT1M 21.8 s vs 32.3 s; GIST1M 228 s vs 417 s; R = 64, Lc = 75, alpha = 1.2); merging 5%/10%/50% churn into FreshVamana costs 11.7/21.9/62.4% (SIFT1M), 5.8/13.1/52.4% (GIST1M) of a rebuild; 800M index with 30M inserts+deletes: StreamingMerge 15,832 s (40 threads) vs rebuild 83,140 s (96 threads) [5, Tables 1-2, Fig. 11] (the paper's text says ">7x"; wall-clock ratio of the table is 5.3x). TempIndex RAM: 128 B vector + 256 B links (R = 64) + ~100 B aux per point.
- Locking: per-node lock on `N_out(p)`, held briefly; insert throughput scales near-linearly with threads [5].

### T4. IP-DiskANN in-place deletion [6]
- Problem: singly-linked graph has no in-edge list; doubly-linking would halve capacity or double memory and complicate locking. FreshDiskANN's consolidation adds R^2 edges per deleted node.
- `Delete(p; ld=128, k=50, c=3, alpha=1.2, R)`: `[Visited, Cand] = GreedySearch(G, x_p, k, ld)`; approximate in-neighbours `N'in = {z in Visited : p in N_out(z)}`; for each z in N'in add the `c` candidates closest to z (excluding p); for each w in N_out(p) add w to the out-lists of the `c` candidates closest to w; remove p now; RobustPrune any list that exceeded R; later, when deletions exceed fraction t (10/20/30% tested), scan and strip dangling edges (no distance computations).
- Evidence (16 threads, runbooks SlidingWindow/ExpirationTime/Clustered, MSTuring-10M/30M, Wikipedia-Cohere 768-d IP): recall@10 94.8 vs FreshDiskANN 94.4 vs HNSW 91.8 (MSTuring-10M sliding); total update time (deletion + insertion; HNSW's deletes are folded into its insert time) HNSW 4016 s vs IP-DiskANN 1110 + 1461 = 2571 s vs FreshDiskANN 3162 s on MSTuring-10M sliding-window, and HNSW 15,296 s vs IP 6,279 s vs Fresh 6,798 s on the 30M clustered runbook (sums computed by me from [6, Table 1]); IP deletion beats FreshDiskANN's consolidation only at >= 10M vectors (slower at 1M: 92 s vs 38 s). Ablations (clustered 30M): c = 1/2/3/5 -> recall 91.0/92.2/92.5/92.8; ld = 60/128/200 -> 91.8/92.6/92.9 (deletion time 1898/3160/4576 s); k = 10/50/100 -> 91.8/92.5/92.6; t = 30/20/10% -> 92.2/92.5/92.6.
- Surprising data point: a graph maintained by these updates beat a graph **rebuilt from scratch** on the active set at the same step (MSTuring-30M clustered) [6].
- Soft-delete status quo: tombstones, rebuild at "10-20% deleted" [6]. Parameters used there: R = 64, lb = ls = 128, alpha = 1.2 (high recall) / R = 32, l = 64 (low recall); HNSW baseline M = 48, efC = efS = 128.

### T5. HNSW update/delete failure modes (hnswlib) [3][5][6][7]
- hnswlib deletes = mark flag (still navigable); optional replace: a new insert reuses a deleted slot: gather 1-hop and 2-hop neighbours of the deleted node, re-select neighbours for each 1-hop node (O(M^3) per layer), then insert as normal. [7]
- **Unreachable points** (def.: out-edges but zero in-edges on all layers, not the entry point): after 3000 iterations of delete-5%/reinsert on SIFT, 3-4% of points unreachable, recall declines ~3% ("cannot be mitigated by increasing ef"), growing with more iterations; GIST ~3-4%, ImageNet 2-3% after 200 iterations [7].
- MN-RU: re-select only the 1-hop nodes that actually link to the deleted node, with candidates = their current list U deleted node's list U new point (O(M^2) per layer); 2-4x faster than HNSW-RU, fewer unreachable points. Backup-index variant (rebuild index of unreachable set when > tau = 40,000 replaced updates) is a band-aid [7].
- Cost: HNSW with replace needs ~1.6x (10M sliding window) to ~2.3x (30M clustered) the total update time of IP-DiskANN/FreshDiskANN (my sums from [6, Table 1]) because O(R^3) edges per delete, no amortisation of pruning, and a larger steady-state index [6].

### T6. Graph reordering for cache locality [8]
- Relabel node ids so graph neighbours sit together; layout is the flat "node+links+data in one block" of nmslib-HNSW; recall/search algorithm unchanged.
- Results (SIFT100M, 1 core, hardware counters): L1 miss 19.53% -> 14.46% (Gorder) / 17.37% (RCM); L2 13.9 -> 9.6 / 7.61; L3 6.5 -> 4.0 / 5.1; **TLB 3.85 -> 2.14 / 2.56**. P99 latency -17% (RCM), -30% (Gorder); average query time -10% (<1M nodes) to **-40%** (large, high recall); both consistently positive. Lightweight degree-based orders (DegSort, HubSort, HubCluster, DBG) give ~no gain (L1 22.76 -> 22.8) because k-NN graphs lack power-law degree [8, Table 2].
- Cost: Gorder is O(sum of squared degrees) but still ~10x cheaper than index construction; RCM cheaper. Reordering is pure relabelling, so it is safe to apply at seal/merge.
- Parameters in their test: efC = 100, max degree kc chosen from {4,...,96}, search buffer 100-5000, k = 100.

### T7. Segment graph merge (what we know)
- FreshDiskANN StreamingMerge = Delete phase (Alg. 4 on the long-term index), Insert phase (Alg. 2 against the LTI, with the new points' edges batched), Patch phase (backward edges), block-wise, multithreaded; peak merge RAM ~100 GB for 800M (125 GB per 1B incl. TempIndexes) [5].
- Lucene (segment-based HNSW) has public issues about CPU and heap cost of HNSW merges [6, refs 49-50]; mechanics [R: it inserts vectors of smaller segments into the largest segment's graph].
- DiskANN billion build costs 1.1 TB peak RAM and 2 days at 32 vCPU - the reason segment merges must be incremental [6].

## 3. Adopt for NexusSearch

### 3.1 Segment lifecycle (graph part)
1. **Tiny segment (n <= ~2-4k vectors)**: no graph; SIMD brute force. Break-even [D]: a graph query scores roughly 1-3k nodes (see `01-filtered-ann.md` 3.1), so brute force wins below that.
2. **Growing segment** (RAM, concurrent insert + search): mutable flat graph, alpha = 1.0, per-node spinlocks, insert-only. Deletes of docs in this segment go to its tombstone bitmap. Seal at ~100k vectors or on refresh/flush [D, tunable].
3. **Seal**: re-run pass 2 over all nodes (alpha = 1.2, same R), final prune to R0, compute entry table, RCM reorder, write the frozen file (3.5). Sealed = lock-free, mmap-able.
4. **Merge** (tiered, fan-in 4-8 [D]): see 3.6. **Compaction** triggered by tombstone fraction > 20% [6] or by tier policy.

### 3.2 Parameters (defaults; every number is a starting point for the benchmark harness)
| Param | Default | Notes / source |
|---|---|---|
| `R0` (max out-degree, row = 1 count + R0 ids) | **31** (row = 128 B = 2 cache lines); **63** for "high recall"/d >= 768 (256 B) | ~hnswlib M = 16 / 32 (Mmax0 = 2M); aligned rows [D] |
| `M_ins` (forward edges on insert) | `(R0+1)/2` = 16 / 32 | HNSW semantics [1]; option `M_ins = R0` (Vamana semantics [5]) to A/B |
| `efC` | 200 (balanced), 100 (bulk fast), 400 (seal pass 2) | hnswlib default 200 [3]; Hub paper uses 100 [4] |
| `ef` (search) | `max(k, 64)`; calibrated per segment (3.7) | hnswlib default 10 is too low for k = 10-100 [3] |
| alpha | 1.0 growing / 1.2 sealed + consolidation | [5] |
| entry table size | 32-64 nodes | 3.4 [D] |
| tombstone compaction | 20% | [6] |
| hierarchy | off if dim >= 64; on (mL = 1/ln M) if dim < 32; between: benchmark | [4][1] |
| brute-force threshold | n <= 4k | [D] |

Pruning geometry: if distances are **squared L2**, apply alpha as `alpha^2` (`alpha2 * d(s,c) <= d(p,c)`); for **cosine on pre-normalised vectors** (`1-cos = ||a-b||^2/2`) the same alpha^2 rule holds. The paper's alpha is defined on true distances [5] [D]. Raw (non-normalised) IP is not a metric: normalise at ingest or run alpha = 1.

### 3.3 Data structures (C11)
```c
typedef struct { float d; uint32_t id; } nx_nbr;              /* ordered by (d,id) */
typedef struct nx_graph {
  uint32_t n, cap, dim, R0, M_ins, row_u32 /*16*k*/, stride /*bytes, mult of 64*/;
  uint8_t  metric, elem; float alpha2;
  uint32_t *rows;      /* cap*row_u32, 64B-aligned: [count|flags<<16][ids...][pad]; empty slot = 0xFFFFFFFF */
  uint8_t  *vecs;      /* cap*stride, 64B-aligned (elem F32/F16/I8 per quantiser note)                  */
  uint32_t *node2doc;  /* graph node -> segment doc ordinal (identity until reordered)                  */
  uint32_t entry[64]; uint32_t n_entry;
  /* optional hierarchy: uint8_t *lvl; uint32_t *up_off; uint32_t *up_pool; int maxL; uint32_t top; */
  uint8_t *lock;       /* growing segment only: 1-byte spinlock per node (not std::mutex-sized)         */
  _Atomic uint32_t n_pub;  /* nodes visible to readers (release-store after a node is fully linked)     */
} nx_graph;
```
Concurrency contract (growing segment): writer takes `lock[p]`, writes ids with relaxed atomic stores, then publishes `count` with release; readers load `count` with acquire and ids relaxed, skip `0xFFFFFFFF` and ids >= `n_pub`. A reader may see a mix of old/new neighbours during a re-prune - harmless because every id is a valid node [D]. Lock order: always ascending node id when two locks are needed (re-prune of a neighbour holds only that neighbour's lock). Global lock only to raise `maxL`/swap `entry`. Cross-platform wrappers: `nx_aligned_alloc` (`_aligned_malloc`/`aligned_alloc`), `nx_prefetch` (`_mm_prefetch`/`__builtin_prefetch`), atomics via a small shim (MSVC C11 atomics are opt-in) [R].

### 3.4 Search (layer 0, beam-array form; equivalent to HNSW Alg. 2 / Vamana Alg. 1)
```
search(q, ef, k, allowed /*bitmap or NULL*/, budget):
  vis.reset()                                  // bitset + dirty list, below
  L = sorted array (cap ef) of {d,id,expanded}; res = sorted array (cap k) of allowed hits
  seeds = best 1-3 of dist(q, entry[i]) over the entry table      // ~n_entry distance evals
  for s in seeds: vis.set(s); L.insert(d,s); if ok(s) res.insert(d,s)   // ok = allowed && !tomb
  cur = 0
  while ((i = L.first_unexpanded(from=cur)) exists) and visited < budget:
      c = L[i]; L[i].expanded = 1
      row = rows + c.id*row_u32; cnt = row[0] & 0xFFFF
      m = 0
      for j in 1..cnt:  v=row[j]; if (v!=INVALID && !vis.test_and_set(v)) { todo[m++]=v; prefetch(vecs+v*stride, first 2-4 lines) }
      for t in 0..m-1:  d = dist(q, vec(todo[t]));
          if (L.size<ef || d < L.worst) { p = L.insert(d,todo[t]); cur = min(cur,p) }
          if (ok(todo[t]) && (res.size<k || d < res.worst)) res.insert(d,todo[t])
  return res          // if res.size<k after budget: caller falls back to exact scan over `allowed` (see 01-filtered-ann.md)
```
- **Navigation list L is over all nodes, results over allowed non-tombstoned nodes** (FreshDiskANN DeleteList semantics [5]; same shape as hnswlib's `isIdAllowed` [3]). For selective filters the planner raises `ef` and sets `budget`; thresholds belong to `01-filtered-ann.md`.
- ef >= k always. For ef > 256 switch L to two binary heaps (hnswlib form) to avoid O(ef) memmoves [D].
- **Visited set** [D]: `uint64_t bits[ceil(cap/64)]` (1.25 MB at 10M, fits L2/L3) + `uint32_t dirty[]`: `w=bits[i>>6]; if(!w) dirty[nd++]=i>>6; hit=(w>>(i&63))&1; bits[i>>6]=w|(1ull<<(i&63))`; reset = zero the dirty words (cost O(visited)). The hnswlib/classic alternative is a tag array with epoch counter (u16 tag => 20 MB at 10M per thread, memset on wrap [R]); benchmark both.
- **Prefetch**: pass 1 gathers unvisited neighbour ids and issues prefetches for all of them before pass 2 scores; this keeps ~R0 misses in flight (a core sustains ~10-12 outstanding L1 misses [R]), whereas hnswlib prefetches one neighbour ahead [3]. Prefetch only the first 2-4 lines of each vector; the hardware streamer follows. Cache-line aligned 128 B rows give exactly one 2-line fetch per hop [D].
- **Entry table** [D, hypothesis-driven by T2]: at seal, take `n_entry = 32-64` nodes: half the highest in-degree nodes (hubs), half random; add the medoid-ish node (closest to centroid). Seed with the best of the table. A/B against {medoid only, random, HNSW layers} in the harness and ship the winner. For hierarchical mode (d < 32): standard greedy descent with ef = 1 per upper layer [1].
- Distances on cosine data are `1 - dot` on unit vectors; all kernels return `float`, ties broken by id for determinism.

### 3.5 Insert (growing segment, flat mode) and build order
```
insert(vec, doc):
  id = alloc(); store vec/doc; row[id] = empty
  if id == 0: entry = {0}; publish; return
  W  = search(vec, efC, k=efC, no filter)            // keep all evaluated nodes if keep_visited
  sel = robust_prune(id, W, alpha, M_ins)             // list sorted ascending d(p,.)
  write row[id] (under lock[id]); 
  for s in sel: lock(s); if cnt(s) < R0: append(id)
                         else: C = row[s] U {id}; compute d(s,.); row[s] = robust_prune(s, C, alpha, R0); unlock(s)
  publish n_pub = id+1
robust_prune(p, C, alpha2, R): out=[]; for c in C ascending d(p,c), c!=p: if no s in out with alpha2*d(s,c) <= d(p,c): out.push(c); if |out|==R break
```
- Overflow re-prune costs R0+1 distance evaluations around `s`; allowing temporary slack before pruning (DiskANN uses a 1.3 slack factor, recall [R]) amortises it; final prune to R0 at seal.
- Bulk build: shuffle ids, insert first ~1000 sequentially, then T = min(cores-1, 8) threads (insert throughput scaled near-linearly in [5]). Pass 2 (seal): for every node `p` (parallel) re-run `search(p)` + `robust_prune(p, V U N_out(p), 1.2, R0)` as FreshDiskANN/Vamana's second refinement pass [5].
- Exact duplicate vectors (copied files, vendored code, same model in several formats) can fill neighbour lists with zero-distance twins [D]: **collapse exact duplicates at ingest** (hash of vector bytes -> one graph node with a node->docs posting) or cap duplicates per node.

### 3.6 Deletes, consolidation, merge
- Sealed segment delete: `tomb` bitmap bit; query `ok(v) = allowed(v) && !tomb(v)` (a single AND-NOT in the planner). Graph untouched. Compact when `tomb/n > 0.20`.
- **Consolidate** (FreshDiskANN Alg. 4, alpha = 1.2) during merge, one parallel pass over live nodes; only nodes with a deleted out-neighbour do work.
- **Merge(A_big, B_1..B_k)**: (1) consolidate A_big's graph against its tombstones + (2) renumber compactly; (3) insert live vectors of B_i with the batch-parallel insert above (entry table of A_big as seeds); (4) pass-2 refinement only for nodes whose list changed; (5) RCM, new entry table, freeze. Cost scales with |B| not |A| + |B|; expected 6-60% of a full rebuild depending on churn [5, Fig. 11]. Never merge by concatenating adjacency lists (no navigability guarantee) [D].
- Update of a doc = tombstone old + insert new into the growing segment (no in-place replace).
- **In-place mode (optional, v2)** for a purely in-memory mutable index: IP-DiskANN Alg. 5/6 with (ld, k, c, alpha) = (128, 50, 3, 1.2), dangling-edge cleanup at t = 20% [6].
- **Health metric**: `unreachable = #{v != entry : in_degree(v) == 0}` computed by one O(E) pass at seal/merge and exposed in stats/EXPLAIN [7][D]. Sealed segments must report 0.

### 3.7 Frozen segment file (pointer-free, mmap-ready) [D, informed by 3 and 8]
All little-endian; sections 4 KiB-aligned (so `madvise(MADV_RANDOM)` / Windows `PrefetchVirtualMemory` can target them [R]); per-section CRC32C.
```
hdr   : magic "NXGRAPH1", u32 version, u32 hdr_size, u32 n, u32 dim, u8 metric, u8 elem, u8 flags(hier|reordered|has_codes), u8 pad,
        u32 R0, u32 row_u32, u32 stride, f32 alpha, u32 n_entry, u32 n_sections, u64 build_seed, f32 ef_for_recall[3] /*0.90,0.95,0.99*/
dir   : n_sections x { u32 id, u64 off, u64 len, u32 crc32c }   ids: ENTRY u32[n_entry] | ROWS u32[n*row_u32] | VECS u8[n*stride] |
        CODES (quantised, optional) | NODE2DOC u32[n] | UP_OFF/UP_POOL (if hier) | TOMBS (roaring, may live in the segment's tombstone file)
```
No pointers, upper-layer lists addressed by index (`up_off`), so open = `mmap` + header check; graph rows and vectors are used in place. Optional huge pages for RAM-resident copies: the TLB miss rate fell 3.85% -> 2.14% after reordering [8], so TLB is a real cost at 10M [R: Linux `MADV_HUGEPAGE`, Windows large pages need a privilege].
**Footprint [D]** (R0 = 31): graph 128 B/node (1.28 GB at 10M), node2doc 4 B/node, visited bitset 1.25 MB/thread at 10M. Vectors dominate: 384-d fp32 = 1536 B/node -> 15.4 GB at 10M (does not fit this 15 GB box) vs SQ8 384 B -> 3.84 GB; traversal-stage total at 10M with SQ8 about 5.2 GB. FreshDiskANN's 25-32 B PQ codes/point shows the lower bound if codes drive traversal [5]. **Test scale on this machine (3 GB free disk): <= 2M x 128-d; extrapolate to 10M with the per-node formula.**
- **Reorder at seal**: build the symmetrised adjacency, run RCM (BFS from a min-degree peripheral node, neighbours in ascending degree, reverse the order), permute `ROWS`, `VECS`, `CODES`, `NODE2DOC`, rewrite ids inside rows, remap entry table. Prefer also renumbering the segment's **doc ordinals** to the graph order so bitmap tests need no `node2doc` hop (conflict with index-sort locality for postings: decide per segment [D]). Gorder (30% P99 vs RCM 17% [8]) is a later upgrade.
- **Calibration**: at seal, run ~256 leave-in self-queries (blocked brute force for n <= 1M, else a proxy truth from ef = 8*efC) and store the smallest ef reaching recall 0.90/0.95/0.99 in the header; the planner maps a `recall:` knob to ef. Same idea as the paper guidance "tune efC on a sample to reach ~0.95" [1][2].

### 3.8 Implementation order
1. Distance kernels + flat brute force + `nx_graph` arrays + validate(). 2. Single-thread insert/search (flat, alpha = 1) with recall harness. 3. Bitset visited + two-pass prefetch. 4. Multi-thread insert with spinlocks. 5. Seal: pass 2 (alpha = 1.2), entry table, calibration, frozen file + mmap load. 6. Tombstones + navigate-through semantics + allowed-bitmap hook. 7. Consolidation + merge. 8. RCM reorder. 9. Optional hierarchy for d < 32. 10. Optional IP-DiskANN in-place mode, Gorder, hub-aware entries.

## 4. Skip and why
- **Hierarchy by default** (d >= 64): no latency/recall gain [4]; keep as a flag for d < 32 [1][4].
- **hnswlib replace_deleted / updatePoint / markDelete-forever**: unreachable points, 5-10x slower than a query, quality not recoverable by ef [7]; our LSM design makes it unnecessary.
- **MN-RU and its backup index**: a patch to a mechanism we do not ship. Keep only the unreachable-count diagnostic.
- **FreshDiskANN SSD LTI / PQ-on-SSD / StreamingMerge block phases**: built for 1B points under 128 GB RAM [5]; irrelevant at <= 10M in one process with 3 GB spare disk. We keep the *ideas* (DeleteList, alpha = 1.2 consolidation, temp-index + periodic merge).
- **IP-DiskANN as v1 default**: needs search-per-delete (ld = 128) and is slower than FreshDiskANN consolidation below ~10M vectors [6]; sealed immutable segments never need it.
- **Gorder and lightweight degree orderings in v1**: Gorder is O(sum deg^2) and more code; degree-based orders do nothing for k-NN graphs [8].
- **SPFresh / IVF-style incremental partitions**: not graph-based; belongs to the IVF/quantisation note [R].
- **Learned/adaptive termination, GPU graphs (CAGRA)**: out of scope on this box; not researched.

## 5. Novel ideas specific to NexusSearch
1. **Bitmap-seeded entry for filtered search** [D, untested]: when the allowed set is small, seed the beam with the best of ~32-64 random members of the allowed bitmap (rank/select sampling) instead of the global entry table, so the search starts inside the filtered region; hubs on the "highway" [4] may be filtered out and useless for restrictive predicates.
2. **Tombstone == filter**: one code path; the graph never learns about deletes; merge consolidation is the only structural delete.
3. **Hub-aware entry table and hub-protected pruning** [D, hypothesis]: pick entry candidates by in-degree/visit counts (the paper shows queries hit hubs in the first 5-10% of steps [4]); optionally exempt top-1% hubs' inbound edges from eviction during re-prune. Measure with per-node visit counters in a debug build.
4. **Query-traffic-learned entry table**: record the first 10% of expansions of sampled queries; refresh the entry table at merge time from the most frequently hit nodes (fits sealed immutability).
5. **Self-calibrated recall knob per segment** (ef_for_recall table in the header), surfaced in EXPLAIN as `ef=96 (recall~0.95 est.)`.
6. **Reverse ANN for WATCH/percolator**: index *subscription* vectors (semantic: clauses) in a small graph; each newly indexed doc vector is the *query* against it; O(log)-ish per doc for 100k subscriptions. Rebuild-by-segment is cheap because subscriptions change slowly.
7. **Exact-duplicate collapse** (hash -> canonical node with doc posting) as a first-class graph feature, relevant to file/code/model corpora.
8. **Graph order == doc order** per segment, so bitmap membership, column reads and vectors share locality (needs a policy for segments with several vector fields: primary field decides).
9. **Insert-time budget classes**: during an indexing storm lower `efC` to 100 on the growing segment and let pass 2 at seal repair quality.

## 6. Validation / test ideas (no downloads; synthetic + bundled sklearn digits 1797x64)
- **Oracle**: brute-force exact kNN; report recall@k (k = 1, 10, 100) vs ef curves, p50/p99 latency, distance computations/query. Datasets: Gaussian d = 8/32/128/384 (L2 and cosine), Gaussian-mixture clustered (64 clusters), 10-30% exact duplicates, hub-heavy (L2 d = 960 random), digits, 384-d random projections of digits.
- **Structural invariants** (`nx_graph_validate`) after every N ops: ids < n, no self loop, no duplicate in row, count <= R0, entry valid, node2doc bijective, **unreachable == 0** for sealed, BFS reachability from entry table covers >= 99.9% of live nodes.
- **Churn test à la FreshDiskANN Fig. 2**: 50 cycles of delete+reinsert of 5/10/50% (growing -> seal -> merge pipeline); assert recall at fixed ef within 1 pt of cycle 0, average degree not shrinking > 10% (the sparsity symptom [5]); alpha = 1.0 vs 1.2 contrast must show the effect.
- **Runbooks à la IP-DiskANN** (scaled to 100k-1M): SlidingWindow (Tmax = 200, delete after Tmax/2), ExpirationTime (lifetimes 100/50/10 in ratio 1:2:10), Clustered (64 k-means clusters, 5 rounds) [6]; track recall@10, distance comps, QPS per step.
- **hnswlib failure reproduction**: implement a replace-in-place toy and show the unreachable growth, so the guard metric is proven to detect it [7].
- **Concurrency**: T writer + T reader stress, no sanitizers available -> invariant checks, canary words, randomised yield injection (`NX_TEST_YIELD()`), results valid ids, post-quiescence recall within 0.5 pt of single-thread build.
- **Persistence**: build -> save -> mmap -> byte-identical results; flip-a-byte/truncation/version-mismatch tests (CRC and header gating); open must be O(1) in n.
- **Merge**: merge(A,B) vs full rebuild: recall within 1 pt at same ef, time ratio logged (reference range 6-62% [5]); with 20% tombstones in A, recall and unreachable == 0.
- **Reorder**: results identical modulo id mapping before/after RCM; QPS gain on 2M x 128-d expected ~10-40% [8] (report p99 too); TLB/cache counters not available on Windows -> use QPS and `perf`-less timing only.
- **Flat vs hierarchical A/B** on d = 8/16/32/64/128/384 (confirms the d < 32 rule on our own data) and entry-table variants (3.4).
- **Calibration check**: `ef_for_recall[0.95]` achieves 0.95 +/- 0.01 on held-out queries.
- **Kernels**: AVX2 vs scalar within 1e-4 relative, dims not multiples of 8, unaligned inputs, denormals; NaN/zero-vector handling in cosine.

## 7. References
[1] Malkov, Yashunin. Efficient and robust ANN search using Hierarchical Navigable Small World graphs. arXiv 1603.09320 (2016, rev. 2018). https://arxiv.org/abs/1603.09320 (full PDF text read: Alg. 1-5, Sec. 4.1, 4.2.3)
[2] hnswlib ALGO_PARAMS.md. https://github.com/nmslib/hnswlib/blob/master/ALGO_PARAMS.md (raw fetched)
[3] hnswlib `hnswalg.h`. https://github.com/nmslib/hnswlib/blob/master/hnswlib/hnswalg.h (raw fetched, read via summarising fetch [F-src])
[4] Munyampirwa, Lakshman, Coleman. Down with the Hierarchy: The 'H' in HNSW Stands for "Hubs". arXiv 2412.01940 (v3, 2025). https://arxiv.org/abs/2412.01940 (full PDF text read)
[5] Singh, Jayaram Subramanya, Krishnaswamy, Simhadri. FreshDiskANN: A Fast and Accurate Graph-Based ANN Index for Streaming Similarity Search. arXiv 2105.09613 (2021). https://arxiv.org/abs/2105.09613 (full PDF text read)
[6] Xu, Manohar, Bernstein, Chandramouli, Wen, Simhadri. In-Place Updates of a Graph Index for Streaming ANN Search (IP-DiskANN). arXiv 2502.13826 (2025). https://arxiv.org/abs/2502.13826 (full PDF text read)
[7] Xiao, Zhan, Xi, Hou, Liao. Enhancing HNSW Index for Real-Time Updates: Addressing Unreachable Points and Performance Degradation. arXiv 2407.07871 (2024). https://arxiv.org/abs/2407.07871 (full PDF text read)
[8] Coleman, Segarra, Smola, Shrivastava. Graph Reordering for Cache-Efficient Near Neighbor Search. arXiv 2104.03221 (2021). https://arxiv.org/abs/2104.03221 (full PDF text read)
Recall-only, not fetched, no URL verified: [R] DiskANN (Jayaram Subramanya et al., NeurIPS 2019); NSG; HCNNG; ParlayANN (Dobson et al., 2023, cited in [4]); Lin & Zhao 2019 (cited in [4]); SPFresh (SOSP 2023, cited in [6]); Gorder (Wei et al., SIGMOD 2016, cited in [8]); Lucene merge issues (cited in [6]); DiskANN graph slack factor 1.3; `vl_type = unsigned short` in hnswlib's visited list; microarchitectural prefetch/LFB figures; Windows/Linux paging APIs.
Companion notes: `01-filtered-ann.md` (filtered traversal planner, ACORN), `05-bitmaps-columns-dicts.md`.
