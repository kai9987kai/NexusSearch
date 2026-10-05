# 08 - Hybrid fusion, learning-to-rank, explainable scoring (research note for NexusSearch)

Status: research spec, written 2026-10-02. Source tags (same convention as notes 01 and 05):
- **[F]** = fetched in this session; numbers come from the fetch tool's summary of the page (a small model summarised it, so treat exact digits as "as reported by the summary"). The Bruch PDF exceeded the fetch size limit, so its numbers come from the ar5iv HTML render.
- **[S]** = seen only in a search-result snippet; not read.
- **[R]** = recall-only (from memory, not fetched). Unverified.
- **[D]** = my own derivation/proposal. Must be validated with the harness in section 6 before being trusted.

Context fixed by the other notes: every structured clause is an exact Roaring bitmap (so survivor count `c` and the survivor set are exact and free);
BM25 uses **global** (snapshot-wide) N/df/avgdl so per-segment scores are comparable (ARCHITECTURE.md); the pipeline is `per-segment top-k -> merge -> fuse/rank -> hits(+explanations)`;
the query language already has `^boost`, `semantic:"..."` (params `k`=100, `min`, `ef`), `name~"x"` fuzzy, `SORT BY _score`, `EXPLAIN [ANALYZE]`, `WATCH`, `SOURCE(a,b)`.

---------------------------------------------------------------------------------------------------

## 1. Landscape

| System / paper | What it does for fusion, LTR or explain | Defaults that matter | Tag |
|---|---|---|---|
| **Bruch, Gai, Ingber, "An Analysis of Fusion Functions for Hybrid Retrieval"** (arXiv 2210.11934; v1 Oct 2022, rev. May 2023 [F]; journal venue ACM TOIS is recall-only [R]) | RRF vs convex combination (CC) of lexical and semantic scores; normalisation study | CC beats RRF in-domain and out-of-domain; CC "sample efficient"; RRF "sensitive to its parameters"; TM2C2 with alpha=0.8 good, alpha in [0.6,0.8] consistently improves; alpha converges with <5% of training data | [F] https://arxiv.org/abs/2210.11934 , https://ar5iv.labs.arxiv.org/html/2210.11934 |
| **RRF** (Cormack, Clarke, Buettcher, SIGIR 2009, "Reciprocal Rank Fusion outperforms Condorcet and individual rank learning methods") | `sum_i 1/(k + rank_i(d))` over ranked lists | k = 60 | [R] original; k=60 and formula confirmed by ES and Weaviate docs [F] |
| **Elasticsearch RRF retriever** | RRF, no score normalisation, equal weights | `rank_constant`=60; `rank_window_size` defaults to `size`; changing the window changes order even of already-seen ranks; at least 2 child retrievers | [F] https://www.elastic.co/docs/reference/elasticsearch/rest-apis/reciprocal-rank-fusion |
| **Elasticsearch linear retriever** | `Score = sum_i weight_i * Normalizer(score_i)`; normalisers `none`, `minmax` ((s-min)/(max-min)), `l2_norm`; weights >=0, default 1.0 | `rank_window_size` default 10; docs warn `none` biases toward lexical because BM25 is unbounded | [F] https://www.elastic.co/docs/reference/elasticsearch/rest-apis/retrievers/linear-retriever |
| **Weaviate hybrid** | `rankedFusion` = 1/(RANK+60); `relativeScoreFusion` = min-max per result set then alpha-weighted; relativeScoreFusion is the default since v1.24 and the recommended one (keeps score magnitude) | `alpha`=0.75 (1 = pure vector) | [F] https://docs.weaviate.io/weaviate/concepts/search/hybrid-search |
| **Qdrant Query API** | server-side `rrf` and `dbsf` (distribution-based score fusion: per-list mean +/- 3 sigma as normalisation limits, then sum) | - | [R] |
| **OpenSearch** hybrid | normalisation-processor (min_max / l2 / z-score) + combination (arithmetic / harmonic / geometric mean, weights) | - | [R] |
| **Vespa** | phased ranking: first-phase cheap, second-phase and global-phase with full rank features, optional cross-encoder/ONNX model in the phase | - | [R] |
| **Lucene `Explanation`** | immutable tree: `value` (Number), `description` (String), `details` (child array), `isMatch`; `match()` / `noMatch()` factories; `toString()` renders the tree. `explain(doc)` is computed after the fact for one doc | - | [F] https://lucene.apache.org/core/9_10_0/core/org/apache/lucene/search/Explanation.html |
| **DAT** (Hsu, Tzeng, "Dynamic Alpha Tuning for Hybrid Retrieval in RAG", arXiv 2503.23013, Mar 2025) | per-query alpha: an LLM scores the top-1 doc of BM25 and of dense, alpha derived from the two effectiveness scores | up to +7.5% Precision@1 on hard queries (snippet) | [S] https://arxiv.org/abs/2503.23013 |
| **LambdaMART** (Burges, "From RankNet to LambdaRank to LambdaMART: An Overview", MSR-TR-2008-109, 2010); LightGBM/XGBoost implement it | gradient-boosted trees with lambda gradients weighted by |delta nDCG| | LightGBM `lambdarank_truncation_level`=30, label_gain 2^i-1 | [R] |
| **Unbiased LambdaMART** (Hu, Wang, Peng, Li; arXiv 1809.05818, 2018/19) | position-bias-corrected pairwise LTR from clicks via propensity weighting inside LambdaMART; validated by online A/B at a commercial engine | - | [F] https://arxiv.org/abs/1809.05818 |
| **Skip-Above click preference** (Joachims et al. 2005), **IPW LTR** (Joachims, Swaminathan, Schnabel, WSDM 2017), **team-draft interleaving** (Radlinski, Kurup, Joachims, CIKM 2008) | clicks -> pairwise preferences; counterfactual position-bias correction; cheap online A/B | - | [R] |
| **ColBERT / ColBERTv2** (Khattab and Zaharia SIGIR 2020 [R]; Santhanam et al., NAACL 2022, arXiv 2112.01488) | late interaction: one vector per token, `score = sum_q max_d <q,d>`; v2 adds **residual compression** + denoised supervision | **6-10x smaller index** than v1 | [F] https://arxiv.org/abs/2112.01488 |
| **PLAID** (Santhanam, Khattab, Potts, Zaharia, arXiv 2205.09707, 2022) | centroid interaction + centroid pruning to avoid decompressing most candidates | up to **7x GPU / 45x CPU** latency reduction vs vanilla ColBERTv2; evaluated at 140M passages; tens of ms GPU, tens to few hundred ms CPU | [F] https://arxiv.org/abs/2205.09707 |
| **MUVERA** (Dhulipala, Hadian, Jayaram, Lee, Mirrokni, arXiv 2405.19504, May 2024, revised Jun 2026) | Fixed Dimensional Encodings: multi-vector -> one vector whose dot product approximates Chamfer/MaxSim, so plain MIPS (HNSW/DiskANN) can retrieve | **2-5x fewer candidates**; on BEIR avg **+10% recall and -90% latency vs PLAID**; eps-approximation guarantee | [F] https://arxiv.org/abs/2405.19504 |

Reading of the landscape: the three most-used engines (Elastic, Weaviate, OpenSearch) converged on **normalised weighted score sum** as the practical default
and keep RRF as the "no tuning, heterogeneous sources" option; Bruch et al. supply the theory/evidence; LTR is a separate rerank stage; late interaction is a *storage* problem more than a ranking problem.

---------------------------------------------------------------------------------------------------

## 2. Techniques

### 2.1 Reciprocal Rank Fusion
- Formula: `RRF(d) = sum_{i in lists} w_i / (k + rank_i(d))`, rank starts at 1, missing doc contributes 0. [F: ES, Weaviate; Cormack 2009 original [R]]
- Defaults: k=60, w_i=1, window = per-retriever depth (ES: = `size`; we use 100, section 3).
- Data: needs only ranked ids; no scores. This is why it is the lingua franca for **federation** (sources that cannot share score scales).
- Gains: robust zero-config baseline. Authors of the 2009 paper: beats Condorcet and individual rank-learning (title claim [R]).
- Pitfalls: (a) Bruch et al.: RRF is sensitive to k and the lexical/semantic k pair; tuned RRF generalises poorly out-of-domain; the optimal (k_lex, k_sem) combination **flips** between in-domain and out-of-domain [F]. (b) throws away score magnitude: ranks 1 and 2 with scores 0.99/0.50 get the same gap as 0.99/0.98 [F: Weaviate rationale]. (c) window-dependent: ES documents that changing `rank_window_size` reorders already-seen results [F] - a pagination hazard. (d) ties: documents at the same rank in two lists tie; need deterministic tie-break.

### 2.2 Convex combination (CC) and normalisation
- Bruch et al. definitions [F]: `f_CC(q,d) = alpha*f_sem + (1-alpha)*f_lex`, 0<=alpha<=1; normalisers `min-max: (s-m_q)/(M_q-m_q)`; `TM2C2` ("theoretical min-max"): `(s - inf)/(M_q - inf)` with **theoretical minimum** `inf` = 0 for BM25 and -1 for cosine, `M_q` = max in the retrieved list; `z-score: (s-mu)/sigma`.
- Findings [F]: CC outperforms RRF in- and out-of-domain; learning of CC "generally agnostic to the choice of score normalisation"; **un-normalised** CC badly degrades in-domain because BM25 is unbounded and shifts rankings of a fine-tuned dense model; **alpha=0.8** worked for in-domain sets, **alpha in [0.6,0.8]** consistently improved; alpha converges with **<5% of training data** (a small set of queries) regardless of domain shift; TM2C2 significantly beat RRF on all datasets (their Table 2) on nDCG. Metric: nDCG@1000 (nDCG@100 for SciFact/NFCorpus); sets: MS MARCO, NQ, Quora in-domain + 8 BEIR zero-shot (NFCorpus, HotpotQA, FEVER, SciFact, DBPedia, FiQA, ...).
- Why alpha is so high [D, inference from the formulas]: with theoretical min -1, a cosine of 0.30 normalises to (0.3+1)/(0.75+1)=0.743 and the best (0.75) to 1.0, so the dense component only spans ~0.26 while BM25 spans [0,1]. A semantic weight of 0.8 vs lexical 0.2 (ratio 4:1) merely restores equal *effective* influence. Consequence for us: the weight defaults only make sense **together with** the normaliser; ship them as a pair (section 3.1).
- Engines' practice: ES `minmax` over the retrieved window; Weaviate relativeScoreFusion = min-max per result set [F]. Both normalise over the *returned window*, so a doc's normalised score depends on window size and (in distributed engines) shard composition. We fix this (3.2).

### 2.3 Calibration and absolute scores
- Relative normalisers (max over this query's candidates) give no absolute meaning: top-1 always gets 1.0. Needed for thresholds ("no good result"), WATCH (percolator: scored against newly committed docs only; there is no candidate set), federation, and agent/MCP confidence.
- Options: (i) analytic bounds (BM25 per-clause upper bound `idf*boost`; cosine in [-1,1]); (ii) Platt scaling `p = sigmoid(a*S+b)` fit by logistic regression on (score, relevant?) pairs [R: Platt 1999]; (iii) DBSF-style mean +/- 3 sigma [R].
- NexusSearch plan: **two normaliser families** - `relative` (search) and `absolute` (WATCH, federation, `min_score`), plus optional Platt calibration stored in `ranker.json` (3.7).

### 2.4 Per-query dynamic weighting
- DAT [S]: LLM judges top-1 of each retriever; too slow/non-local for a zero-dependency C engine. Keep the idea (query-specific alpha from retriever confidence) but compute confidence from score shapes - "QPP-lite" [D] in 5.3.
- Cheap query-feature priors [D]: quoted phrase, identifier-like tokens (CamelCase, digits, `_`, path separators, version numbers) push weight to lexical; >=4 unquoted natural-language tokens and no field operators push to semantic. Implement as a multiplier in [0.5,1.5] on w_t / w_v, default OFF until validated.

### 2.5 Learning to rank
- **Offline LambdaMART** (R): best accuracy with many features and thousands of judged queries; far too data-hungry for a personal local index, so train in Python (LightGBM) if ever, export as flat arrays. Not in the C core v1.
- **Online/lightweight (adopt)**: a *linear* model over <=16 bounded features, pairwise logistic loss, trained from clicks with inverse-propensity weights [D built on F: Unbiased LambdaMART's propensity idea; R: Skip-Above, IPW-LTR]:
  ```
  features x(d) in [0,1]^D : T,F,V,R_rec,R_pop, title_exact, id_exact, rank_T/ W, rank_V/ W, ctr_shrunk, ...
  s(d) = w . x(d)                       // w >= 0, initialised to the default formula weights
  click at rank r on doc c; skipped docs u ranked above c (Skip-Above)
  p(r)  = 1 / r^eta , eta=1             // examination propensity [R: position-based model]
  loss  = min(1/p(r), 10) * log(1 + exp(-(s(c) - s(u))))      // IPW, clip 10
  w    <- w - lr*(grad + lambda*(w - w_default)), lr=0.02, lambda=1e-2; then clamp w_i in [0.5, 1.5]*w_default_i; renormalise
  ```
  Ridge-toward-prior and a +/-50% trust region make it safe in the tiny-data regime; the explanation stays exact (`contrib_i = w_i x_i`).
- Activation gate [D]: >=200 click events and a held-out replay that improves MRR@10 by >=0.02 with sign-test p<0.05 (Smucker et al. CIKM 2007 compare significance tests [R]); otherwise keep defaults. Keep click logs local, append-only, CRC'd, opt-in.

### 2.6 Late interaction: is a small multi-vector path worthwhile?
- Mechanics: `MaxSim(Q,D) = (1/|Q|) sum_{q in Q} max_{d in D} <q,d>` (normalised average keeps it in [-1,1], comparable to cosine) [R for the 1/|Q| variant].
- ColBERTv2: residual compression gives 6-10x smaller index than v1 [F]. Arithmetic [D, assuming 128-d, fp16 baseline = 256 B/token]: centroid id (4 B) + 2-bit residuals (32 B) = 36 B/token = 7.1x; 1-bit residuals = 20 B/token = 12.8x (consistent with the reported range). PLAID cuts latency 7x GPU / 45x CPU [F] but is an engine built for 10^7-10^8 passages.
- MUVERA FDE [F for claims; construction details R, low confidence]: SimHash-partition the embedding space into 2^k_sim buckets; per bucket sum (queries) or average (docs) the token vectors, random-project each to d_proj, concatenate; repeat R_reps times; dim = R_reps * 2^k_sim * d_proj. Typical configs reach ~10^4 dims (e.g. 20*32*16 = 10,240 floats = 40 KB/doc) before optional compression [R].
- NexusSearch machine reality [D]: 1M docs x 100 tokens x 36 B = **3.6 GB** > the ~3 GB free disk; 10k-100k docs = 36-360 MB is feasible but at that size exact brute-force MaxSim over the top-100 fused candidates is enough: 32 query tokens x 100 docs x 200 tokens x 128 dims = 81.9 M MAC ~ 5-20 ms with AVX2 int8. Verdict: **do not build PLAID/MUVERA; offer MaxSim only as a rerank hook** with a reserved schema slot (`vector.kind:"multi"`), plus the much cheaper **chunk-max aggregation** (doc score = max over chunk vectors) which long files/code/docs need anyway.

### 2.7 Cross-encoder rerank hook
- Interface (plugin ABI, no dependency in core): `double (*score_pairs)(void *ctx, const char *query, const nx_text_view *docs, size_t n, double *out)`. Run on the top `M=30` of the fused list (hard cap 100), cache by `(hash(query), doc_id, doc_gen)`. Final `S' = (1-beta)*S_rel + beta*sigmoid(ce)`, beta=0.7 default when enabled [D]; explanation records both. ONNX Runtime is available locally for the plugin; latency per pair must be measured on this CPU [D].

### 2.8 Priors and decay
- Recency [D, shape from Elasticsearch function_score `exp`/`gauss`/`linear` decay [R]]: `R_rec = 0.5^(max(0, age - offset)/half_life)`; defaults offset=7 d, half_life=180 d (per-type override).
- Popularity [D, shape from Lucene `FeatureField` saturation `x/(x+pivot)` [R]]: `R_pop = x/(x+pivot)`, pivot = column median from the equi-depth histogram (note 05). Bounded in [0,1), outlier-robust, no global max needed, merge-stable.
- `R = rho*R_rec + (1-rho)*R_pop`, rho=0.7 when both configured; a missing field contributes its neutral value 0 (not 0.5) and its weight is renormalised away.
- Learned click prior (optional): `ctr = (clicks + a)/(impr + a + b)`, a=1, b=19 (5% prior CTR) [D].

### 2.9 Diversity
- MMR (Carbonell and Goldstein, SIGIR 1998 [R]): `argmax_d [ lambda*Rel(d) - (1-lambda)*max_{d' in S} sim(d,d') ]`, lambda=0.7 default, pool = top 50 after fusion. `sim` = cosine of stored vectors if present, else Jaccard of trigram/term sets, else same-directory/same-repo indicator. Cost k*W = 20*50 = 1000 similarities (tens of microseconds).
- Cheaper and often more useful for files/models: **collapse by key** (e.g. same `repo`, same model family across formats) with `inner_hits` count.

### 2.10 Metrics (definitions to implement exactly)
```
DCG@k   = sum_{i=1..k} (2^rel_i - 1) / log2(i+1)         [R: Jarvelin & Kekalainen 2002]
nDCG@k  = DCG@k / IDCG@k            (0 if IDCG=0)         // also keep linear-gain variant `ndcg_lin` to match trec_eval
recall@k= |relevant ∩ top_k| / |relevant|
MRR@k   = mean_q 1/rank_of_first_relevant  (0 if none in top k)
```
Report nDCG@10 (BEIR convention [R]), recall@100, MRR@10; Bruch used nDCG@1000 [F].

---------------------------------------------------------------------------------------------------

## 3. Adopt for NexusSearch

### 3.1 The final scoring formula and defaults
Structured clauses (`type:`, ranges, `format:` ...) are **filters**: they define the survivor set and contribute no score. Scoring components:

```
S(d) = w_t*T(d) + w_f*F(d) + w_v*V(d) + w_r*R(d)        all components in [0,1]  => S in [0,1]
defaults (w_t, w_f, w_v, w_r) = (0.35, 0.15, 0.50, 0.10)
```
Only components whose clause class is present are *active* (T needs a text/code clause, F a fuzzy clause, V a `semantic:` clause, R a configured recency/popularity field).
Weights of active relevance components are renormalised to sum to `1 - w_r` (R keeps its own `w_r`, only when some relevance component is active):
`eff_i = w_i / sum_{j in active relevance} w_j * (1 - w_r)`.
Check: active {t,v} -> eff = (0.3706, 0.5294), r=0.10; active {t,f} -> 0.7/0.3 split; active {t,f,v} -> 0.35/0.15/0.50 split.
This corresponds to roughly lexical:semantic = 0.41:0.59 when both present (inside Bruch's [0.6,0.8] semantic range once the compressed-cosine effect of 2.2 is removed by the floor normaliser below) [D - validate].

| Component | Definition | Normaliser |
|---|---|---|
| **T** text/code BM25 | per field Lucene-style BM25: `idf = ln(1 + (N-df+0.5)/(df+0.5))`, `tfpart = tf/(tf + k1*(1-b+b*dl/avgdl))`, `k1=1.2, b=0.75` [R: Lucene defaults]; term score `boost*idf*tfpart`; phrase = sum of term idf with tf := phrase frequency; fields combined dis-max style `max + 0.3*sum(others)` [D]; clauses (AND/OR) summed; NOT contributes nothing | **TM2C2-obs**: `T = b / M_t` with theoretical min 0 and `M_t' = max(b_top1, gamma*UB_q)`, `UB_q = sum_clauses boost*idf`, gamma=0.25 [D]. `b_top1` = max over **all filter survivors** (exact, falls out of WAND/MaxScore; independent of LIMIT/OFFSET/window/segments) |
| **F** fuzzy/name | per query token `sim = 1 - ED/max(len)` (Damerau-Levenshtein on normalised UTF-8 code points; max distance auto: 0 for len<=2, 1 for <=5, else 2 as in QUERY_LANGUAGE.md), idf-weighted mean over tokens, 0 if any required token has no match within bound; exact match = 1.0 | already in [0,1]; none |
| **V** vector | cosine (dot on unit vectors) of the doc's best chunk; several `semantic:` clauses -> boost-weighted mean | **floor-calibrated TM2C2**: `V = clamp((s - s0)/(M_v' - s0), 0, 1)`, `s0` = mean cosine of 2,048 random vector pairs of that field (stored in the stats section, recomputed at merge) - the empirical "unrelated" level; `M_v' = max(s_top1, s0 + 0.25*(1 - s0))`. Alternative `norm_v:"tm2c2"` uses s0=-1 (Bruch) - then use (w_t,w_v) ratio 1:4 |
| **R** priors | `rho*R_rec + (1-rho)*R_pop` (2.8) | already in [0,1] |

Non-finite or zero-norm vectors -> s := s0 (V=0); `M_t <= 0` -> T=0; never divide without an epsilon guard.
Windows: per-retriever window **W=100** (cap 1,000), fusion union <= 3W. Tie-break: quantise S to 1e-6, then `_id` bytewise ascending (needed for stable pagination).

**Modes** (`rank_by` in query params / schema):
- `cc` (default, single index): the formula above.
- `rrf`: `sum w_i/(k+rank_i)`, k=60, W=100 - used automatically for **federation** (`SOURCE(a,b)`) and as an opt-in; `learned`: 2.5.
- `absolute` normalisation (WATCH, federation score thresholds, `min_score`): `T = b/UB_q` (analytic bound), `V = (s-s0)/(1-s0)`; never depends on other docs.

### 3.2 Pipeline (pseudocode, "complete-then-fuse")
```
rank(snapshot, plan, cfg):
  parallel per segment s:                                     // exact bitmaps already computed by the planner
     B_s   = survivors bitmap (exact), c_s = |B_s|
     L_t   = WANDtop(W, text clauses, global stats, B_s)      // (ord, bm25), also max_s
     L_f   = fuzzytop(W, fuzzy clauses, B_s)
     L_v   = filtered_ann_or_exact(W, B_s)                    // note 01; (ord, cos), also max_s
  merge: k-way merge each list across segments to global top-W; M_t = max_s max_s^t ; M_v likewise
  U = union(L_t, L_f, L_v) sorted by (seg, ord)               // <= 3W candidates
  for u in U sorted:                                          // monotone advance => cheap
     complete missing: V by gather+dot (~50 ns @384d AVX2), T via postings advance(ord) per term
     (~1-2 us/doc), F via term-dict fuzzy table, R from column values
  normalise (3.1), fuse, push to bounded heap (offset+limit) with deterministic tie-break
  optional: cross-encoder / MaxSim rerank on top M ; MMR / collapse on top pool
  explain only for the returned hits, lazily (3.4)
```
Cost for |U|=300: well under 1 ms of fusion work [D estimate]; no allocation on the hot path beyond the arena (`nx_arena`).
Because every candidate has *all* components exact, the score is a pure per-document function of (doc, query, snapshot constants); window size only changes **membership**, not order.

### 3.3 Data layouts
```c
enum { NX_C_T, NX_C_F, NX_C_V, NX_C_R, NX_C_N };            // component ids
typedef struct nx_cand {          // 56 bytes, arena-allocated array
    uint32_t seg, ord;
    float    raw[NX_C_N];         // bm25, sim, cosine, prior(0..1)
    float    norm[NX_C_N];        // after normaliser
    float    score;               // fused
    uint16_t rank[NX_C_N];        // rank inside each retriever list (0 = absent) for rrf + explain
    uint8_t  have;                // bit i: component i present (retrieved or completed)
    uint8_t  pad[1];
} nx_cand;

typedef struct nx_rank_cfg {      // persisted in schema.json "ranking" + ranker.json
    float w[NX_C_N];              // 0.35 0.15 0.50 0.10
    uint8_t norm_t, norm_v, mode; // TM2C2OBS|ABS ; FLOOR|TM2C2|ABS ; CC|RRF|LEARNED
    float k1, b, tie_breaker, gamma_t, gamma_v;   // 1.2 0.75 0.3 0.25 0.25
    float half_life_d, offset_d, rho, rrf_k;      // 180 7 0.7 60
    uint16_t window;              // 100
} nx_rank_cfg;
```
Per-index constants live in the segment `stats` section (note 05 layout): `N, df tables, avgdl[field]`, `vec_floor s0[field]` (f32), `pop_pivot[field]` (f64), plus the engine-wide merged values in the manifest so all segments in a snapshot use the same numbers.

### 3.4 Explanation record (exact format, version 1)
Query-level record (returned by `EXPLAIN`, and by `EXPLAIN ANALYZE` with actuals) holds the constants once; each hit holds the per-document tree. Lucene's `Explanation` shape (value, description, details, isMatch) [F] is mirrored by a generic arena tree `nx_explain_node{double value; const char *desc; uint32_t n; nx_explain_node **kids; uint8_t match;}` with renderers for text and JSON; the JSON below is the **normative** wire format.

```json
{
  "id": "models/tinyllama-1.1b.Q4_K_M.gguf",
  "score": 0.7978,
  "explain": {
    "v": 1,
    "snapshot": 41,
    "mode": "cc",
    "norm": {"t": "tm2c2-obs", "v": "floor"},
    "weights": {"t": 0.35, "f": 0.15, "v": 0.50, "r": 0.10, "active": ["t", "v", "r"]},
    "eff": {"t": 0.3706, "v": 0.5294, "r": 0.1000},
    "consts": {"b_top1": 14.8, "ub": 21.3, "M_t": 14.8, "s0": 0.18, "s_top1": 0.83, "M_v": 0.83, "survivors": 12031},
    "parts": [
      {"c": "t", "raw": 11.2, "norm": 0.7568, "w": 0.3706, "contrib": 0.2804, "rank": 4, "completed": false,
       "clauses": [{"clause": "body:\"thread pool\"", "boost": 1.0, "raw": 11.2,
                    "terms": [{"term": "thread", "idf": 4.419, "tf": 3, "dl": 400, "avgdl": 500, "k1": 1.2, "b": 0.75, "score": 3.298}]}]},
      {"c": "v", "raw": 0.71, "norm": 0.8154, "w": 0.5294, "contrib": 0.4317, "rank": 9, "completed": false,
       "detail": {"field": "emb", "metric": "cos", "path": "exact|hnsw", "chunk": 2}},
      {"c": "r", "raw": 0.8565, "norm": 0.8565, "w": 0.1000, "contrib": 0.0856, "rank": 0, "completed": true,
       "detail": {"recency": {"age_days": 40, "offset": 7, "half_life": 180, "value": 0.8807}, "pop": {"x": 1200, "pivot": 300, "value": 0.8}, "rho": 0.7}}
    ],
    "filters": [{"clause": "type:model", "bitmap_card": 12031}, {"clause": "format:(onnx OR gguf)", "bitmap_card": 5120}],
    "rerank": null,
    "sum": 0.7978
  }
}
```
Invariants (all unit-testable): `sum(parts[].contrib) == score` within 1e-6; `contrib == w*norm`; `norm` in [0,1]; `eff` sums to 1; `rrf` mode replaces `norm/w` by `{"rank": r, "k": 60, "w": w, "contrib": w/(k+r)}`; `learned` adds `{"model":"linear","features":[{"name","x","w","contrib"}]}`; the worked numbers above are checked arithmetic (idf with N=10000, df=120, tf=3, dl=400). Levels: `explain:"summary"` (parts without `clauses`/`terms`), `"full"` (term level, <=8 terms per clause). **Explanations are computed after ranking, only for the returned page**, by re-scoring those docs with the same code path (so there is zero cost when off and one code path to keep correct). Determinism: key order fixed, floats printed with 4 significant decimals but stored/compared as double.

### 3.5 Implementation order (C11, all with brute-force oracle)
1. **P0 `src/query/rank.c`**: BM25 scorer glue (global stats), normalisers, CC fuse, deterministic top-k heap, `nx_rank_oracle` (naive double-precision scoring of all survivors).
2. **P0 `src/query/explain.c`**: explain tree + JSON/text renderers + invariants; wire to `EXPLAIN`/hit output.
3. **P0 `tools/nx_eval` + `bench/`**: metrics (2.10), synthetic generator (section 6), weight grid tuner (simplex step 0.05, ~200 points x <=50 queries; objective nDCG@10; require +0.02 over defaults to adopt).
4. **P1** complete-then-fuse; RRF/weighted RRF for federation; R priors; `absolute` mode for WATCH and `min_score`; `s0` sampling at build/merge.
5. **P1** Platt calibration (Newton, <=20 iterations), `collapse`, MMR.
6. **P2** rerank hooks (cross-encoder, MaxSim int8) through the plugin ABI; click log + linear online ranker (2.5), shadow mode and rollback.
7. **P3 (stretch)** QPP-lite dynamic weights; LightGBM-exported tree ensembles in a flat array layout; reserved `vector.kind:"multi"`.

### 3.6 Config (schema.json `ranking` block; per `type` overrides)
```json
"ranking": { "mode": "cc", "window": 100,
  "weights": {"t": 0.35, "f": 0.15, "v": 0.50, "r": 0.10},
  "norm": {"t": "tm2c2-obs", "v": "floor"},
  "bm25": {"k1": 1.2, "b": 0.75, "tie_breaker": 0.3},
  "recency": {"field": "modified", "half_life": "180d", "offset": "7d"},
  "popularity": {"field": "downloads", "pivot": "median"},
  "types": {"3d": {"weights": {"t": 0.15, "f": 0.45, "v": 0.30, "r": 0.10}},
            "code": {"weights": {"t": 0.55, "f": 0.10, "v": 0.25, "r": 0.10}}} }
```
Type overrides are starting guesses [D] (3D assets have little text, code is identifier-heavy) and are exactly what the tuner should replace.

---------------------------------------------------------------------------------------------------

## 4. Skip and why
- **RRF as the default**: sensitive, window-dependent, discards magnitude, generalises poorly out-of-domain [F Bruch]; keep only for federation/opt-in.
- **PLAID, ColBERT index, MUVERA FDE as a core index**: storage (3.6 GB at 1M docs x 100 tokens, 40 KB/doc for a 10k-dim FDE [D/R]) breaks the 15 GB RAM / ~3 GB disk budget and the retrieval-quality gain over single-vector + chunk-max + rerank is unproven at our scale. Revisit only if a local ColBERT-style ONNX model is shipped.
- **LLM-judged per-query alpha (DAT)**: needs an LLM call per query, non-deterministic, violates local-first latency. Keep the confidence-based variant (5.3).
- **Full LambdaMART/GBDT training inside the C core**: needs thousands of judged queries and a trainer; offline Python only.
- **z-score / DBSF as default normaliser**: assumes unimodal score distributions; BM25 top-W distributions on small filtered sets are neither normal nor stable. Offer as option only.
- **Neural/click-embedding rankers** and **query-document click graphs**: privacy, cold-start and data volume.
- **Un-normalised score sums** (ES `none`): explicitly harmful [F: ES docs, Bruch Fig. 11].

---------------------------------------------------------------------------------------------------

## 5. Novel ideas specific to NexusSearch
1. **Complete-then-fuse with exact bitmap-filtered scores.** Industry fusers (ES, Weaviate, Qdrant) see only each retriever's returned window and zero-fill or rank-fill the rest. We have all vectors and postings in the same process and an exact survivor bitmap, so every candidate in the union gets *all* components computed exactly (gather+dot, postings advance). Removes the missing-score error that both RRF and windowed CC suffer; makes order independent of the window (only membership changes). Vespa's second-phase ranking is the closest relative [R].
2. **Window-, segment- and pagination-stable normalisers.** `M_t`, `M_v` come from the global survivor top-1 (merge of per-segment maxima), `s0` and `pivot` from index statistics. Test: same docs split over 1 vs 8 segments, any LIMIT/OFFSET -> identical order. Also the `absolute` family makes WATCH thresholds meaningful, which no reviewed engine addresses [D].
3. **QPP-lite dynamic weights.** Score-shape confidence from lists already computed: `conf_t = clamp((b_1 - b_10)/b_1, 0, 1)`, `conf_v = clamp((s_1 - s_10)/(s_1 - s0), 0, 1)`; multipliers `m_x = 0.5 + conf_x` (range [0.5,1.5]); `w_x := w_x*m_x`, renormalise. Intuition: a sharp lexical peak (rare identifier) should dominate; a flat BM25 list with a sharp vector peak (paraphrase) should defer to V. Pure [D]; default off; ship only if the harness shows a significant gain.
4. **Score receipts and `EXPLAIN DIFF a b`.** The record carries every constant needed to recompute the score offline (snapshot gen, `M_t`, `s0`, weights). Because S is linear in components, "why A above B" is `delta = sum_i eff_i*(norm_ia - norm_ib)` per component, and the **flip weight** for component i is closed-form: the `w_i` at which `S_a = S_b`. Cheap, deterministic, ideal for the MCP server (agents can justify choices) and for support/debug.
5. **Selectivity-aware inflation guard.** With 3 survivors the best always gets `T=1`. The `gamma*UB_q` / `s0+0.25*(1-s0)` floors on the denominators fix this without losing the relative behaviour on large sets; the explain record exposes which branch fired.
6. **Per-type ranking profiles and component-attribution telemetry**: which component decided clicked results per `type` feeds both the linear learner and the adaptive indexer (e.g. V rarely decisive for `type:code` -> deprioritise vector build there) [D].
7. **Learned weights as a delta over defaults** with provenance (`trained_on`, `n_events`, `replay_mrr_gain`, `created`), shadow-mode logging of disagreement, and one-command rollback.

---------------------------------------------------------------------------------------------------

## 6. Validation and test ideas
**Synthetic corpus (no downloads, seeded, ~2,000 docs, 100-300 queries).** K=20 latent topics; per-topic Zipf vocabulary of 300 terms plus a synonym table so paraphrase queries share *no* tokens with their targets; 10% of docs carry rare identifier tokens (`ModelXk7`, `v2.3.1`). "Embedding" = unit-normalised topic-mixture vector + Gaussian noise (sigma adjustable) to mimic a good/bad embedder. Query classes: (a) lexical overlap, (b) paraphrase (V must win), (c) identifier/near-duplicate (T must win). Graded qrels from generator ground truth (rel 2 = target doc, 1 = same-topic near duplicate).
**Properties to assert**
- Hybrid with tuned weights satisfies `nDCG@10 >= max(T-only, V-only) - 0.01` on the mixed query set; CC >= RRF(k=60) by a margin on classes (b)+(c) mixture (hypothesis from [F Bruch]; if it fails on synthetic data, investigate normaliser bugs, not the paper).
- Shift test: change embedder noise (domain shift); record regret of default weights vs tuned for CC and of k=60 vs tuned (k_lex,k_sem) for RRF. Expect CC regret <= RRF regret (hypothesis, [F] qualitative claim only).
- Sample efficiency: tuned weights from 10, 25, 50 queries converge (replicates Bruch's "<5% of data" qualitatively).
- **Metamorphic**: (1) same corpus in 1, 3, 8 segments and after merge/compaction -> identical ranking; (2) LIMIT/OFFSET/`window` 100 vs 400 -> identical order of shared docs under `cc`; (3) deleting then re-adding a doc -> same score; (4) tombstoned docs never appear and never change `M_t` unless they were the max (document and test the recompute); (5) scaling BM25 by a constant (`k1`-independent monotone transform) leaves TM2C2 output unchanged.
- **Oracle diff**: `nx_rank_oracle` (double, scores every survivor, no ANN) vs fast path: identical top-k up to 1e-6 ties when V is exact; with HNSW assert recall@k >= 0.95 of oracle top-k.
- **Explain invariants** (3.4): sum identity, `contrib=w*norm`, normalised in [0,1], JSON round-trips, recomputation from the receipt reproduces the score bit-for-bit in double; fuzz explain with NaN/Inf/zero-norm vectors, empty survivors, single survivor, all-equal scores.
- **Normaliser properties**: monotone in raw score, bounded, invariant to survivors outside the filter, `absolute` independent of other docs.
- **Metric tests**: hand-computed nDCG/MRR/recall on 5-doc lists; ties; no-relevant-docs queries (nDCG=0, MRR=0, no NaN).
- **LTR safety**: with zero clicks the learner returns defaults exactly; with synthetic position-biased click simulator (examination propensity 1/r, true relevance known) IPW learner recovers the ordering of true weights while the naive (unweighted) learner is biased toward top positions.
- **Performance gates**: fusion stage < 1 ms for |U|=300 at d=384; completion stage < 3 ms; explain "full" < 100 microseconds per hit; zero heap allocation outside the arena in the fusion loop.

---------------------------------------------------------------------------------------------------

## 7. References
Fetched [F]:
- Bruch, Gai, Ingber. An Analysis of Fusion Functions for Hybrid Retrieval. arXiv:2210.11934 (2022/2023). https://arxiv.org/abs/2210.11934 ; HTML https://ar5iv.labs.arxiv.org/html/2210.11934 (PDF too large for the fetch tool).
- Dhulipala, Hadian, Jayaram, Lee, Mirrokni. MUVERA: Multi-Vector Retrieval via Fixed Dimensional Encodings. arXiv:2405.19504. https://arxiv.org/abs/2405.19504
- Santhanam, Khattab, Potts, Zaharia. PLAID: An Efficient Engine for Late Interaction Retrieval. arXiv:2205.09707. https://arxiv.org/abs/2205.09707
- Santhanam, Khattab, Saad-Falcon, Potts, Zaharia. ColBERTv2: Effective and Efficient Retrieval via Lightweight Late Interaction. NAACL 2022, arXiv:2112.01488. https://arxiv.org/abs/2112.01488
- Hu, Wang, Peng, Li. Unbiased LambdaMART: An Unbiased Pairwise Learning-to-Rank Algorithm. arXiv:1809.05818. https://arxiv.org/abs/1809.05818
- Elasticsearch RRF: https://www.elastic.co/docs/reference/elasticsearch/rest-apis/reciprocal-rank-fusion
- Elasticsearch linear retriever: https://www.elastic.co/docs/reference/elasticsearch/rest-apis/retrievers/linear-retriever
- Weaviate hybrid search fusion: https://docs.weaviate.io/weaviate/concepts/search/hybrid-search
- Lucene Explanation (9.10.0): https://lucene.apache.org/core/9_10_0/core/org/apache/lucene/search/Explanation.html
Search snippet only [S]:
- Hsu, Tzeng. DAT: Dynamic Alpha Tuning for Hybrid Retrieval in Retrieval-Augmented Generation. arXiv:2503.23013. https://arxiv.org/abs/2503.23013
Recall-only [R] (titles from memory, not fetched; verify before citing externally): Cormack, Clarke, Buettcher SIGIR 2009 (RRF); Khattab and Zaharia SIGIR 2020 (ColBERT); Burges MSR-TR-2008-109 (LambdaMART overview); Joachims et al. 2005 (Skip-Above click interpretation); Joachims, Swaminathan, Schnabel WSDM 2017 (unbiased LTR with biased feedback); Radlinski, Kurup, Joachims CIKM 2008 (interleaving); Carbonell and Goldstein SIGIR 1998 (MMR); Jarvelin and Kekalainen 2002 (nDCG); Platt 1999 (probability calibration); Smucker, Allan, Carterette CIKM 2007 (significance tests); Thakur et al. NeurIPS 2021 (BEIR); Lucene `FeatureField` saturation; Elasticsearch function_score decay functions; Qdrant DBSF; OpenSearch normalization-processor; Vespa phased ranking; MUVERA FDE construction constants; ColBERTv2 residual bit widths and centroid counts; PLAID default `nprobe`/`t_cs`/`ndocs` (the fetched abstract did not state them).
