# 04 - Lexical ranking: BM25, dynamic pruning, positional queries, learned sparse retrieval

Scope: how NexusSearch scores and top-k-retrieves text (BM25/BM25F), prunes (MaxScore, Block-Max WAND, VBMW, BMP, Seismic),
tokenises prose and code, answers phrase/proximity queries, computes exact idf across segments with tombstones, and hosts
learned-sparse (SPLADE-style) fields. Date of research: 2026-10-01/02. Hardware target: x86-64 AVX2/FMA/BMI2/POPCNT, no AVX-512; C11; `long` is 32-bit on Windows.

Evidence tags: **[F]** read primary source text this session (fetched page, or the paper PDF text); **[F-abs]** only the abstract/landing page was read;
**[C]** verified indirectly through the reference list of a paper I did read (DOI/title/venue are copied from that list, the cited paper itself was NOT read);
**[R]** recall-only (I could not fetch a source - treat as a hypothesis to verify); **[D]** my own derivation/design for NexusSearch, not a published claim.
Correction logged: my first recollection that arXiv 1908.10598 was Mallia et al.'s experimental study was wrong - it is the Pibiri-Venturini compression survey. That ID is cited only as the survey.

---------------------------------------------------------------------------------------------------

## 1. Landscape

| System / paper | What it does | Relevance |
|---|---|---|
| **Lucene 8+ / Elasticsearch / OpenSearch** | BM25 default since 2015 (added 2011); WAND added end of 2017; block-max indexes + Block-Max WAND shipped in Lucene 8.0, March 2019; stores (tf, doc-length) pairs per block, not scores [F: Grand et al. ECIR 2020] | The production reference. 3-7x on term queries, up to 15x on disjunctions [F: Elastic blog]. Hit count becomes a lower bound (`gte`). |
| **PISA / DS2I** (C++) | Research engines: MaxScore, WAND, BMW (block 128 default), VBMW (lambda=12), PEF/BIC/SVB compression [F via RISE] | Best open C++ reference for pruning code; BMW block was 40 postings in the BMP paper's learned-sparse runs [F]. |
| **RISE** (Rust, arXiv 2606.07187, Jun 2026) | Re-implements EF/PEF/BIC/StreamVByte + WAND/MaxScore/BMW/BMMS (static and variable blocks); up to 2x faster than PISA on BMW/BMMS [F] | The freshest apples-to-apples numbers for algorithm choice by query length (section 2.4). |
| **Tantivy** (Rust, Quickwit) | Lucene-like library, block-wand over 128-doc blocks [R; one raw-source fetch returned 404, so no layout verified] | Worth reading source before freezing the block format. |
| **Anserini/Pyserini** | Lucene-based research toolkit; BM25 defaults k1=0.9, b=0.4 [F: Kamphuis] | Source of the "k1=0.9,b=0.4" convention vs Lucene's 1.2/0.75. |
| **BM25S** (Lu, 2024) | Eager scoring into a CSC sparse matrix; up to 500x vs rank_bm25 and higher QPS than Elasticsearch on all 15 BEIR sets in its Table 1 (e.g. 574 vs 14 QPS on ArguAna, 12.2 vs 11.9 on MS MARCO) [F] | Shows the idea of baking scores into the index; also gives clean BEIR ablations on stemming/stopwords/k1/b. |
| **Kamphuis et al. ECIR 2020** | Eight BM25 variants, no significant effectiveness differences, including Lucene's lossy 1-byte length [F] | Justifies picking the Lucene variant for engineering reasons. |
| **SPLADE-v3** (Naver, 2024), inference-free sparse (OpenSearch authors, 2024/25), ELSER (Elastic) | Learned sparse retrieval: neural term expansion + weights into an inverted index | Query-side inference cost is the issue; inference-free variants avoid it. ELSER internals [R]. |
| **Seismic** (Bruch et al., SIGIR 2024), **BMP** (Mallia-Suel-Tonellotto, SIGIR 2024), **Superblock Pruning** (Carlson et al., SIGIR 2025) | Pruning designed for learned sparse (long queries, skewed weights) | Classic BMW/WAND collapse on SPLADE-length queries [F: BMW 614 ms vs MaxScore 121 ms]. |
| **IOQP/JASS** (impact-ordered, score-at-a-time) | Anytime query processing, nearly flat latency in k | Slowest of the compared inverted-index methods in Seismic's tables but the natural "deadline" engine [F]. |

---------------------------------------------------------------------------------------------------

## 2. Techniques

### 2.1 BM25 variants and parameters

**Formula (Lucene variant, what to implement)** [F: Kamphuis Table 1; Lucene BM25Similarity docs]:
`score(q,d) = sum_t idf(t) * tf / (tf + k1*(1 - b + b*dl/avgdl))`, `idf(t) = ln(1 + (N - df + 0.5)/(df + 0.5)) = ln(N+1) - ln(df+0.5)` [D: algebra].
The `+1` inside the log keeps idf > 0, so per-term scores are non-negative and bounded by `idf` (tf/(tf+K) < 1). That boundedness is exactly what let Lucene add WAND without
changing its index format [F: Grand et al.]. Lucene docs: default **k1 = 1.2, b = 0.75, discountOverlaps = true**; stats use `CollectionStatistics.docCount()` (docs having the field), not `numDocs()` [F].
Lucene compresses doc length to **one byte** so `K = k1*(1-b+b*dl/avgdl)` can be pre-tabulated for 256 values; Kamphuis found no significant AP/P@30 difference vs exact lengths (Robust04 AP .2531 vs .2533) [F].

**Parameter evidence.** Kamphuis used k1=0.9, b=0.4 (Anserini defaults) on Robust04/Core17/Core18; all variants within ~0.01 AP [F]. BM25S Table 3 (BEIR, Lucene variant, own tokenizer) [F]:
k1=1.2/b=0.75 avg nDCG@10 **39.9**; k1=1.5/b=0.75 **39.7**; k1=0.9/b=0.4 **41.1**; but per-dataset swings are huge (ArguAna 48.7 -> 40.8; FEVER 50.3 -> 63.8; Touche 33.2 -> 44.2).
Conclusion: no universal default; ship Lucene's 1.2/0.75 (users expect it, ES-compatible) and make k1/b per-field and auto-tunable from a few judged queries. Variants BM25L (delta 0.5), BM25+, BM25-adpt, TFl-delta-p give no significant gains; BM25-adpt's per-term k1 optimum was undefined for ~90% of terms [F].

**BM25F / multi-field.** Robertson-Zaragoza-Taylor CIKM 2004 (combine per-field tf with per-field length normalisation *before* saturation) [R]. Simple alternative: sum of per-field BM25 with boosts (no cross-field saturation). Per-field sum keeps UBs additive and is a pure DAAT list-per-(field,term). See Adopt step 8.

**Eager/impact scoring (BM25S, PISA impact quantisation).** BM25S precomputes `tf*idf/(tf+K)` per posting into a sparse matrix; for BM25L/BM25+ it subtracts the non-occurrence score S_theta(t) and adds it back per query [F]. Top-k via argpartition O(n) [F]. **Pitfall for us**: baked scores freeze idf/avgdl, which change on every segment add/merge. Use only after a force-merge, or never (section 4).

### 2.2 Dynamic pruning (document-at-a-time)

All of these need per-list *upper bounds* on the score contribution; `theta` is the current k-th best score.

- **MaxScore** (Turtle & Flood, IP&M 1995 [C: doi 10.1016/0306-4573(95)00020-H]). Sort lists by term UB ascending; prefix sums `pre[i]`; lists with `pre[i] <= theta` are *non-essential* (a doc present only in them cannot enter the top-k); iterate only the union of essential lists, then probe non-essential lists in descending UB order with an early break.
- **WAND** (Broder et al., CIKM 2003 [C: doi 10.1145/956863.956944]). Sort lists by current doc; pivot = first list where the UB prefix sum exceeds `theta`; skip everything before the pivot doc.
- **Block-Max WAND** (Ding & Suel, SIGIR 2011 [C: doi 10.1145/2009916.2010048]): add per-block max scores to refine the pivot test; skip whole blocks. Original reports 18x (TREC05) / 8x (TREC06) vs naive exhaustive OR; Lucene's optimised exhaustive OR leaves ~3x [F: Grand Table 1: Lucene exhaustive 98/188 ms vs BMW 32/55 ms, k=10].
- **VBMW** (Mallia et al., SIGIR 2017 [C: doi 10.1145/3077136.3080780]): variable-size blocks chosen by dynamic programming to minimise bound looseness plus metadata; fixed cost lambda per block. Best settings: static block **128** postings or **lambda = 12** [F via RISE]. Variable beats static on both BMW and BMMS [F].
- **Block-Max MaxScore (BMMS)** with Lucene-style double windowing: outer window from block boundaries drives the essential split; inner fixed windows score all docs into an accumulator **without touching the heap**, updating `theta` only at window end [F: RISE]. Amortises heap cost on dense/long queries.

**Numbers to plan against** (RISE Table 2/4, ClueWeb09-B 50M docs, 15.9B postings, 1 thread, k=10, StreamVByte, RGB-reordered, ms/query) [F]:
exhaustive R-OR 72.9; WAND 9.7; MaxScore 6.8; BMW static 3.8 / **variable 3.2**; BMMS static 4.6 / variable 3.9. With PEF: MaxScore 10.9, BMW-var 3.7. With BIC: MaxScore 18.8, BMW-var 10.1.
**Query-length effect** (CCNews, SVB): 2-term queries BMW ~1.5 ms vs MaxScore ~6 ms; MaxScore is flattest (4.5 ms at 1 term -> 17.5 ms at 9+); MaxScore overtakes block-max methods past **5-6 terms** (17.5 vs 32 ms at 9+ terms) [F].
**k effect** (Lucene via Anserini, ClueWeb09b): BMW speedup over exhaustive OR 3.4x at k=10, 2.7x at k=100, 1.4x at k=1000 [F]. Indexing time +5.7% to +7.6% for the (tf,dl) pair metadata [F].

**Hit-count and aggregation caveats** [F]: with pruning the total match count is a lower bound (Lucene `hits.total.relation = gte`, `track_total_hits` threshold); pruning is disabled when facets/aggregations must see every match. Lucene also requires non-negative scores [F: LUCENE-7996 https://issues.apache.org/jira/browse/LUCENE-7996].

**Lucene's flexible block bound** [F]: store per block the *Pareto frontier* of (tf, dl) pairs (drop (tf_j, dl_j) if some (tf_i, dl_i) has tf_i >= tf_j and dl_i <= dl_j), organised in skip levels over groups of 8, 64, 512, 4096 blocks. Bounds are computed at query time from the pairs, so the scoring function (and global idf/avgdl) can change without reindexing. Macdonald & Tonellotto ICTIR 2017 give tight approximate bounds the same way [C].

### 2.3 Score-at-a-time and impact ordering
IOQP/JASS process postings in decreasing impact order and stop after a budget (fraction of postings). On SPLADE/MS MARCO exact IOQP costs ~79-81 ms regardless of k (k=10/100/1000) but with 1%/5%/10% budgets RR@10 is 23.96/32.22/34.61 vs 38.11 exhaustive at 1.4/3.1/5.3 ms [F: BMP Table 2/3]. Robust to long queries, poor at low latency-vs-quality. Keep as a "deadline" fallback idea only.

### 2.4 Posting-list compression (for the block payload)
Survey numbers (Pibiri & Venturini, ACM CSUR 2020; arXiv 1908.10598) [F], bits/int and ns/int on Gov2 / ClueWeb09 / CCNews:
PEF 3.1/5.0/5.5 bits at 0.76/1.10/1.31 ns; BIC 2.9/4.4/5.2 bits but 5.1-7.0 ns; QMX 5.1/7.3/7.4 bits at 0.8-0.87 ns; Opt-VByte 3.9/5.7/6.4 bits at 0.7-0.9 ns; VByte 8.8-9.3 bits at ~1.0 ns; Roaring 6.6-9.8 bits at 0.5-0.7 ns. SIMD-BP128 = 16 blocks of 128 ints aligned to 128-bit boundaries [F].
RISE bits/int on CW09: SVB 11.7, EF 5.0, PEF 3.7, BIC 3.5 [F]. **Document reordering** (recursive graph bisection, Dhulipala et al. KDD 2016 [C]) shrank PEF by 10% (CW09, 15.24 -> 13.74 GiB) and 36% (CCNews, 18.86 -> 12.02 GiB), with no query slowdown [F]; it also makes block maxima tighter [F: BMP].
Sibling note 05 already specifies BP128 + StreamVByte tail; this note only adds the *metadata* and *algorithms* on top.

### 2.5 Learned sparse retrieval and its pruning

- **SPLADE-v3** (Lassance, Dejean, Formal, Clinchant; arXiv 2403.06789, Mar 2024) [F]: MS MARCO dev MRR@10 **40.2**, BEIR-13 mean nDCG@10 **51.7** (SPLADE++SD 50.7). Variants: DistilBERT 38.7 / 50.0; **Lexical** (no query expansion) 40.0 / 49.1, FLOPS 0.6; **Doc** (no query-side computation, query = bag of words) 37.8 / 47.0. BM25 sits near 40-42 on a comparable BEIR subset (BM25S Table 3, 14-15 sets, not identical) so SPLADE-v3 is roughly +10 nDCG points on BEIR (rough, different subsets) at the price of much larger indexes and 40+ term queries: SPLADE docs have ~119 non-zeros and queries ~43 on MS MARCO (E-SPLADE 181/5.9; uniCoil-T5 68/6) [F: Seismic] => ~1.0e9 postings for 8.8M passages (8.8M x 119) [D], i.e. several times a BM25-style index of the same passages (my rough guess ~2-3e8, unverified).
- **Inference-free sparse** (Geng, Wang, Ru, Yang; arXiv 2411.04403, v2 Jul 2025) [F-abs]: only the document side is encoded; the query is a bag of tokens with learned IDF-like weights; IDF-aware penalty + heterogeneous ensemble distillation; **+3.3 nDCG@10** over the previous inference-free SOTA on BEIR, client latency **1.1x BM25**. This is the model class that fits a zero-dependency local-first core: the engine needs no query encoder.
- **Why classic pruning fails**: SPLADE queries are long (expansion), so BMW costs 614 ms at k=10 vs MaxScore 121 ms and IOQP 79 ms; BMP exact 10.5 ms (b=32) / 11.0 (b=16) / 15.0 (b=8); at k=1000 BMP-b=8 27.6 ms [F: BMP Table 2].
- **BMP** (arXiv 2405.01117, SIGIR 2024) [F]: docIDs in BP order; split doc-id space into blocks of b docs; per term store an array of ceil(n/b) block-max impacts (u8, sparse representation, compressed); upper bound per block = sum_t w_q(t) * bm_t[block] (SIMD); sort blocks by UB (partial counting sort above a threshold seeded by a **single-term-quantile top-k estimator**); evaluate a block with a hybrid structure (per-block inverted lists with log2(b)-bit local doc ids + sorted term table, accumulate into b accumulators); stop when `theta >= alpha * UB(next block)`. Optional query-term pruning parameter beta. Exact mode alpha=1: **2.9-7.5x faster than the closest competitor on SPLADE**, ~2x+ on E-SPLADE/uniCoil [F]. Approximate on SPLADE: b=64, alpha=0.85 -> 8.1 ms RR@10 38.05; with raw (uncompressed) BM index b=64, alpha=0.75 -> **2.7 ms, RR@10 37.45**, alpha=0.65 -> 1.6 ms, 35.01 (exhaustive 38.11) [F]. Space (SPLADE MS MARCO, GB): forward index 7.1/6.0/5.1/4.3/3.7/3.3 and block-max index raw 30.0/15.0/7.4/3.7/1.9/0.9 (compressed 5.5/4.1/3.0/2.3/1.7/1.2) for b = 8/16/32/64/128/256 [F]. Source https://github.com/pisa-engine/BMP.
- **Seismic** (arXiv 2404.18812, SIGIR 2024) [F]: observation "concentration of importance": top-10 query entries hold 0.75 of L1 mass, top-50 doc entries (~30% of non-zeros) hold 0.75. Index: per coordinate keep top-lambda postings (static pruning); cluster them into <= beta blocks with **shallow k-means** (beta random doc vectors as centroids, assign by max inner product); per block a summary = coordinate-wise max vector, kept to its alpha-L1-mass sub-vector, **8-bit scalar quantised** (4x smaller). Query: keep top-`cut` query coordinates; per list scan blocks, skip a block if `<q, summary> < heap.min / heap_factor`; otherwise fetch from a forward index and compute exact inner products. Best on MS MARCO/SPLADE: **lambda=6000, beta=400, alpha=0.4**; NQ: lambda=5250, beta=525, alpha=0.5; query grid cut in 1..10, heap_factor in {0.7,0.8,0.9,1.0}. Mean latency **187 us at 90% accuracy, 531 us at 97%** (SPLADE, one i9-9900K thread) vs exact PISA 100,325 us; 2.6-21.6x faster than graph methods; MS MARCO index 6,416 MiB, 5 min to build (graph methods 137-267 min) [F]. Code https://github.com/TusKANNy/seismic.
- **Dynamic Superblock Pruning** (arXiv 2504.17045, SIGIR 2025) [F-abs]: hierarchy of document blocks grouped into superblocks, prune at superblock level first; generalises BMP/flat clustering; both safe and approximate modes. No numbers read.

### 2.6 Positional indexes, phrase and proximity [R unless noted; algorithms are standard]
Layout: separate positions stream; per posting, positions are delta-coded within the doc; per 128-posting block keep `pos_off`. Evaluate only candidates that survive the conjunction.
Exact phrase: leapfrog conjunction driven by the rarest term, then for each candidate decode position lists and intersect offset-adjusted positions (`p_i - i`), smallest list first with galloping; phrase frequency `pf` = number of hits, scored with the BM25 tf slot (Lucene-style; idf of the phrase = sum of term idfs [R]).
**Safe bound for phrases** [D]: since `pf_d <= tf_{i,d}` for every term i and the tf-part of BM25 is non-decreasing in tf at fixed dl, `tfpart_phrase <= min_i h_i` where `h_i` is the block's tf-part bound of term i; phrase UB = `sum_i idf_i * min_i h_i`.
Proximity (NEAR/n, unordered window): multi-way merge of position lists with a min-heap and sliding window; sloppy-phrase weight like 1/(1+distance) [R].
Do **not** drop stopwords from the positional field, or "to be or not to be" becomes unanswerable.

### 2.7 Tokenisation and analysis
- BM25S ablation, BEIR mean nDCG@10 (k1=1.5, b=0.75, regex `(?u)\b\w\w+\b`) [F]: English stopwords+Snowball **39.7**; stopwords only 38.4; Snowball only **39.6**; neither 38.3. So **stemming ~ +1.3 points, stopword removal ~ +/-0.1** (large per-dataset effect on TREC-COVID/ArguAna). BM25S pairs Scikit-style splitting with Elastic's stoplist and the C Snowball stemmer [F].
- Unicode word boundaries = UAX #29 [R], NFKC + case folding [R]. CJK needs bigram or dictionary segmentation (Lucene's CJK analyzer = overlapping bigrams [R]).
- Code identifiers: Lucene's WordDelimiterGraphFilter generates word parts, numbers and the catenated/original token at the same position (flags GENERATE_WORD_PARTS / PRESERVE_ORIGINAL / CATENATE_*) [R]. Our own rules in Adopt step 6.

### 2.8 Exact idf over segments and tombstones
Elasticsearch scores per shard by default; identical docs get different scores depending on shard; `dfs_query_then_fetch` first collects global term stats ("same as one shard", one extra round trip) [F: Elastic blog]. In a single-process engine we get global stats for free: sum per-segment `df_s(t)`, `docCount_s(field)`, `sumDL_s(field)` at query time (a dictionary lookup per segment, already needed to open the list).
Tombstones: Lucene's documented API says `docCount` replaces `numDocs()` for consistency with term stats [F]; whether deleted docs stay in df/ttf until a merge is the commonly-cited behaviour [R]. Analysis [D]: `idf = ln(N+1) - ln(df+0.5)`, so d idf/d df = -1/(df+0.5). If a fraction rho of docs is deleted *uniformly*, both N and df are inflated by 1/(1-rho) and the ratio-driven idf barely moves; error appears only under *non-uniform* deletion (e.g. one directory removed): a term with 98 stale + 2 live postings has idf error ln(100/2) = 3.9 nats. Design in Adopt step 5.

---------------------------------------------------------------------------------------------------

## 3. Adopt for NexusSearch (ordered work plan)

Cross-reference: posting block payload (BP128 + StreamVByte tail, 128 docs) is in `05-bitmaps-columns-dicts.md` section 3.4; this note replaces its skip-entry fields.

1. **BM25 core (Lucene variant), per-field.** `idf = log1pf((N - df + .5f)/(df + .5f))`; `K[code] = k1*((1-b) + b*len_decode[code]/avgdl)` as a 256-entry float LUT built per (field, query) from *global* avgdl; `contrib = idf*boost*tf/(tf+K[norm[doc]])`. Defaults **k1=1.2, b=0.75**, per-field override; `discount_overlaps=true` (position-increment-0 tokens, e.g. catenated code identifiers, do not count toward length). One byte per doc per field in a column `uint8 norm[maxdoc]` (length code: exact for small lengths, ~4-bit mantissa above, 256-entry decode table; exact thresholds are ours [D], justified by Kamphuis' no-difference result). Fixed summation order (by list id) everywhere so pruned and exhaustive paths give **bit-identical** scores.
2. **Skip metadata that stays valid under changing global stats** (12 B per 128-posting block, same size as sibling note's entry):
   ```c
   typedef struct nx_skip {       // one per 128 postings; array contiguous, 4-byte aligned
     uint32_t last_docid;         // largest segment-local docid in the block
     uint32_t byte_off;           // offset of block header (u8 bits_doc,u8 bits_tf,payload) from list base
     uint8_t  tfA, nrmA;          // competitive pair A: every doc in block has (tf<=tfA && norm>=nrmA) ...
     uint8_t  tfB, nrmB;          // ... or (tf<=tfB && norm>=nrmB); tf saturates at 255 => treat as infinity
   } nx_skip;                     // static_assert(sizeof==12)
   typedef struct nx_skip2 {      // one per 32 skips (4096 postings): same pairs, covers all children
     uint32_t last_docid; uint32_t first_skip; uint8_t tfA,nrmA,tfB,nrmB; } nx_skip2;
   ```
   Term dictionary entry additionally stores the list-level pairs (A,B) and `df`, `ttf`. Block bound at query time: `ub = idf*boost*max(tfA/(tfA+K[nrmA]), tfB/(tfB+K[nrmB]))` (2 divisions, cacheable per block per query). Build: when sealing a block, compute the Pareto frontier of (tf, norm) and collapse to <= 2 pairs conservatively (round tf up, norm down). Lists with df < 128 have no skip entries (list-level pair only). This is Lucene's idea [F] with a 2-pair cap and a 2-level skip instead of 4+ levels [D].
3. **Scorer interface** (Lucene-style shallow/lookahead API names are recall [R]; the interface is ours):
   ```c
   typedef struct nx_scorer nx_scorer;
   struct nx_scorer { uint32_t doc;                       // current doc or NX_NO_MORE
     uint32_t (*advance)(nx_scorer*, uint32_t target);    // first doc >= target
     float    (*score)(nx_scorer*);                       // exact, for current doc
     uint32_t (*shallow)(nx_scorer*, uint32_t target);    // move block cursor only, return last doc of that block
     float    (*block_ub)(nx_scorer*);                    // UB for docs in block found by shallow()
     float    term_ub; };                                 // list-level UB
   ```
   A structured filter (Roaring bitmap from the per-clause result sets) is wrapped as a **required, zero-score** scorer so it participates in leapfrogging; clauses that must yield bitmaps (under OR/NOT of a larger tree) fall back to an exhaustive docid-set producer (no pruning, no scores).
4. **Algorithms, in this order**: (a) exhaustive OR + AND with block skipping (reference, also the correctness oracle); (b) **MaxScore**; (c) **BMW** (pseudocode below); (d) BMMS with inner windows W=4096 docs; (e) VBMW builder at merge time (lambda=12, static fallback 128). **Auto-selection** by number of scoring lists m (after fuzzy/synonym/field fan-out): `m<=4 -> BMW; 5<=m<=8 -> BMMS; m>=9 -> MaxScore`; if `k>=1000` use MaxScore/BMMS for m>=3 (BMW gain shrinks to 1.4x [F]). Planner exposes the choice and the counters in EXPLAIN ANALYZE.
   **Threshold seeding** [D, idea from BMP's quantile estimator [F]]: before traversal, fully score the top-UB block(s) of the highest-UB term until k docs are scored; set `theta0 = nextafterf(kth_score, -INFINITY)` (one ulp lower so that docs *equal* to it can still be found by the normal pass). Safe because scores are non-negative sums of real documents.
   ```
   BMW(L[0..m), k, theta0):                      // docids ascending, ties resolved by smaller docid
     heap = minheap(k); theta = theta0
     loop:
       sort L by L[i].doc                        // insertion sort, m small
       s = 0; p = -1
       for i in 0..m-1: s += L[i].term_ub; if s > theta { p = i; break }   // strict >: equal scores cannot enter
       if p < 0 or L[p].doc == NO_MORE: break
       d = L[p].doc; while p+1<m and L[p+1].doc == d: p++
       bs = 0; for i in 0..p: L[i].shallow(d); bs += L[i].block_ub()      // metadata only, no decode
       if bs > theta:
         if L[0].doc == d:                       // all of L[0..p] are positioned at d
           sc = sum_{i<=p} L[i].score()          // plus any L[j>p] at d are already included via p++
           if sc > theta: heap.push(sc,d); if heap.full: theta = heap.min
           for i in 0..p: L[i].advance(d+1)
         else: pick i<p with L[i].doc < d and max term_ub; L[i].advance(d)
       else:                                     // cannot beat theta inside current blocks
         nd = min_{i<=p}(block_last_i) + 1; if p+1<m: nd = min(nd, L[p+1].doc)
         pick i<=p with L[i].doc < nd (largest idf); L[i].advance(nd)
   ```
   ```
   MaxScore(L[0..m), k):                         // L sorted by term_ub ascending, pre[i]=sum_{j<=i} term_ub_j
     ne = 0
     loop:
       while ne<m and pre[ne] <= theta: ne++     // non-essential prefix grows as theta rises
       if ne==m: break
       d = min_{i>=ne} L[i].doc; if d==NO_MORE: break
       sc = sum_{i>=ne, L[i].doc==d} L[i].score(); advance those past d
       for i = ne-1 downto 0:
          if sc + pre[i] <= theta: break
          L[i].advance(d); if L[i].doc==d: sc += L[i].score()
       if sc > theta: push(sc,d); theta = heap.min if full
   ```
   Pitfalls: UBs must be >= the float actually produced (multiply UB by `1+2^-20`, or compute both with the same code path); never prune with `>=`; re-sort after advances; strict handling of NaN (reject boosts <= 0 to keep scores non-negative [F: Lucene requirement]); duplicate query terms become one list with `boost = multiplicity`.
5. **Exact idf over segments + tombstones.** Snapshot object per refresh: per field `doc_count=sum dc_s`, `sum_dl=sum sdl_s`, per query-term `df = sum df_s`. Level 0 default: stale-inclusive (tombstoned docs still counted until merge; stats are consistent within a snapshot, so a query never sees mixed epochs). Level 1: when deleting a doc, subtract it from `doc_count_s`/`sum_dl_s` using its norm byte (O(1)). Level 2 (only for segments with deleted ratio rho_s > 5%): for query terms with `df_s <= 4096`, compute `df_live = df_s - |postings ∩ tombstones_s|` by one scan; larger df terms keep stale df (error <= ~ln(1+rho) nats, and cancels with N under uniform deletion [D]). Merge policy keeps rho_s <= ~20-33% and S <= ~30 segments. Because skip metadata holds (tf, norm) pairs and not scores, **pruning stays exact under global stats** [D]. Federation: two-phase DFS (sources return `doc_count, sum_dl, df_t`; coordinator sends the aggregate back with the query) [D, mirrors ES `dfs_query_then_fetch` [F]].
6. **Analyzers** (per field, set in schema): `prose`: NFKC+casefold -> UAX#29-lite word split (letters/digits runs, apostrophes inside words kept, CJK -> overlapping bigrams) -> Porter2 (Snowball English; hand-written C, zero deps) -> **no stopwords**; keep tokens of length 1..255 bytes (drop longer). `exact`: casefold only, no stemming (names, tags, paths segments). `code`: split on non-alnum, then identifier splitting: boundaries at lower->Upper (`fooBar`), acronym end (`HTTPServer` -> `http`,`server`), letter<->digit (`utf8Decode` -> `utf`,`8`,`decode`), `_`, `-`, `.`, `::`; emit **consecutive positions** for parts so the phrase "thread pool" matches `ThreadPool`, `thread_pool`, `thread-pool`; keep the whole lowercased identifier as a second token with position increment 0 (counted out of length via `discount_overlaps`) or in a second field `ident` (tf only, no positions). Never stem code or names. Max tokens per field 2^20; replace invalid UTF-8 with U+FFFD.
7. **Positional index + phrase/NEAR** per section 2.6: `.pos` stream, `pos_off` stored beside each skip entry in a parallel `uint32` array (kept out of the 12 B BM25 entry so non-phrase queries never touch it). Phrase UB `sum idf * min_i h_i`.
8. **Multi-field**: v1 = per-field BM25 summed with boosts (title/name x2-3, path x1.5, body x1 as starting points [D]); each (field, term) is its own list with its own UB. v2 = BM25F for 2-4 designated fields via a virtual merged list.
9. **Learned-sparse field type `sparse`** (plugin ingestors produce `(vocab_id, weight)`; the core never runs a model): direct-indexed list table by vocab id (30,522 entries x ~16 B for BERT WordPiece [R on vocab size]), postings `(docid, u8 impact)` with global per-field scale, query weights u8/u16, integer accumulation in u32. Step A: query = MaxScore over lists (works for inference-free queries, ~5-10 terms). Step B: **BMP** with b=16 for exact / b=64-128 and alpha=0.75-0.85 for approximate, docs BP-ordered at merge time [F]; filter bitmap used to skip blocks with zero population. Step C (stretch): **Seismic** with lambda=6000, beta=400, alpha=0.4, 8-bit summaries, cut<=10, heap_factor 0.7-1.0.
10. **SIMD (AVX2 + scalar fallback, runtime dispatch; NEON path later)**: decode via vertical 4/8-lane BP128; block-wide scoring only in "score everything in the window" paths (MaxScore essential lists, BMMS inner windows): for 8 docs at a time `tf = cvtepi32_ps(tf_u32)`; `nrm = i32gather(norm_bytes, doc) & 0xFF` (pad 3 bytes at end of column); `K = i32gather_ps(Klut, nrm, 4)`; `score = fmadd(idf, tf/(tf+K), acc)` with `_mm256_div_ps` (or rcp+1 Newton step if bit-exactness with the scalar path is not required - default: **keep div for exactness**). BMW pivot scoring stays scalar. Windows accumulate into a `float acc[W]` array; heap push only if `acc[i] > theta` (SIMD compare+movemask). Use `uint64_t`/`size_t` for offsets (Windows `long` is 32-bit); MSVC needs `_BitScanForward64` wrappers for ctz.
11. **Doc ordering**: segment sort key = (object `type`, then recursive-graph-bisection order) built at merge when segment > ~1M postings; keeps a `docid -> external id` map (needed anyway for immutable segments). Gains [F]: 10-36% smaller lists, tighter block bounds.
12. **Hit-count semantics**: return `{value, relation: "eq"|"gte"}`; exact count only when no pruning or when `track_total_hits` bound is not exceeded; disable pruning for facets/aggregations [F].

### Defaults summary
BM25 Lucene variant, k1=1.2, b=0.75; skip block 128 postings; skip2 every 32 blocks; VBMW lambda=12; BMMS inner window 4096 docs; algorithm switch at m=4/8; norm byte per doc-field; tokens <= 255 B; no stopwords; Porter2 only on `prose`; positions only on fields flagged `phrase`; BMP b=16 (exact) or 64-128 with alpha 0.75-0.85; Seismic lambda 6000/beta 400/alpha 0.4; impacts u8; merge keeps deleted ratio <= 20-33% and <= ~30 segments.

---------------------------------------------------------------------------------------------------

## 4. Skip and why

- **BM25L / BM25+ / BM25-adpt / TFl-delta-p x IDF**: no significant effectiveness gain over Lucene BM25 on Robust04/Core17/Core18 [F]; BM25L/BM25+ need non-occurrence terms and break additive pruning bounds; BM25-adpt undefined for ~90% of terms [F].
- **Eager baked scores as the main format (BM25S style)**: frozen idf/avgdl conflict with NRT segments and global stats; allow only as an optional post-force-merge "impact segment" (u8, enables SaaT and BMP-like paths).
- **Plain WAND**: dominated by BMW/MaxScore in every RISE column [F].
- **BMW on long/expanded queries**: 614 ms vs MaxScore 121 ms on SPLADE [F]; the planner must never pick it for m > ~8.
- **Score-at-a-time/IOQP as primary**: slowest inverted-index method in Seismic's comparison (17-52 ms at 90-97% accuracy on SPLADE) [F]; revisit for deadline mode.
- **Graph-based sparse ANN (GrassRMA, PyAnn)**: Seismic is 2.6-21.6x faster; build 137-267 min vs 5 min [F].
- **BIC / PEF as the block codec in v1**: BIC decodes 5-7 ns/int and made MaxScore 2.8x slower than SVB (18.8 vs 6.8 ms) [F]; PEF is the best space/time scheme (3.1-5.5 bits/int, ~1 ns) but its partitioning DP and select structures are heavy; defer until index size hurts. (Also affects sibling note 05; keep BP128.)
- **Stopword removal**: +/-0.1 nDCG [F], breaks phrases.
- **Stemming code/names; multi-language Snowball in core**: harmful or out of scope; English Porter2 only, others as plugins.
- **Running SPLADE/ELSER query encoders inside the core**: breaks zero-dependency; supported only through plugins or inference-free query weights.
- **Full UAX#29 + full Unicode tables in v1**: ship a compact subset (Latin/Greek/Cyrillic/CJK/digits) with a scalar fallback.

---------------------------------------------------------------------------------------------------

## 5. Novel ideas specific to NexusSearch [D unless noted]

1. **Filter-aware block skipping.** NexusSearch always has a structured result set (Roaring bitmaps) before text scoring. During BMW/MaxScore, test `filter.range_cardinality(prev_last+1, block.last_docid) == 0` per posting block and skip the payload decode; with type-clustered docid order the filter becomes a few ranges and whole lists collapse. Planner rule: if `|F| < sum_t df_t / 64` or `|F| < 0.01 N`, iterate F and probe term lists by galloping advance instead of walking the union.
2. **Type-scoped statistics.** For queries like `type:code ...`, df/avgdl from 3D-asset metadata pollute code idf. Option `stats_scope: type` prefixes dictionary keys with a 2-byte type id for flagged fields, giving per-type idf/avgdl for free; cross-type queries sum per-type namespaces and report which scope scored each hit.
3. **Certified approximate results.** In any approximate mode (BMP alpha<1, Seismic heap_factor<1, deadline cut-offs) track `remaining_UB_max` (largest UB of unevaluated blocks); if `remaining_UB_max <= theta` the answer is provably exact. Return `exact: true|false` and the gap in the explanation - unusual in the literature, trivial to compute from block metadata.
4. **EXPLAIN ANALYZE counters for lexical ranking**: per list blocks decoded/skipped, pivot moves, UB checks, theta trajectory, algorithm chosen and why (m, k, df). Gives the planner real feedback to learn its m-threshold per corpus.
5. **Block-seeded theta** (Adopt step 4) plus **per-term kth-score sidecar** for frequent terms (store top-10 `(doc,tf,norm)` and recompute with fresh idf at query time) as a cheap alternative.
6. **Reverse WAND for WATCH subscriptions (percolator)**: index the *queries*; for each incoming document tokenise once, then find subscriptions whose UB sum over matching terms exceeds their alert threshold tau using MaxScore over the subscription index; prunes most subscriptions without full scoring.
7. **Code-aware phrase semantics for free**: consecutive positions for identifier parts make `"thread pool"` hit camelCase/snake_case/kebab-case; add `ident` field boost so exact-identifier hits outrank part-matches without a second query.
8. **Inference-free sparse for AI-model and 3D objects**: a plugin encodes README/metadata offline with a document-side encoder; queries stay bag-of-tokens with a shipped IDF-like weight table, so `semantic:`-adjacent lexical expansion works with BM25-class latency (1.1x BM25 [F-abs]) and no runtime model.

---------------------------------------------------------------------------------------------------

## 6. Validation / test ideas

- **Oracle equivalence**: random Zipf corpora (N=1e3..1e6, vocabulary 1e4..1e6), random 1-12 term queries, k in {1,10,100,1000}; assert MaxScore == BMW == BMMS == exhaustive top-k *including scores bit-exact and tie-order by docid*. Adversarial cases: all tf equal, k > matches, k=1, duplicate terms, term present in all docs (idf>0 but tiny), boosts 0 and huge, empty segments, single-block lists, tf>255 saturation.
- **Bound invariants at build time**: for every block assert `real_max_contrib <= block_ub` over a sweep of (k1, b, avgdl) values; for every term `term_ub >= max block_ub`; Pareto frontier collapse never lowers a bound.
- **Merge invariance (exact idf)**: index the same docs as 1, 5, 30 segments and after force-merge; scores must agree to 1 ulp and rankings be identical. **Tombstone test**: delete 20% uniformly and 20% clustered; compare Level 0/1/2 against a clean rebuild with rank-biased overlap and score deltas; assert Level 2 equals the rebuild for df<=4096 terms.
- **Effectiveness regression** (small BEIR sets only - SciFact ~5k and NFCorpus ~3.6k docs [R on sizes]; download, run, delete; keep under ~20 MB): BM25S Table 3 (its own tokenizer, Lucene variant) gives nDCG@10 targets **SciFact 68.0 / NFCorpus 31.8** at k1=1.2,b=0.75; **68.7 / 32.1** at k1=1.5,b=0.75; **67.6 / 31.8** at k1=0.9,b=0.4 [F]. Accept +/-1.0 because analyzers differ. Switching stemming on/off should move the BEIR mean ~1 point in the same direction as BM25S's ablation (39.6 vs 38.3).
- **Variant parity**: implement ATIRE/Robertson in the test harness only; Kamphuis Table 2 shows Lucene-accurate == ATIRE on Robust04 (.2533) [F]; compare Lucene-lossy vs exact-length norm (delta AP <= 0.005).
- **Performance gates (single thread)**: BMW vs exhaustive OR >= 3x at k=10, m<=4 on a Zipf corpus with >=1e7 postings; <= 1.5x at k=1000 acceptable [F: Lucene 3.4x/1.4x]; MaxScore beating BMW at m >= 9; metadata overhead <= 12 B/128 postings (~10% of payload) and indexing slowdown <= 8% [F: Lucene +5.7-7.6%].
- **Phrase/NEAR**: compare against a naive position scan on 1e4 random docs; unit table for identifier splitting (`HTTPServer2XML`, `parseXMLHttp`, `__init__`, `snake_case_ID`, `OAuth2`, `utf8Decode`, `C++`, `foo.bar::baz`) with golden outputs; phrase hits for `"thread pool"` on `ThreadPool/thread_pool/thread-pool`.
- **Fuzz**: invalid UTF-8, combining marks, ZWJ emoji, 10 MB token, NUL bytes, 1M-token docs; analyzer must be total (no crash, bounded memory).
- **Learned sparse**: BMP exact mode must equal exhaustive dot-product top-k; approximate modes report recall@k vs exact and the certified-exact flag rate; Seismic recall at heap_factor=1.0 vs 0.7 on a 100k-doc synthetic sparse set.
- **Hit counts**: with pruning on, `relation:"gte"` and `value <= true count`; with aggregations, pruning disabled and `eq`.

---------------------------------------------------------------------------------------------------

## 7. References

Read in full (text) this session **[F]**
- Lu, X.H. 2024. *BM25S: Orders of magnitude faster lexical search via eager sparse scoring.* arXiv 2407.03618. https://arxiv.org/abs/2407.03618 ; code https://github.com/xhluca/bm25s
- Kamphuis, de Vries, Boytsov, Lin. ECIR 2020. *Which BM25 Do You Mean? A Large-Scale Reproducibility Study of Scoring Variants.* (preprint text read; code https://github.com/Chriskamphuis/olddog ; canonical paper URL not fetched, recall-only)
- Grand, Muir, Ferenczi, Lin. ECIR 2020. *From MaxScore to Block-Max WAND: The Story of How Lucene Significantly Improved Query Evaluation Performance.* (preprint text read; JIRA threads https://issues.apache.org/jira/browse/LUCENE-2959 , LUCENE-4100, LUCENE-7996)
- Mallia, Suel, Tonellotto. SIGIR 2024. *Faster Learned Sparse Retrieval with Block-Max Pruning.* https://arxiv.org/abs/2405.01117 ; code https://github.com/pisa-engine/BMP
- Bruch, Nardini, Rulli, Venturini. SIGIR 2024. *Efficient Inverted Indexes for Approximate Retrieval over Learned Sparse Representations (Seismic).* https://arxiv.org/abs/2404.18812 ; doi 10.1145/3626772.3657769 ; code https://github.com/TusKANNy/seismic
- Lassance, Dejean, Formal, Clinchant. 2024. *SPLADE-v3: New baselines for SPLADE.* https://arxiv.org/abs/2403.06789
- Savino, Venturini. 2026. *RISE: A Rust Library for Inverted Index Search Engines.* https://arxiv.org/abs/2606.07187 ; code https://github.com/AngeloSav/rise-rs
- Pibiri, Venturini. ACM CSUR 2020. *Techniques for Inverted Index Compression.* https://arxiv.org/abs/1908.10598 ; doi 10.1145/3415148
- Elastic. *Faster retrieval of top hits in Elasticsearch with Block-Max WAND.* https://www.elastic.co/blog/faster-retrieval-of-top-hits-in-elasticsearch-with-block-max-wand
- Elastic. *Practical BM25 - Part 1: How Shards Affect Relevance Scoring.* https://www.elastic.co/blog/practical-bm25-part-1-how-shards-affect-relevance-scoring-in-elasticsearch
- Apache Lucene. `BM25Similarity` (9.10 javadoc). https://lucene.apache.org/core/9_10_0/core/org/apache/lucene/search/similarities/BM25Similarity.html

Abstract/landing page only **[F-abs]**
- Geng, Wang, Ru, Yang. 2024 (rev. 2025). *Towards Competitive Search Relevance For Inference-Free Learned Sparse Retrievers.* https://arxiv.org/abs/2411.04403
- Carlson, Xie, He, Yang. SIGIR 2025. *Dynamic Superblock Pruning for Fast Learned Sparse Retrieval.* https://arxiv.org/abs/2504.17045

Verified only via the reference lists of the papers above **[C]** (DOIs/venues copied, papers not read)
- Ding, Suel. SIGIR 2011. *Faster top-k document retrieval using block-max indexes.* https://doi.org/10.1145/2009916.2010048
- Broder, Carmel, Herscovici, Soffer, Zien. CIKM 2003. *Efficient query evaluation using a two-level retrieval process (WAND).* https://doi.org/10.1145/956863.956944
- Turtle, Flood. IP&M 1995. *Query evaluation: Strategies and optimizations (MaxScore).* https://doi.org/10.1016/0306-4573(95)00020-H
- Mallia, Ottaviano, Porciani, Tonellotto, Venturini. SIGIR 2017. *Faster BlockMax WAND with Variable-sized Blocks.* https://doi.org/10.1145/3077136.3080780
- Macdonald, Tonellotto. ICTIR 2017. *Upper bound approximation for BlockMaxWand.* (no URL)
- Mallia, Siedlaczek, Mackenzie, Suel. 2019. *PISA: Performant Indexes and Search for Academia.* http://ceur-ws.org/Vol-2409/docker08.pdf
- Dhulipala et al. KDD 2016. *Compressing Graphs and Indexes with Recursive Graph Bisection.* https://doi.org/10.1145/2939672.2939862
- Mackenzie, Petri, Gallagher. 2022. *IOQP: A simple Impact-Ordered Query Processor written in Rust.* DESIRES (no URL)
- Tonellotto, Macdonald, Ounis. 2018. *Efficient Query Processing for Scalable Web Search.* FnTIR 12(4-5). https://doi.org/10.1561/1500000057
- Mackenzie, MacAvaney, Mallia, Siedlaczek. 2026. *Practical, Efficient, In-Memory Inverted Indexes.* https://doi.org/10.1007/978-3-032-21321-1_1 (tutorial; not read)
- Lemire, Kurz, Rupp. 2018. *Stream VByte.* https://doi.org/10.1016/j.ipl.2017.09.011

Recall-only **[R]** (no source fetched; verify before relying)
- Robertson, Zaragoza, Taylor. CIKM 2004. BM25F. ; Tantivy (https://github.com/quickwit-oss/tantivy) block layout/constants ; Elastic ELSER internals ; Unicode UAX #29 and NFKC details ; Lucene WordDelimiterGraphFilter flags ; Lucene sloppy-phrase scoring and phrase idf ; Lucene's deleted-doc treatment in term statistics ; BERT WordPiece vocabulary size 30,522 ; Lucene length-norm byte exactness thresholds.
