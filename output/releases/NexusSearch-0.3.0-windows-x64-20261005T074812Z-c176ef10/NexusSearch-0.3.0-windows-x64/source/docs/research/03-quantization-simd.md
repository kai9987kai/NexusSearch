# 03 - Vector quantisation and SIMD distance kernels

Status: research note / spec input. Date: 2026-10-01. Scope: how NexusSearch should compress vectors, estimate distances with SIMD, and
rerank. Every technique carries a source; items I could not fetch are tagged **(recall-only)**. Numbers tagged **(measured)** or
**(simulated)** were produced by me on this machine in scratch code (not in the repo); numbers tagged **(paper)** come from the fetched
primary source; **(derived)** are my arithmetic from paper formulas.

## 0. Decisions in one screen

1. Primary compressed tier = **RaBitQ 1-bit + 4-bit-LUT FastScan** (replaces the naive "sign-bit binary" tier planned in ARCHITECTURE.md).
   It is data-oblivious (no codebook training, only a mean), unbiased, carries a per-pair error bound, and is ~28x smaller than f32.
2. Second tier = **SQ8 in the LVQ style** (per-vector min/max after mean-centring, 8 bit), which is the safe "near-lossless" tier.
3. Upgrade path = **Extended RaBitQ with B = 1 + 4 (5 bit) and 1 + 8 (9 bit)**, staged: the 1-bit code prunes, extra bits refine only survivors.
4. Always keep f16 (or f32) raw rows on the mmap'd segment for exact rerank; compressed codes are in RAM, raw rows are paged.
5. Rerank rule = **error-bound-driven** (lower bound vs current k-th exact distance), with a fixed oversampling factor only as fallback.
6. Kernels: one function-pointer table bound once from `nx_simd_level_get()`; scalar oracle always present; AVX2 and NEON both first-class.
7. Skip PQ/OPQ/AQ/ScaNN/TurboQuant/LSH for v1. PQ4 FastScan shares RaBitQ's kernel, so it stays a cheap stretch item.
8. **Environment finding:** this dev box is a Snapdragon X Plus (ARM64) running x86-64 binaries under emulation (details in 1.2). AVX2 code is
   testable for correctness here, but absolute AVX2 timings are meaningless; NEON is the native path on this hardware.

## 1. Landscape

### 1.1 Who does what

| Method | Source (year) | bits/dim | Bound | Notes |
|---|---|---|---|---|
| RaBitQ | Gao and Long, SIGMOD 2024, https://arxiv.org/abs/2405.12497 (paper) | 1 | yes, O(1/sqrt D), unbiased | beats PQ/OPQ x4fs and HNSW in the authors' IVF benchmarks |
| Extended RaBitQ | Gao, Gou, Xu, Yang, Long, Wong, arXiv Sep 2024 (SIGMOD 2025 per brief, recall-only), https://arxiv.org/abs/2409.09913 (paper) | 1..10 | yes, ~2^-B/sqrt D | asymptotically optimal; beats SQ and LVQ at equal bits |
| RaBitQ-Library | https://github.com/VectorDB-NTU/RaBitQ-Library (README) | 1..9 | yes | Apache-2.0, C++17, x86 AVX2/FMA (AVX-512 optional) and ARM64 NEON; IVF, HNSW, SymphonyQG; README lists integrations in Milvus, Faiss, cuVS, DiskANN, VSAG, Lucene, Elasticsearch, Qdrant, Weaviate, LanceDB, ClickHouse and others (README claim, depth unverified) |
| SAQ | arXiv 2509.12086 (search snippet only) | variable | - | code adjustment + PCA dimension segmentation; claims up to 80% lower error and >80x faster encoding than Extended RaBitQ |
| TurboQuant | arXiv 2504.19874 (title via search only, recall-only) | variable | - | RaBitQ authors' note https://arxiv.org/abs/2604.19528 (abstract fetched; interested party) reports TurboQuant is worse than RaBitQ in most IP/NN/KV-cache tests and that some TurboQuant results did not reproduce |
| LVQ / LVQ-B1xB2 | Aguerrebere, Bhati, Hildebrand, Tepper, Willke, https://arxiv.org/abs/2304.04759 (paper; VLDB 2023 recall-only) | 4/8 (+residual) | none | Intel SVS; per-vector scalar quantisation, graph-friendly |
| Streaming LVQ | arXiv 2402.02044 (abstract) | 4/8 | none | Turbo LVQ +28%, multi-means LVQ +27% (paper abstract) |
| LeanVec | Tepper et al., https://arxiv.org/abs/2312.16335 (abstract) | reduced dims + LVQ | none | linear dim-reduction (ID/OOD variants) then LVQ; up to 3.7x throughput, 4.9x faster build (abstract) |
| PQ FastScan / Quick ADC | Andre, Kermarrec, Le Scouarnec, VLDB 2015 doi:10.14778/2856318.2856324 and ICMR 2017 doi:10.1145/3078971.3078992 (refs from RaBitQ paper); Faiss wiki https://github.com/facebookresearch/faiss/wiki/Fast-accumulation-of-PQ-and-AQ-codes-(FastScan) (fetched) | 0.25..4 | none | 16-entry u8 LUT in a SIMD register via pshufb/tbl |
| BBQ (Elastic/Lucene) | https://www.elastic.co/search-labs/blog/better-binary-quantization-lucene-elasticsearch (fetched) | 1 (+4-bit query) | none stated | centroid-centred, 1-bit docs, 4-bit query, 2 (L2) or 3 (dot) correction floats per vector; ~28x smaller; "20-30x less quantization time, 2-5x faster queries" than PQ (blog claim) |
| Matryoshka (MRL) | Kusupati et al., NeurIPS 2022, https://arxiv.org/abs/2205.13147 (recall-only) | prefix dims | none | only for models trained with MRL |
| SIMD kernel libs | SimSIMD, now NumKong, https://github.com/ashvardanian/SimSIMD (README) | f64..i4, bits | - | C99 core, runtime CPUID dispatch to function pointers, AVX-512/AMX/NEON/SVE/RVV |

### 1.2 Measured environment (this machine)

`PROCESSOR_IDENTIFIER` = ARMv8 Qualcomm, WMI = "Snapdragon X Plus (8-core) @ 3.30 GHz", Architecture 12 (ARM64). GCC 15.2 UCRT64 is an x86-64
toolchain, so everything runs under Windows' x64 emulation, which exposes AVX2/FMA/BMI2/F16C/POPCNT and no AVX-512, no AVX-VNNI,
no AVX512-BF16 (probed with `__builtin_cpu_supports`: avx2=1 fma=1 bmi2=1 f16c=1 avxvnni=0 avx512f=0). Consequences:
- AVX2 kernels can be differential-tested here (semantics are exact); their speed cannot be trusted. My D=768 f32 dot took 229 ns (about
  7 cycles per FMA, roughly 5-10x slower than native). Use **ratios only**, and calibrate cost constants at runtime (section 5, idea 3).
- NEON is the native SIMD on this hardware but cannot be compiled with the installed toolchain. Plan an arm64 CI leg (windows-11-arm,
  ubuntu arm, macOS arm runners: recall-only) or install MSYS2 CLANGARM64 if disk allows (about 2.9 GB free, do not download casually).

## 2. Techniques

### 2.1 RaBitQ (1-bit) - Gao and Long, SIGMOD 2024 (paper, fetched PDF)

Idea: normalise vectors against a centroid, rotate randomly, keep only the sign of every coordinate (D bits), and correct the resulting
bias with an exactly known per-vector scalar, which yields an unbiased inner-product estimator with a provable error bound.

Notation: D' = D padded with zeros to a multiple of 64 (paper default; more padding = more bits = more accuracy). P = random orthogonal
D' x D' matrix, one per field. c = centroid (paper: IVF cluster centre; we use the segment mean).

Index (per vector o_r):
```
r = o_r - c;  n_o = |r|;  o = r / n_o          (n_o == 0 => flag, est distance = |q_r - c|^2 exactly)
o' = P^T o                                      (rotate)
x_b[i] = (o'[i] > 0)                            (D' bits)
oo  = sum_i |o'[i]| / sqrt(D')                  (= <obar,o>, concentrates at 0.798..0.800 for D in 1e2..1e6)
store: x_b, n_o, inv_oo = 1/oo, err_o = sqrt(1-oo^2)/oo, pc = popcount(x_b)
```
Query (per query, per centroid):
```
r = q_r - c;  n_q = |r|;  q' = P^T (r / n_q)
vl = min q', vr = max q', delta = (vr - vl)/15                  (Bq = 4 bits)
qu[i] = floor((q'[i]-vl)/delta + u_i),  u_i ~ U[0,1)            (RANDOMISED rounding; needed for unbiasedness)
K1 = 2*delta/sqrt(D'),  K2 = 2*vl/sqrt(D'),  K3 = delta/sqrt(D')*sum(qu) + sqrt(D')*vl
```
Per candidate: `S = sum_i x_b[i]*qu[i]` (integer, see 2.2), then
```
ip_hat = (K1*S + K2*pc - K3) * inv_oo                           (estimates <o,q> of the unit vectors)
d_hat  = n_o^2 + n_q^2 - 2*n_o*n_q*ip_hat                        (squared L2)
d_lb   = d_hat - 2*n_o*n_q * eps0 * err_o / sqrt(D'-1)           (lower bound)
```
Bound (paper Thm 3.2): P(|ip_hat - <o,q>| > sqrt((1-oo^2)/oo^2) * eps0/sqrt(D-1)) <= 2 exp(-c0 eps0^2). Default eps0 = 1.9, Bq = 4
(Bq = Theta(log log D) suffices). IP/cosine form (paper footnote 8): `<o_r,q_r> = n_o*n_q*ip_hat + <o_r,c> + <q_r,c> - |c|^2`; store `<o_r,c>`
per vector, `|c|^2` per segment. Rotating the centroid once at build time (c' = P^T c) lets one query rotation serve every probed cluster
(q'_k = (P^T q_r - c'_k)/n_qk), saving nprobe x D'^2 FMAs (derived, linearity).

Gains (paper): same or better accuracy than PQ/OPQ x4fs with half the code (D vs 2D bits); about 3x faster than PQ/OPQ in single-code mode at equal
accuracy; max relative error of squared distance <= 40% on all six datasets, while PQx4fs/OPQx4fs reach ~100% on four of six (and fail "disastrously" on
MSong, D=420); index time on GIST 117 s (RaBitQ) vs 105 s (PQ) vs 291 s (OPQ) with 32 threads; IVF with 4096 clusters at 1M scale, K=100, single
thread, AVX2. Pitfalls: (a) the proof needs a Haar-random P and the centroid-normalised unit vector; a weak rotation (identity, block-diagonal
on structured data) breaks it silently; (b) rounding the query deterministically instead of randomly adds bias; (c) the bound is on the *pair*, not
the ranking - borderline neighbours still need exact rerank; (d) the bound covers only quantisation error, not error from truncating dimensions.

Measured on random unit vectors with a dense Haar rotation **(simulated, N=200-400, Q=100-200)**:

| D | B | mean oo | bias | std(err)*sqrt(D) | 99.9% quantile of abs err | paper 5.75*2^-B/sqrt D | est-vs-true slope |
|---|---|---|---|---|---|---|---|
| 128 | 1 | 0.7986 | +4e-5 | 0.754 | 0.227 | 0.254 | 0.997 |
| 768 | 1 | 0.7978 | +4e-4 | 0.758 | 0.0925 | 0.104 | 0.996 |
| 768 | 2 | 0.939 | ~0 | - | 0.0434 | 0.0519 | 0.995 |
| 768 | 3 | 0.9814 | ~0 | - | 0.0230 | 0.0259 | 1.000 |
| 768 | 4 | 0.9944 | ~0 | - | 0.0129 | 0.0130 | 0.999 |
| 768 | 5 | 0.9984 | ~0 | - | 0.0068 | 0.0065 | 1.001 |
| 768 | 7 | 0.9999 | ~0 | - | 0.0019 | 0.0016 | 1.000 |

So: std of the IP estimate ~= 1.5 * 2^-B / sqrt(D'), and **eps0 behaves like a number of standard deviations**. One-sided miss rate (true
neighbour wrongly pruned by the lower bound), B=1, identical at D=128 and 768: eps0=1.0 -> 16%; 1.5 -> 6.7%; **1.9 -> 2.8%**; 2.5 -> 0.57%;
**3.0 -> 0.12%**; 3.8 -> 0.004%. The paper's eps0=1.9 gives near-perfect *NN recall* because only borderline candidates can be hurt, but a per-pair
guarantee needs eps0 about 3.0-3.8. Expose eps0 as a knob: balanced 1.9, high-recall 3.0.

### 2.2 4-bit LUT FastScan kernel (André et al. 2015/2017; Faiss; used verbatim by RaBitQ-1)

Faiss facts (wiki, fetched): 4-bit codes in blocks (bbs=32) interleaving sub-quantiser pairs; 16 x uint8 LUT per sub-quantiser held in a SIMD
register (pshufb/vpshufb, NEON supported); LUT entries quantised to u8, 16-bit accumulators; `PQ32x4fs`, `IndexPQFastScan`, `IndexIVFPQFastScan`;
claims up to 1M QPS without rerank and 280k QPS at 1-recall@1=0.9, ~2x HNSW with 2.7x less memory (benchmark dataset not captured by my fetch).
RaBitQ authors: the speed of PQ in memory is "largely attributed to the fast SIMD-based implementation".

RaBitQ-1 needs no LUT quantisation: with qu in 0..15, `lut[j][m] = sum_t bit_t(m)*qu[4j+t] <= 60`, exact. Worst-case sum S = 15*D' must fit u16
=> D' <= 4369 (guard: if D' > 4096 flush u16 accumulators into u32 every 4096 dims).

Layout (mine; Faiss uses a permuted variant, permutation constants recall-only): block = 32 vectors, `D'/8` rows of 32 bytes (4*D' bytes/block);
row r holds chunks j=2r (lane 0 = bytes 0..15) and j=2r+1 (lane 1 = bytes 16..31); byte i of a lane: low nibble = 4-bit chunk of vector i, high
nibble = chunk of vector i+16; nibble bit t = dimension 4j+t. LUT stored as `D'/8` rows of 32 bytes (lut_j then lut_{j+1}).

AVX2 kernel - **verified bit-exact against the scalar oracle** (20 random blocks + all-ones/all-15 worst case, D'=768, emulated AVX2):
```c
NX_TARGET("avx2")
static void nx_fs_scan_avx2(const uint8_t *blk, const uint8_t *lut, size_t D, uint16_t out[32]) {
    const __m256i m4 = _mm256_set1_epi8(0x0F), m16 = _mm256_set1_epi16(0x00FF);
    __m256i a0 = _mm256_setzero_si256(), a1 = a0, a2 = a0, a3 = a0;
    for (size_t r = 0; r < D / 8; r++) {
        __m256i c  = _mm256_loadu_si256((const __m256i *)(blk + r * 32));
        __m256i lt = _mm256_loadu_si256((const __m256i *)(lut + r * 32));
        __m256i x = _mm256_shuffle_epi8(lt, _mm256_and_si256(c, m4));                        /* vectors 0..15  */
        __m256i y = _mm256_shuffle_epi8(lt, _mm256_and_si256(_mm256_srli_epi16(c, 4), m4));  /* vectors 16..31 */
        a0 = _mm256_add_epi16(a0, _mm256_and_si256(x, m16));  a1 = _mm256_add_epi16(a1, _mm256_srli_epi16(x, 8));  /* even / odd */
        a2 = _mm256_add_epi16(a2, _mm256_and_si256(y, m16));  a3 = _mm256_add_epi16(a3, _mm256_srli_epi16(y, 8));
    }
    __m128i e0 = _mm_add_epi16(_mm256_castsi256_si128(a0), _mm256_extracti128_si256(a0, 1));  /* lane0 + lane1 */
    __m128i o0 = _mm_add_epi16(_mm256_castsi256_si128(a1), _mm256_extracti128_si256(a1, 1));
    __m128i e1 = _mm_add_epi16(_mm256_castsi256_si128(a2), _mm256_extracti128_si256(a2, 1));
    __m128i o1 = _mm_add_epi16(_mm256_castsi256_si128(a3), _mm256_extracti128_si256(a3, 1));
    _mm_storeu_si128((__m128i *)(out +  0), _mm_unpacklo_epi16(e0, o0));  _mm_storeu_si128((__m128i *)(out +  8), _mm_unpackhi_epi16(e0, o0));
    _mm_storeu_si128((__m128i *)(out + 16), _mm_unpacklo_epi16(e1, o1));  _mm_storeu_si128((__m128i *)(out + 24), _mm_unpackhi_epi16(e1, o1));
}
```
NEON mapping (same layout, same integer output; written, not compiled here): per row load `c0,c1` (16 B each) and `l0,l1`;
`x0 = vqtbl1q_u8(l0, vandq_u8(c0, m4))` (vectors 0..15), `y0 = vqtbl1q_u8(l0, vshrq_n_u8(c0, 4))` (16..31), likewise chunk 2r+1 with `c1,l1`;
accumulate with `vaddw_u8`/`vaddw_high_u8` into u16x8 accumulators (natural vector order, no even/odd fix-up).
Measured, emulated AVX2, D=768 **(measured, ns per vector)**: FastScan 23.4 (131k vectors, 12.6 MB of codes) to 27.4 (64 vectors, cache-resident) vs f32 dot 229. A variant
that sums 4 rows in u8 (4*60=240 < 255) before widening was *slower* here (26-33 ns); retest on native hardware, do not adopt blindly. Uop-count estimate
for native AVX2: ~15 uops per 8 dims x 32 vectors, ~11 cycles/vector at D=768, i.e. ~3-4 ns/vector/core **(derived, unmeasured)**.

### 2.3 Extended RaBitQ (B bits/dim) - Gao et al. 2024 (paper, fetched PDF)

Codebook: grid `G = {(-(2^B-1)/2 + u)_{u in 0..2^B-1}}^D`, normalise each grid point, rotate by P. B=1 is exactly RaBitQ. Code `y_u[i]` in `0..2^B-1`;
centred value `ybar = y_u - cB`, `cB = (2^B-1)/2`.

Encode (nearest codebook vector, O(2^B D log D)): maximise `<y,o'>/|y|` by sweeping a scale t and rounding `t*|o'|`; critical values change one coordinate at a time.
```
a = |o'| ; M = 2^(B-1)                                  (magnitude levels 0..M-1, |ybar| = m + 0.5)
events = { (t = m/a[i], i, m) : i in dims, m = 1..M-1 }  sorted ascending (min-heap or sort)
ip = 0.5*sum(a); n2 = 0.25*D'; best = ip/sqrt(n2); k = 0
for each event e in order: ip += a[i]; n2 += 2*m; if ip/sqrt(n2) > best: best = ip/sqrt(n2); k = index(e)
apply first k events -> level m_i;   y_u[i] = (o'[i] > 0) ? M + m_i : M-1-m_i     (MSB of y_u == the 1-bit RaBitQ code)
store f_v = 1/ip_final,  err_o = sqrt(1-oo^2)/oo with oo = ip_final/sqrt(n2_final),  n_o
```
Estimator: `ip_hat = f_v * (<y_u, q'> - cB * sum(q'))` (the |ybar| norm cancels: derived and checked in my simulation, slope 0.995-1.001). Paper stores two floats per
vector, `|o_r-c|` and `1/(|y_u|*<o,obar>)`. Staged search (paper section 4.2): y_u = 2^(B-1)*y0 + y_last, y0 = 1-bit code. Stage 1: FastScan on y0 gives
`<y0,q'> ~= delta*S + vl*pc` (dequantised, reused), lower bound prunes; stage 2 on survivors only: `<y_u,q'> = 2^(B-1)*<y0,q'> + <y_last,q'>`.
B=5 => y_last is a nibble (u4 x f32 query), B=9 => y_last is a byte; other B split into power-of-two parts (code for B=3,4,7,8 in their repo).
Because y_last <= 15 for B=5, an int8-quantised query allows the safe `_mm256_maddubs_epi16` path (pair sum <= 2*15*127 = 3810, no saturation) - derived idea, untested.

Gains (paper): empirical error `eps < 5.75*2^-B/sqrt(D)` with >99.9% probability (confirmed above); **without any raw-vector rerank, 4/5/7 bits reach >90/95/99% recall on all tested
datasets** (K=100, IVF), 3 bits already >95% on OpenAI-1536/3072 embeddings; 5 bit = 6.4x, 7 bit = 4.5x compression; vs LVQ at equal bits the
average relative error of LVQ is 1.3-3.1x larger for B>6 and orders of magnitude larger at B=1,2 (kernels cost the same); PQ/OPQ do not beat SQ/LVQ at >=4 bits/dim.
MSMARCO 113.5M x 1024: B=4 gives >95% recall with 56.9 GB vs 433.5 GB raw; quantising with B=9 took ~2 h. Encoding time for 1M x 3072 on 96 threads:
43.8 s (B=1), 64.6 s (B=5), 98.3 s (B=7), 233 s (B=9), 418 s (B=10). Extrapolation to our box, D=768, B=5: ~0.3-0.8 ms/vector/core (derived, rough).
Pitfalls: encode cost grows 2^B; sorting D'*(M-1) events per vector dominates for B>=7; the proof needs the *nearest* grid point, so an approximate
(few-scale) encoder must be re-validated for bias (slope of est vs true) before use.

### 2.4 SQ8 / LVQ / LVQ-B1xB2 / LeanVec (Intel SVS; paper, fetched PDF)

LVQ-B: `mu` = dataset mean; per vector `r = x - mu`, `l = min r`, `u = max r`, `delta = (u-l)/(2^B-1)`, `code_i = round((r_i - l)/delta)`;
reconstruction `x_hat = mu + l + delta*code`. Constants l,u in f16 (B_const=16) in the paper. Footprint `ceil((d*B + 2*B_const)/8/p)*p` with p=32 B pad;
compression 3.84x (d=96) and 3.98x (d=768) at B=8 without padding. Per-vector range uses ~100% of the code space whereas global/per-dimension
bounds use ~60%/75% (paper Fig. 2). Two-level LVQ-B1xB2: second level quantises the residual r - Q(r) in [-delta/2, delta/2) with B2 bits and needs no
extra constants (footprint + d*B2); level 1 drives traversal/scan, level 2 is only a rerank. Model update is cheap (recompute mu, re-encode, linear in n).
Kernel facts: decompress fused with the distance, static dimension gives up to 32% speedup, ~2x faster similarity overall; when data exceed L2 LVQ-8 is
up to 2.12x faster than float16 (bandwidth), while for cache-resident data float16 is 2x faster than LVQ-4. Graph reported gains: up to 20.7x throughput
with 3x less memory (low-memory regime), 5.8x with 1.4x less memory (high-throughput), billion-scale.

Scoring without decompressing: `<x_hat,q> = <mu,q> + l*sum(q) + delta*<code,q>` (per-query constants `<mu,q>`, `sum(q)`); L2 needs per-vector `|x_hat|^2` (store f32).
LeanVec = project to d' < d with a linear map (ID: data-only PCA-like; OOD: query-aware, details recall-only), then LVQ; search in d', rerank in full-d LVQ.

### 2.5 Binary quantisation with rerank; BBQ

Naive sign quantisation `hamming(a,b) = popcount(a xor b)` estimates the angle only if data are centred and rotated, and is biased otherwise. BBQ (blog): centre on
the centroid, 1-bit docs, **4-bit query**, per-vector correction floats (2 for L2, 3 for dot), popcount arithmetic. Reported (blog, via fetch summary, verify): ~28x
memory reduction, 90%+ recall on CohereV3 1M with 3x oversampling, 74% recall on E5-small 500K at 1 bit (no oversampling), 138M vectors at 70% recall 451 QPS /
90% recall 168 QPS. RaBitQ-1 is the same structure with a proved estimator and bound, hence the replacement of naive BQ.
Oversampling rule of thumb when no bound is usable: rerank `R = max(4k, k+32)` best-by-estimate (3x from the blog; 4x chosen as slack, **unvalidated**, tune in tests).

### 2.6 PQ / OPQ / 4-bit FastScan (Jegou et al. TPAMI 2011, Ge et al. CVPR 2013: recall-only)

Sub-vector k-means codebooks. RaBitQ paper evidence: PQx4fs/OPQx4fs have max relative error ~100% on 4 of 6 datasets and fail on MSong; LSQ indexing exceeded 24 h on GIST.
Only advantage: below 1 bit/dim (e.g. 0.25 bit/dim = 24 B for D=768) where RaBitQ cannot go.

### 2.7 Matryoshka truncation and PCA reduction

If a model was trained with MRL (OpenAI text-embedding-3, Nomic and others: recall-only), keep the first d1 coordinates, re-normalise, quantise those, then rerank with
full dimension. For non-MRL embeddings use PCA (LeanVec-ID style) to d1. Truncation error is *not* in the RaBitQ bound, so bound-driven pruning is invalid
after truncation: switch to fixed oversampling. Store `mrl_dims` per field; no authors' numbers fetched, model-dependent.

### 2.8 SIMD kernels and dispatch (what the table has to contain)

Measured on this box (emulated, D=768, ns/vector; relative values only): f32 dot (4 accumulators) 229; f16->f32 via F16C 411-435; bf16->f32 (shift) 428-448; i8 dot via
`cvtepi8_epi16 + madd_epi16` 221-236; u8 code x f32 query 385-431; xor+popcnt64 over 12 words 14-22; AVX2 nibble-LUT AND-popcount 25-31; FastScan 23-27 per vector.
Observations: (1) scalar popcnt wins over the AVX2 LUT popcount at 12 words (Muela-Kurz-Lemire style popcount needs >= ~512 B vectors, recall-only); (2) RaBitQ's single-code
path needs Bq=4 AND-popcounts (4x the xor-popcnt cost ~ 58-90 ns here) so batched FastScan is ~2.5-3.5x faster; (3) halving storage (f16) did not halve compute: conversion costs
dominate when cache-resident (LVQ paper saw the same: f16 2x faster than LVQ-4 in cache, LVQ 2.12x faster out of cache).

Kernel list and pitfalls:
- f32 dot/L2: 4 independent accumulators, `fmadd`, pad rows to a multiple of 32 floats with zeros so no tail loop exists; horizontal add once per vector.
- f16: `_mm256_cvtph_ps` (F16C); NEON `vcvt_f32_f16`. bf16: `slli_epi32(cvtepu16_epi32(x),16)`; NEON `vshll_n_u16(x,16)`. f16 overflows above 65504, bf16 keeps range but only 8 bits of mantissa.
- int8 dot: **do not use `_mm256_maddubs_epi16` with u8 x i8 operands** - it saturates (2*255*127 = 64770 > 32767). Use sign-extend + `madd_epi16` (verified exact incl. -128 x -128), or restrict one side to <= 127 (7-bit codes) / <= 15 (u4). On AVX-VNNI/AVX512-VNNI use non-saturating `dpbusd`; NEON `vdotq_s32` (FEAT_DotProd) else `vmull_s8 + vpadalq_s16`. Not on this box (avxvnni=0): add `avx_vnni` (CPUID.7.1:EAX bit 4) and NEON dotprod/bf16/i8mm bits to `nx_cpu_features` later (Windows `IsProcessorFeaturePresent`, Linux HWCAP, macOS sysctl: recall-only).
- popcount: `_mm_popcnt_u64` unrolled x4; MSVC `__popcnt64`; NEON `vcntq_u8` + `vaddvq_u8`; wrap in `nx_popcnt64` in nx_config.h.
- Dispatch granularity: bind **batch** kernels (`scan_n(query, base, n, d, out)`) not per-vector calls, so an indirect call is amortised and prefetch can run ahead.
- GCC rule: helper `static inline` functions called from `NX_TARGET("avx2,fma")` code need the same target attribute or inlining fails; MSVC needs no attribute for intrinsics; `NX_TARGET` is already empty there.
- Avoid `alignas(32)` stack arrays and big `__m256` aggregates in MinGW-w64 target functions (long-standing Win64 AVX stack-alignment bug, GCC bugzilla 54412, recall-only); my kernels use only `loadu/storeu` and <= 8 live ymm values and passed.
- Denormals: f16 -> f32 conversions of tiny values can hit slow paths; test with denormal inputs; FTZ/DAZ would break scalar-oracle equality, so do not set it blindly.
- `long` is 32-bit on Windows: all index arithmetic `size_t`, all counts `uint64_t`.
- Existing `src/core/nx_simd.c` already does GCC `__builtin_cpu_supports` and MSVC `__cpuid/__cpuidex/_xgetbv` with an XCR0 check, plus the `NX_SIMD` env cap; extend rather than replace.

### 2.9 Norm and metric handling

- **Cosine**: L2-normalise in f32 *before* any quantisation; `cos = ip`, `L2^2 = 2 - 2 ip`. Store the original norm only if the field is also queried by IP. Zero vector: store flag, score 0, never divide.
- **L2**: RaBitQ form above; flat rerank uses `|o|^2 + |q|^2 - 2<o,q>` with precomputed `|o|^2`, clamp at 0; centre by the segment mean first to limit cancellation (LVQ and RaBitQ both do).
- **IP / MIPS**: RaBitQ footnote-8 formula (per-vector `<o,c>`); error scales with `n_o*n_q`, so heavy-tailed norms widen intervals - the bound reports this honestly. Do not use the norm-augmentation MIPS->L2 trick (recall-only) with RaBitQ.
- NaN/Inf rejected at ingest; f16 overflow rejected or rescaled; `n_o < 1e-12` treated as zero.

## 3. Adopt for NexusSearch (ordered, concrete)

Config per `vector` field (extends ARCHITECTURE.md): `dims`, `metric`(cosine|ip|l2), `dtype`(f32|f16|bf16, default f16), `quant`(none|sq8|rabitq1|rabitq5|rabitq9, default rabitq1 for
n >= 4096 else none), `rerank`(bound|oversample, default bound), `eps0`(1.9), `oversample`(4, fallback only), `bq`(4), `pad`(64), `mrl_dims`(optional).

Step A0 - kernels (src/index/vec/nx_vec_kernels.{h,c}, nx_vec_k_avx2.c, nx_vec_k_neon.c): table `nx_vec_kernels` = {dot_f32, l2sq_f32, norm2_f32, dot_f16_f32, dot_bf16_f32, dot_u8_f32, dot_i8, xor_popcnt, and_popcnt, fs_scan_block, fs_build_lut};
`nx_vec_kernels_get()` bound once; `nx_vec_kernels_for(level)` for tests; scalar oracle first. Rows 64-byte aligned, padded to 32 elements.
Step A1 - `flat` exact over f16/f32 rows with a typed top-k heap and cosine pre-normalisation.
Step A2 - `sq8` (LVQ-8): per-segment mean, per-vector l,delta (f32), `|x_hat|^2` (f32); stage-1 `delta*<code,q>` plus constants; oversample 2 then exact rerank.
Step A3 - `rabitq1` flat: per-field seeded dense rotation (QR of Gaussian in double; D'=768 -> 2.4 MB, 1536 -> 9.4 MB, 3072 -> 38 MB, stored once per field, not per segment), per-segment centroid, block packer,
LUT builder, `nx_fs_scan_*`, estimator and bound as in 2.1, **two-pass rerank**: (1) scan all blocks keeping est/lb; (2) exact-rerank the best R0 = 4k by est to get tau = k-th exact distance;
(3) exact-rerank every remaining candidate with lb < tau, ascending lb, tightening tau. (Paper streams clusters; two-pass avoids the cold-start flood of reranks.) Flat scan only up to ~262,144
vectors per segment: 1-bit scan is ~0.1-1 ms per 100k vectors native (derived), so no k-means is needed yet.
Step A4 - IVF for larger segments: `nlist = round(4*sqrt(n))` (4096 at 1M, paper/Faiss), mini-batch k-means on a sample of >= 39*nlist points (Faiss minimum, recall-only), `nprobe` tuned by EXPLAIN ANALYZE, rotated centroids stored, per-cluster centroid
used for normalisation. Filter bitmap -> per-block 32-bit mask: skip blocks with mask 0, never rerank masked-out lanes; tail lanes of the last block are padded and must be masked.
Step A5 - `rabitq5` (1 + nibble) and `rabitq9` (1 + byte) with Algorithm 1 encoder and the staged estimator; per-vector layout `[nibble/byte rows 64-B aligned][f_v, err_o, n_o, (<o,c>)]`. Needed only for "no raw rows in RAM" or high-recall mode.
Step A6 - MRL/PCA truncation stage, structured rotation (3 rounds of random sign + fast Walsh-Hadamard over padded 2^k dims, O(D log D); validate with the statistical tests below), faster encoder.
Defaults to ship: Bq=4, eps0=1.9 (balanced) / 3.0 (high recall), D' multiple of 64, code blocks 32 vectors, u16 accumulators with the D'>4096 guard, rerank candidates read from mmap with prefetch.
Memory at 1M x 768: f32 3.07 GB; f16 1.54 GB; SQ8 0.78 GB; RaBitQ-5 0.49 GB; RaBitQ-1 0.108 GB (96 B code + ~12 B factors = 28x) - raw rows stay on disk/mmap.

## 4. Skip and why

- PQ/OPQ/LSQ/AQ: training cost, no bound, ~100% max relative error on several datasets, slower than SQ at >=4 bits. Revisit only for <1 bit/dim; FastScan kernel is shared.
- ScaNN anisotropic loss: RaBitQ authors report its edge vanishes once PQ gets the same SIMD (paper claim).
- TurboQuant: disputed by RaBitQ authors (interested party); RaBitQ is open, bounded and widely integrated.
- LSH: orders of magnitude worse than quantisation (RaBitQ paper citing prior studies).
- AVX-512, AMX, AVX-VNNI kernels now: absent here; dispatch table leaves room.
- LeanVec-OOD learned projections, SymphonyQG/graph-on-RaBitQ: later, after IVF+flat is solid (RaBitQ paper: graphs "require much more effort" because batches of 32 are hard to form).
- fp8 and sub-byte floats: no evidence of benefit for retrieval storage in the fetched sources.

## 5. Novel ideas specific to NexusSearch

1. **Filter-aware FastScan.** The structured result bitmap becomes a per-block lane mask; planner chooses exact f32 over survivors vs FastScan by cost: exact = |S|*T_f32, FastScan = nonzero_blocks*32*T_fs with nonzero_blocks = N/32*(1-(1-s)^32) for random selectivity s. With T_f32/T_fs ~ 10 the crossover is s ~ 10% (derived); clustered filters (sorted ordinals) shift it.
2. **Bound-aware hybrid rerank.** RaBitQ gives each vector an interval [est - eps0*sigma, est + eps0*sigma]; propagate it through the fusion weights to a score interval and exact-rerank only candidates whose interval straddles the k-th fused score. EXPLAIN shows "vector 0.713 +- 0.021 (B=1) -> exact 0.704".
3. **Self-calibrating kernel costs.** At first use, micro-benchmark each kernel for the field's D' (a few ms) and feed ns/vector into the planner. Mandatory on this box where emulated and native costs differ by ~5-10x.
4. **Online eps0 calibration.** Every exact rerank also yields (exact - est)/sigma; track the tail rate and raise eps0 when it exceeds target (free, no extra compute).
5. **Percolator by role swap.** FastScan is symmetric: pack the registered WATCH query vectors as the "database" blocks and treat each incoming document as the "query" (one rotation + LUT, ~3-4 ns per subscription native, derived).
6. **Bit-identical ranking across platforms.** The u16 FastScan sums are exact integers, so scalar/AVX2/NEON/federated nodes agree exactly on stage-1 scores if the float tail is computed in a fixed order: differential tests can assert equality, not epsilon.
7. **Heat-tiered precision.** Merges re-encode anyway (centroid changes): hot segments keep rabitq5 + f16, cold segments rabitq1 + raw on disk, driven by the adaptive-indexing policy.
8. **Data-oblivious NRT.** Encoding needs only the segment mean and the per-field rotation, so flush/refresh never trains anything; merge = re-encode from raw.

## 6. Validation and test ideas

- Kernel differential tests vs scalar oracle at dims {1,3,7,8,15,16,31,33,127,129,768,1536,3072}, under `NX_SIMD=scalar|avx2`: integer kernels (FastScan, popcnt, i8) must match exactly; float kernels within `1e-5 * sum|a_i b_i|`. Include -128 x -128, all-ones/all-15 FastScan, denormals.
- Layout tests: pack/unpack round trip, n % 32 != 0 tails never surface as hits, D' > 4096 u32-flush path, 64-B alignment of mmap rows, corruption fuzz on the vector section (counts x stride overflow-checked before allocation).
- Statistical tests with fixed seeds (the table in 2.1 is the reference): slope of est vs true in 1 +- 0.01; std*sqrt(D') in 0.75 +- 0.05 for B=1; mean oo = 0.80 / 0.94 / 0.98 / 0.995 / 0.9985 for B=1..5; one-sided miss rate at eps0=1.9 in [1.5%, 4%], at 3.0 in [0.05%, 0.3%]. These catch weak rotations, deterministic query rounding and encoder bias.
- Adversarial data: one-hot, constant, duplicates, vector equal to the centroid (n_o = 0), zero vectors, huge/tiny norms, all-negative coordinates, D=1..8.
- Recall regression vs `flat` on synthetic power-law-spectrum clustered data (D in {96,384,768,1536}; spectrum ~ 1/k, random basis, cluster centres), plus a small real embedding set built locally (no big downloads). Plot recall@10 vs eps0 and vs oversample; assert the bound-driven rerank never loses to oversample=4 at equal reranks.
- Rerank accounting in EXPLAIN ANALYZE: blocks scanned/skipped, lb-pruned, reranked, tau, observed bound-violation rate.
- NEON: run the same suite on arm64 CI; on this box only scalar and emulated AVX2 can execute.
- Benchmarks (`bench/bench_vec_kernels.c`): ns/vector and GB/s per kernel, cache-resident vs RAM-resident, p50/p95, recorded with the CPU model string so emulated runs are labelled.

## 7. References

Fetched and read: RaBitQ https://arxiv.org/abs/2405.12497 (PDF read); Extended RaBitQ https://arxiv.org/abs/2409.09913 (PDF read); LVQ https://arxiv.org/abs/2304.04759 (PDF read);
Streaming LVQ https://arxiv.org/abs/2402.02044 (abstract); LeanVec https://arxiv.org/abs/2312.16335 (abstract); RaBitQ-Library https://github.com/VectorDB-NTU/RaBitQ-Library (README);
Faiss FastScan wiki https://github.com/facebookresearch/faiss/wiki/Fast-accumulation-of-PQ-and-AQ-codes-(FastScan); Elastic BBQ blog https://www.elastic.co/search-labs/blog/better-binary-quantization-lucene-elasticsearch;
SimSIMD/NumKong https://github.com/ashvardanian/SimSIMD (README); Revisiting RaBitQ and TurboQuant https://arxiv.org/abs/2604.19528 (abstract).
Search-snippet only: SAQ https://arxiv.org/abs/2509.12086; TurboQuant https://arxiv.org/abs/2504.19874.
Cited by reference list of fetched papers: Andre et al. VLDB 2015 https://doi.org/10.14778/2856318.2856324; Andre et al. ICMR 2017 https://doi.org/10.1145/3078971.3078992.
Recall-only: Matryoshka https://arxiv.org/abs/2205.13147; Muela, Kurz, Lemire, "Faster Population Counts Using AVX2 Instructions" arXiv:1611.07612; Intel SVS https://github.com/intel/ScalableVectorSearch;
PQ (Jegou et al. 2011), OPQ (Ge et al. 2013), GCC bug 54412, Faiss 39-points-per-centroid warning, FastScan permuted layout constants, Windows/Linux/macOS ARM feature-detection constants.
Code from the authors (links as given in the papers): https://github.com/gaoj0017/RaBitQ, https://github.com/VectorDB-NTU/Extended-RaBitQ (not read).
