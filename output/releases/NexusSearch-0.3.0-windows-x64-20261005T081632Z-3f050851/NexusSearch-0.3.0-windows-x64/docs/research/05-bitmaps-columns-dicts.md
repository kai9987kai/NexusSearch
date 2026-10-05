# 05 - Bitmaps, bit-sliced indexes, posting compression, term dictionaries

Research date: 2026-10-01. Scope: the "structured side" of NexusSearch: how each query clause becomes a
bitmap, how numeric range clauses are evaluated, how posting lists and term dictionaries are laid out in a
flat mmap-able segment.

Evidence legend used throughout (be strict about it when implementing):
- [F] fetched and read in this session (paper/abstract/README/blog/source). Numbers quoted are from the source.
- [V] verified locally by running code (scripts in the session scratchpad; results quoted).
- [R] recall-only: from memory, NOT fetched. Treat as a lead to verify, not a spec.
- [E] my own back-of-envelope estimate or design proposal (derivation shown).

---------------------------------------------------------------------------------------------------

## 1. Landscape

| Area | Who / what exists | State of the art in one line |
|---|---|---|
| Compressed bitmaps | Roaring (CRoaring C library, Java, Go, Rust...) used by Lucene, Spark, Kylin, Druid, Elasticsearch, Netflix Atlas [F: arXiv 1603.06549, 1709.07821]. WAH/Concise/EWAH (RLE schemes) are the predecessors. | Roaring with run containers is the de-facto standard: "up to two orders of magnitude" faster than WAH/Concise/EWAH and smaller [F]. |
| Range predicates over numbers | Bit-sliced index (BSI): O'Neil and Quass SIGMOD 1997 [R]; BSI arithmetic + top-k: Rinfret, O'Neil, O'Neil SIGMOD 2001 [F]; Roaring Go BSI package [F]; Aug 2026 arXiv 2608.26368 "Direct-Operable SIMD Bit-Slicing" [F abstract only]. | k bit-slices turn `<, <=, >, >=, =, between` into O(k) bitmap ops, independent of row count (per row: ~k/64 word ops). |
| Block skipping | Zone maps / Small Materialized Aggregates (Moerkotte VLDB 1998) [R]; Parquet page index, Lucene/BKD, DuckDB min-max [R]. | Per-block min/max/null-count; free, tiny, composes with everything. |
| Posting compression | SIMD-BP128 / SIMD-FastPFOR (Lemire-Boytsov 2012/2015) [F]; Stream VByte (Lemire-Kurz-Rupp 2017) [F]; Elias-Fano / Partitioned EF (Vigna 2013, Ottaviano-Venturini 2014) [R]; survey Pibiri-Venturini ACM CSUR 2021 [F abstract]. | Block-wise delta + SIMD bit-packing for speed; EF for random-access/nextGEQ and for monotone offset arrays. |
| Term dictionaries | FST (BurntSushi `fst` crate; Lucene/tantivy) [F blog]; Adaptive Radix Tree (Leis et al. ICDE 2013) [F]; front-coded blocks with binary search (Lucene BlockTree variants, Martinez-Prieto et al. 2016 "Practical compressed string dictionaries") [R]; Marisa / double-array tries [R]. | FST = smallest and the only one that also does regex/Levenshtein; front-coded blocks = simplest, within ~1.5-2x of FST on typical terms; ART = best in-memory mutable ordered dictionary. |

Key architectural conclusion: two posting representations coexist. **Filter postings** (structured fields,
set algebra, vector pre-filtering) want Roaring (word-parallel AND/OR/ANDNOT). **Ranked text postings**
(BM25 with tf, block-max skipping) want delta+bit-packed blocks. Lucene, Tantivy and PISA all converge on
this split [R].

---------------------------------------------------------------------------------------------------

## 2. Techniques

### 2.1 Roaring bitmaps (32-bit) - format, containers, algorithms

Sources: RoaringFormatSpec README [F] https://github.com/RoaringBitmap/RoaringFormatSpec ;
Lemire et al. "Consistently faster and smaller compressed bitmaps with Roaring" 2016 [F abstract]
https://arxiv.org/abs/1603.06549 ; "Roaring Bitmaps: Implementation of an Optimized Software Library"
(CRoaring) 2017/2018 [F abstract] https://arxiv.org/abs/1709.07821.

Core idea: split the 32-bit docid space into chunks of 2^16 = 65,536 ids by the high 16 bits. Each non-empty
chunk is one container chosen to be the smallest representation of its low-16-bit set:
- ARRAY: sorted u16[], used while cardinality <= 4096 (2 bytes/id; 4096*2 = 8192 B equals the bitset size, hence the threshold) [F].
- BITSET: 1024 x u64 = 8 KB fixed, bit j of word j/64 [F].
- RUN: u16 n_runs, then n_runs pairs (start, length-1) [F]; size = 2 + 4*n_runs bytes. Beats ARRAY when
  4*n_runs+2 < 2*card and beats BITSET when n_runs <= 2047 (4*2047+2 = 8190 < 8192) [E from the layout sizes]. CRoaring picks
  by comparing exactly these three sizes after `run_optimize` [R].

Portable serialization (little endian) [F, constants verbatim]:
```
SERIAL_COOKIE_NO_RUNCONTAINER = 12346   SERIAL_COOKIE = 12347   NO_OFFSET_THRESHOLD = 4
variant A (no run containers): u32 cookie=12346 | u32 n_containers
variant B (runs possible)    : u32 (cookie=12347 in low 16 bits, (n_containers-1) in high 16 bits)
                               | run-flag bitset of ceil(n/8) bytes (LSB of byte0 = container 0)
descriptive header           : n x { u16 key ; u16 (cardinality-1) }      // 4 bytes per container
type rule                    : run-flag set -> RUN ; else card<=4096 -> ARRAY ; else BITSET
offset header                : n x u32 byte-offset-from-stream-start, present iff cookie==12346 OR n>=4
payloads                     : ARRAY u16[card] | BITSET u64[1024] | RUN u16 nruns, {u16 start,u16 len-1}[nruns]
64-bit variant               : u64 bucket_count ; per bucket (ascending): u32 high-key, then a 32-bit Roaring bitmap
```
Because the (card-1) is in the header, the exact total cardinality of a bitmap is the sum of header
fields without touching payloads: **exact df for free** (used by the planner, section 4.3).

Frozen / zero-copy views: CRoaring has a "frozen" serialisation that can be used in place from an aligned
(32-byte) buffer [R: `roaring_bitmap_frozen_serialize/view` - verify layout in `roaring.h` before copying].
The portable format is *not* in-place usable for the bitset containers because payloads are unaligned.
Our own flat layout (section 3.1 NXR) solves this.

Set-operation algorithms (all [R] unless noted; the CRoaring paper [F abstract] confirms SIMD array
union/intersection/difference/xor exist):
```
AND(a,b): two-pointer merge on the sorted key arrays (gallop if len ratio > ~32).
  per equal key dispatch on (typeA,typeB):
    ARRAY x ARRAY : if |small|*64 < |large| -> galloping/binary search per small element
                    else merge (AVX2/SSE4.2 block-compare; scalar branchless fallback)
    ARRAY x BITSET: for x in array: test bit            -> output ARRAY
    BITSET x BITSET: AND 1024 words, popcount; if card<=4096 -> convert to ARRAY
    RUN x *       : expand run intersection; output via size rule
OR(a,b): ARRAY|ARRAY: merge; if card>4096 convert to BITSET. BITSET|x: OR words/set bits.
  *Lazy union*: when OR-ing many bitmaps, BITSET results keep cardinality = UNKNOWN (-1); only at the end
  recompute popcount once and (maybe) downgrade to ARRAY/RUN. Saves a popcount pass per pairwise OR.
ANDNOT / XOR: same dispatch table.
and_cardinality(a,b): never materialise the result (planner/counting).
```
Popcount: this CPU has POPCNT; scalar `popcnt` loop is fine for 8 KB. AVX2 Harley-Seal popcount
(Mula-Kurz-Lemire, "Faster Population Counts Using AVX2 Instructions", arXiv 1611.07612 [R, URL from memory]) wins only for big
buffers (>= ~512 B) - for an 8 KB bitset it is worth a later optimisation, not day 1.

Default parameters: array/bitset threshold 4096; chunk = 65,536; run-optimise at flush/merge time only (never
on every insert); galloping ratio 64 for array x array [R]. Reported gains [F]: Roaring vs WAH/Concise/EWAH
"several times faster (up to two orders of magnitude)" with better compression.

Pitfalls:
1. Run containers are a read-optimised feature: building them per mutation is slow. Convert only when sealing a segment.
2. Descriptive-header cardinality is card-1 in 16 bits (a full 65,536 container stores 65535): all code must use +1 arithmetic. Off-by-one at 4096/4097 and at run ending exactly at 65535 are the classic bugs.
3. Untrusted/corrupt input: offsets, nruns, card and array sortedness must be validated before an mmap'd segment is trusted (checksum the segment, validate cheaply on open).
4. 64-bit Roaring is only needed for global ids; segment-local docids are u32 (<= 4.29e9 docs/segment is far beyond any realistic segment).

### 2.2 Bit-sliced indexes (BSI) and O'Neil range evaluation

Sources: Rinfret, O'Neil, O'Neil, "Bit-Sliced Index Arithmetic", SIGMOD 2001 [F full text]
https://www.cs.umb.edu/~poneil/SIGBSTMH.pdf (defines BSI arithmetic Algorithm 3.1 add, 3.2 subtract, 3.3 min,
4.1 top-k, BSTM term-matching). The original range-restriction algorithm is O'Neil and Quass, "Improved
Query Performance with Variant Indexes", SIGMOD 1997 [R - not fetched; URL not verified, find via ACM DL by title].
Implementation reference: RoaringBitmap/roaring `BitSliceIndexing/bsi.go` [F, via summariser]
https://raw.githubusercontent.com/RoaringBitmap/roaring/master/BitSliceIndexing/bsi.go : struct = `bA []*roaring.Bitmap`
(one per bit plane) + `eBM` existence bitmap + Max/MinValue + runOptimized flag. **Observation [F]:** its
`CompareValue` iterates **per column id in batches** (parallel goroutines over the found set, descending bit
planes per row) rather than doing whole-bitmap ops per slice. That is O(|foundSet| * bits), not the
O(bits)-bitmap-ops of the textbook algorithm. Do not copy that; implement the bitmap-at-a-time form below.

Layout: column of N unsigned k-bit integers -> k bitmaps B[0..k-1] (B[i] = rows whose bit i is 1; B[0] = LSB)
plus E (existence/not-null bitmap). Signed/offset: store v - base (base = column min, "frame of reference")
so k = bitlen(max-min), usually far below 64.

Range algorithm (bitmap-at-a-time; [V] verified against brute force on 200 random columns with nulls,
all of >, >=, <, <=, =, between, k=12) - the form of the O'Neil-Quass idea:
```
cmp(c, F):               // c in [0,2^k) already offset; F = candidate rows (default E); returns (GT, EQ)
  GT = {} ; EQ = F & E
  for i = k-1 downto 0:
      if EQ is empty: break                      // all remaining rows are decided
      if bit(c,i)==1:  EQ = EQ & B[i]            // rows with 0 here are < c, drop out of EQ
      else:            GT = GT | (EQ & B[i]);    // rows with 1 here are > c
                       EQ = EQ & ~B[i]
  GE = GT | EQ ;  LT = (F&E) - GE ;  LE = LT | EQ ;  NE = (F&E) - EQ
  between(lo,hi) = GE(lo) - GT(hi)               // two passes, 2k ops; or fuse in one pass
clamp: c' = c - base ; if c' < 0 -> GT=F&E, EQ={} ; if c' >= 2^k -> GT={}, EQ={}
```
Cost: <= 3 bitmap ops per slice => at most 3k ops, O(k) in the number of **bitmaps**, independent of N except
through word-parallelism. Passing the candidate set F (the result of the other, cheaper clauses) as the initial
EQ makes the cost proportional to |F|, not N.
Trailing-zero trick [V, 500 randomised trials]: for `x >= c` (and `x < c`) with t = ctz(c), only slices
i >= t matter, because x >= m*2^t iff floor(x/2^t) >= m. Round constants - `parameters:<10B` = 10,000,000,000 =
2^10 * 9,765,625 skips 10 of ~34 slices; `size:>=1GB`, `ram:<8GB` skip even more. Canonicalise predicates
in the planner to the `>=`/`<` forms when the integer domain allows (`>c` = `>=c+1`, `<=c` = `<c+1`).
Aggregates and ordering (Rinfret et al. [F]):
```
SUM over found set F      = sum_i 2^i * popcount(B[i] & F)           // k popcounts; AVG, COUNT similar
TOPK(k, F)  (Alg. 4.1) [V]: G = {} ; E' = F & E
  for i = P downto 0:
     X = G | (E' & B[i]); n = |X|
     if n > k: E' = E' & B[i]
     elif n < k: G = X; E' = E' & ~B[i]
     else: E' = E' & B[i]; break
  result = G | E'   (trim ties from E' so |result| = k)
```
`ORDER BY modified DESC LIMIT 20` over a filtered set therefore costs ~2-3 bitmap ops per slice and no sort.
Addition (Alg. 3.1): S0 = A0 xor B0; C = A0 and B0; Si = Ai xor Bi xor C; C = maj(Ai,Bi,C) - a ripple-carry
adder over bitmaps; the paper's BSTM uses it to sum per-term indicator slices and TOPK to pick best docs,
"comparable in performance to the most efficient known IR algorithm" (Perry-Willet) [F] but degrading as
the number of query terms grows [F, section 5.1].

Floats: order-preserving map double->u64: if sign bit set flip all bits else set the top bit [V: monotone on
{-1e300,-5.5,-0.0,0.0,1e-300,...}; note -0.0 maps *below* +0.0 so normalise -0.0 to +0.0 at ingest, and reject/segregate NaN]. That
is 64 slices - too many; see novel idea N5 (coarse BSI over equi-depth bucket id plus exact edge verify).
2026 data point [F abstract only]: arXiv 2608.26368 (Mathiyazhagan, 26 Aug 2026) evaluates predicates directly
over bit-sliced compressed data with SIMD (Java Vector API), supports int/long/double (IEEE-754 transform) and
dictionary-encoded strings; reports up to 8x memory reduction and 2.4-10.8x speed-up over scalar scans on TPC-DS (50M rows)
and 1.5-43x per type. Baselines were not visible in the abstract; speed-ups are against *scalar scans*, so
treat as directional confirmation of the design, not as a bar to beat.

Pitfalls: (1) k slices of Roaring = k separate cursors; keep them chunk-aligned so one 8 KB window per slice is
processed at a time (cache-resident). (2) Dense slices compress poorly: expect low-order slices to be ~50% density
bitsets (8 KB per chunk each) - BSI of k bits costs about k bits/row ~ same as the raw column; the win is query
speed, not space. (3) NULL handling must go through E, otherwise `<` returns nulls. (4) Updates: a BSI is
immutable per segment (deletes via tombstone bitmap AND-NOT, never by editing slices).
Back-of-envelope [E]: k=32, 65,536-row chunk: 32 slices x 8 KB, ~3 AVX2 ops per 256 bits => 32*3*256 = 24.6K
vector instructions per chunk = 0.375/row; 100M rows ~ 37M instructions (compute ~10-20 ms), 400 MB of slice traffic
(~30-40 ms at ~10-12 GB/s): memory-bound; zone-map pruning and early exit (EQ empty) are what actually save time.

### 2.3 Zone maps (min/max skipping) and equi-depth histograms

Zone maps: Moerkotte, "Small Materialized Aggregates: A Light Weight Index Structure for Data Warehousing",
VLDB 1998 [R, URL not verified]. Same idea as Parquet column-chunk/page statistics and DuckDB row-group min-max [R].
Per fixed block of rows store min, max, null_count (optionally all-equal flag). A predicate is classified per block
as NONE / ALL / SOME: NONE -> skip block; ALL (block range inside predicate range, nulls=0) -> emit the whole
chunk container as a RUN with no per-row work; SOME -> run BSI/scan only on that chunk.
Default [E]: block = 4096 rows for scans, plus a second level of 65,536 rows aligned to Roaring chunks. Entry 24 bytes
(u64 min, u64 max, u32 nulls, u32 flags) => 24*8/4096 = 0.047 bits/row. Zone maps only help when values correlate
with docid (timestamps in ingest order, sorted-by-path files); for random columns min/max are useless - detect at
build time (e.g. if >80% of blocks span >50% of the global range, store `zm_useful=0` and skip).

Equi-depth histogram (stored beside the column, used for cardinality estimates and for the planner; [E] derivation):
B = 64 buckets, bounds b[0..64] (b[0]=min, b[64]=max) so each bucket holds N/64 rows. Selectivity of `x in [a,b]` =
(fully covered buckets + interpolated fractions of the two end buckets) / 64. Worst-case absolute error is < 2/B
(two partial buckets) = 3.1% with B=64; with linear interpolation inside buckets the typical error is far smaller.
Add the top-16 most-common values with exact counts (equi-depth blurs heavy hitters) and an NDV estimate (HLL, 4 KB or exact
if < 4096). Build at segment seal from the sorted column or from a 64K reservoir sample (`nth_element` for the
bucket bounds). Storage: 2+2 header + 65*8 + 16*(8+4) + 8 ~= 720 bytes per column per segment.

### 2.4 Posting-list compression

**SIMD-BP128 / SIMD-FastPFOR** - Lemire and Boytsov, "Decoding billions of integers per second through
vectorization" (arXiv 2012, SPE 2015) [F abstract] https://arxiv.org/abs/1209.2137. 128-integer blocks, delta
coded, bit-packed at a per-block width b (block = 16*b bytes), in a "vertical" SIMD layout (4 lanes in SSE).
Reported: nearly twice as fast as the previous fastest (varint-G8IU, PFOR); saves up to 2 bits/int vs those;
SIMD-FastPFOR within 10% of Simple-8b's ratio at 2x its decoding speed [F]. Pitfall: pure BP pays for outliers in
a block (width = max); for postings, blocks of 128 gaps keep the width tight, and the tail (<128) uses a byte-wise code.

**Stream VByte** - Lemire, Kurz, Rupp 2017 [F abstract] https://arxiv.org/abs/1709.08990. Control stream separated from the data stream [F]
(details from memory [R]: 2 control bits per integer = byte length 1..4, decoded 4 ints at a time with `pshufb`); >4 billion differentially-coded integers/s on a
3.4 GHz Haswell, up to 2x faster than varint-G8IU, sometimes faster than memcpy [F]. Use for: the <128-int tail of
each list, term frequencies/positions in non-blocked contexts, and any variable-length column of u32.

**Elias-Fano (EF)** - Vigna "Quasi-succinct indices" WSDM 2013 (arXiv 1206.4300 - [R], URL from memory);
Partitioned EF, Ottaviano-Venturini SIGIR 2014 [R]. For n sorted values in [0,u): l = floor(log2(u/n)); low l bits
of each value stored in a packed array (n*l bits); high parts stored as unary gaps in a bit vector of n + (u >> l) + 1 bits.
Space <= n*(2 + log2(u/n)) bits [V: n=1,000, u=1e6 -> l=9, 11.95 bits/elt (bound 11.97); n=1e6, u=5e7 -> l=5, 7.56 (7.64);
n=100, u=1e9 -> l=23, 25.20 (25.25)]. `nextGEQ(x)`: j = high(x); position of the j-th zero in the upper
array via select0 (sampled every 256-512 entries) then a short linear scan [R]. PEF partitions the list (DP over
cost) and picks EF / plain bitmap / all-ones per partition, giving the best space among fast schemes
[R; the CSUR survey Pibiri-Venturini arXiv 1908.10598 [F abstract] is the right place to read the numbers].
Use EF (unpartitioned) for monotone offset arrays (term -> postings offset, block directory), docid -> row maps, zone-map
boundaries: 1M terms over a 200 MB postings file => u/n = 200 => ~9.6 bits/term vs 64 bits plain [E].

**Which representation per density** [E, derived]: an ARRAY container costs 16 bits/id, a BITSET costs 1/density bits/id, delta+bit-packing costs about
log2(1/density) + ~1.5 bits/id for random (geometric) gaps. At density 1/16: ARRAY 16, BITSET 16, BP128 ~5.5 bits/id. The bitmap only beats
BP on space above roughly 30-40% density (BITSET 2.5-3.3 bits/id vs BP ~3.2-3.8). Hence Roaring for filters
(speed of set algebra, not size) and BP128 for ranked text; keep BP128 for lists with df < ~N/8 and add an optional
Roaring twin only for text terms with df > ~N/16 that are used as filters.

### 2.5 Term dictionaries

**FST** - BurntSushi, "Index 1,600,000,000 Keys with Automata and Rust" [F] https://burntsushi.net/transducers/
(the Lucene/tantivy dictionary structure). Acyclic minimal transducer built incrementally from **sorted** keys; a bounded
registry (~10,000-slot hash with LRU per slot) of frozen states gives "approximately minimal ... linear time and
constant memory". Outputs (u64) are placed on transitions and summed along the path (needs +, - and min-prefix). States
are ~1 byte typically; nodes carry a common-input index for O(1) transition lookup and delta-encoded addresses. Queries:
prefix/range (binary search on transitions), regex and Levenshtein automaton intersection, union/intersection of FSTs; mmap-friendly.
Numbers [F]: 1,649,195,774 URLs (134 GB raw) -> 27 GB (20.1%), build 82 min sorted with 56 MB max memory (240 min unsorted);
15.7M Wikipedia titles (384 MB) -> 40.9% of original. Pitfalls: keys must arrive sorted; immutable; random access
on cold mmap pages; implementation is ~1-2k lines of intricate code; output must be monotone-summable (term ordinal works).

**Front-coded blocks** [R for sizes; layout [E]]. Bucket B=16 consecutive terms; first term stored raw
`[vint len][bytes]`, others `[vint lcp][vint suffix_len][suffix bytes]`. A top-level array of per-block start offsets
(plain u32 or EF) plus a binary search over block-leading terms gives lookup in log2(N/16) string compares plus <= 15
decode steps. Ordinal = block*16 + i, so term ids are dense and free (`ord -> term` is O(1) block seek + <= 15 steps).
Prefix and range queries are one lower_bound + a forward scan, producing a contiguous ordinal interval [lo,hi) - which is
exactly what postings/BSI-over-ordinals need. Typically ~40-60% of raw text size for natural-language/identifier terms [R].

**ART (Adaptive Radix Tree)** - Leis, Kemper, Neumann, ICDE 2013 [F, full text read] https://db.in.tum.de/~leis/papers/ART.pdf. Four inner
node types by fan-out, 16-byte header (type, #children, compressed path): Node4 (<=4 sorted key bytes + 4 ptrs, 52 B), Node16 (5-16, SIMD
compare, 160 B), Node48 (17-48; 256-byte child-index array + 48 ptrs, 656 B), Node256 (49-256 direct array, 2064 B) [F table I].
Path compression + lazy expansion cut height. Space is bounded to 52 bytes/key worst case (34 with six node types), "as low as 8.1
bytes per key" on dense integers [F]. Keys must be transformed to binary-comparable form (unsigned big-endian; flip the sign bit
for signed ints; IEEE-754 transform for floats; strings terminated with a byte that does not occur in the key) [F, the paper's key-transformation section]; then
range scan, prefix, min/max and top-k are ordered traversals. Pointer-based => in-memory only; not mmap-able on disk as-is.

**Double-array / Marisa** [R]: static, very small (Marisa uses nested patricia tries + LOUDS), slow to build, lookups are cache-hostile,
no efficient ordered ranges. Not competitive for a system that needs prefix/range/regex.

**Wildcard/regex over terms**: constant-prefix part -> ordinal range; the rest filtered by a glob/regex matcher over the range.
Leading wildcard (`*castle`): second dictionary over reversed terms (suffix -> prefix query) or a trigram index
(topic 04). Fuzzy `~`: FST + Levenshtein automaton is the only structure here that gives it natively [F]; with front-coded blocks
use a trigram/n-gram shortlist then bit-parallel Myers verify. Expansion cap [E]: if a pattern expands to > 1,024 terms,
switch to "constant-score" mode (OR the term bitmaps lazily into one bitmap with no per-term scoring) - Lucene's rewrite strategy [R].

---------------------------------------------------------------------------------------------------

## 3. Concrete byte layouts for the flat mmap-able segment

All integers little-endian. Every top-level section starts on a 64-byte boundary; a 32-byte section directory at the file tail
stores (offset,len,crc32c) per section. Validate crc on open, bounds-check offsets before dereferencing.

### 3.1 NXR - frozen Roaring (chunk-aligned, in-place usable)
```
u32 magic 'NXRB' | u32 n_containers | u64 total_card | u32 flags | u32 reserved       // 24 B (pad to 32)
u16 keys[n]                 // sorted high-16 of docid
u16 cardm1[n]               // cardinality-1 (exact df without touching payload)
u8  types[n]                // 0 ARRAY u16[card] | 1 BITSET u64[1024], 64B-aligned | 2 RUN {u16 n; {u16 start; u16 len-1}[n]}
u32 offs[n]                 // byte offset of payload from NXR base (always present: no NO_OFFSET_THRESHOLD special case)
pad to 64; payloads (BITSETs first so they are naturally aligned, then RUN, then ARRAY)
```
Struct-of-arrays so the key binary search touches a dense u16 array. Conversion to/from the portable format
(cookie 12346/12347) is a header rewrite plus memcpy of payloads, so we stay interoperable with CRoaring for tests/tools.

### 3.2 Numeric column: zone map + histogram + BSI
```
ZM : u32 block_rows(4096) | u32 nblocks | { u64 min ; u64 max ; u32 nulls ; u32 flags }[nblocks]   // 24 B/block
HIST: u16 nb(64) | u16 nmcv(16) | u64 bounds[nb+1] | u64 mcv_val[nmcv] | u32 mcv_cnt[nmcv] | u64 ndv | u64 nnull
BSI : u32 magic 'NXBS' | u8 k | u8 flags (bit0: value=raw-bits-of-double-mapped, bit1: dict-ordinal) | u16 rsv
      | u64 base | u64 nrows | u64 exist_off | { u64 off; u32 len; u32 card }[k]  // slice i (LSB first)
      then k+1 NXR blobs (existence + slices), 64B aligned
RAW : the plain fixed-width column (u32/u64/f64) is kept as well: needed for result display, edge-bucket verification, sorting, and as a fallback when k > 40
```
### 3.3 Term dictionary (front-coded v1)
```
HDR : magic 'NXTD' | u32 nterms | u32 block_terms(16) | u32 nblocks | u64 blocks_off | u64 idx_off | u64 terminfo_off
IDX : u32 block_off[nblocks]  (or EF) ; optionally u8 first_term_prefix8[nblocks][8] for cheap first-level compares
BLK : [vint len0][term0] then 15 x [vint lcp][vint sfx_len][sfx]                  // last block may be shorter
TERMINFO[ord] : u32 df | u32 max_impact | u64 postings_off      (or EF-coded offsets + packed df: ~9.6 bits/offset, see 2.4)
```
### 3.4 Ranked-text postings (per term)
```
u32 df | u32 n_blocks | skip[n_blocks]: { u32 last_docid ; u32 byte_off ; u16 max_tf ; u16 max_impact_q }  // 12 B
block : u8 bits_doc | u8 bits_tf | doc_gaps(128 x bits_doc, vertical 4-lane BP128; gap = docid - prev - 1; first gap relative to previous block's last_docid) | tf-1 (128 x bits_tf)
tail  : Stream VByte of the (df mod 128) remaining gaps, then their tf-1
```

---------------------------------------------------------------------------------------------------

## 4. Adopt for NexusSearch (ordered work plan)

### 4.1 Order of implementation (C11, AVX2 behind runtime dispatch, scalar fallback always present)
1. **`nx_roaring` (core, ~1,500 lines)** - 32-bit only. Containers ARRAY(<=4096)/BITSET(1024 x u64, 64B-aligned)/RUN. API: add, contains, and/or/andnot/xor
   (+ in-place and `*_cardinality`), `or_many` (lazy: scratch BITSET per key, one popcount at the end), iterate, `run_optimize`,
   `serialize_portable` / `deserialize_portable` (12346/12347 cookies) and `freeze` (NXR 3.1). Constants: array max 4096, gallop ratio 64.
   Cross-check against CRoaring/pyroaring bytes in tests only (no runtime dependency).
2. **Columns + zone maps + histograms (core)** - per numeric/date/size/boolean/enum field: RAW column, ZM (4096-row blocks), HIST (64 buckets + 16 MCVs).
   Enum/low-cardinality fields (`format`, `language`, `type`; <= 256 distinct): dictionary ids + **one Roaring posting per value** (equality = 1 bitmap, `format:(onnx OR gguf)` = lazy OR).
3. **BSI (core for ordered fields)** - k = bitlen(max - min) offset-coded, cap k <= 40 (covers parameters in the 1e10 range, byte sizes to ~1 TB, epoch seconds
   and milliseconds when offset-coded). Operators: GE/LT canonical with ctz skip, EQ, between (fused single pass), TOPK, SUM/COUNT. The
   planner passes the already-computed candidate bitmap F as the initial EQ. Strings ordered by dictionary ordinal (sorted dictionary
   => ordinal order = byte order) so `name:>"foo"` = dictionary lower_bound -> BSI GE over ordinals.
4. **Term dictionary v1 = front-coded blocks, B=16 (core)**, ordinal = dense term id; prefix/range = ordinal interval. **FST v2 (stretch/important for regex+fuzzy)**
   built at merge time from the sorted terms, used only for `~` and regex terms; keep front-coded blocks as the exact-lookup path until FST proves smaller/faster in our bench.
5. **Ranked postings = BP128 (vertical 4-lane, SSE4.1 baseline) + Stream VByte tail + skip entries with block-max impact (core for BM25)**. Write scalar pack/unpack with the
   vertical layout first (decoder is then SIMD-able later without a format change). Elias-Fano for offset arrays (important, small: ~150 lines).
6. **Memtable (NRT)**: hash map term -> growable postings builder; sort terms at refresh (cheap for <= ~1e5 terms). **ART only if** in-memtable prefix/range queries on
   unflushed docs show up in profiles (stretch) - it gives ordered iteration and 8-52 B/key but ~800 lines of node-growth code.
7. **Partitioned Elias-Fano, Marisa, double-array: do not implement** (see section 5).

### 4.2 Default parameters (all overridable per field in segment metadata)
Roaring chunk 65,536; array->bitset 4096; run size 2+4n; run_optimize at seal/merge only. Zone-map block 4096 rows (+ 65,536 row level). Histogram 64 buckets + 16 MCV.
BSI slice cap 40, ctz skip on. Front-code block 16 terms. Postings block 128 docs; bit widths 0..32; skip entry per block; tail < 128 in Stream VByte. EF offset sampling every 256.
Multi-term expansion cap 1,024 terms then constant-score mode. Segment alignment 64 B; per-section CRC32C.

### 4.3 How the planner uses these (feeds EXPLAIN / EXPLAIN ANALYZE)
- Leaf cardinality of a term/enum clause = sum of `cardm1+1` over NXR headers: **exact, O(#containers), zero payload reads**.
- Numeric range selectivity = histogram interpolation (error < 2/64 worst case); equality = MCV hit else N/NDV.
- Cost model (units = 8 KB container ops): AND of k clauses ordered by ascending cardinality; BSI range = (slices touched) x (chunks surviving zone maps) x ~3 ops;
  fused: slices touched = k - ctz(c). EXPLAIN ANALYZE should print per clause: estimated vs actual card, chunks skipped by zone map, slices touched, container types produced.
- Evaluate cheapest/most selective clause first and feed its bitmap as the BSI candidate set F; evaluate expensive text/regex clauses last and only on F.

---------------------------------------------------------------------------------------------------

## 5. Skip and why
- **WAH / Concise / EWAH / PLWAH**: RLE word-aligned schemes; Roaring is smaller and up to ~100x faster on set ops [F, 1603.06549].
- **64-bit Roaring as the primary type**: segment-local docids are u32; the portable 64-bit form is just `u64 bucket count + (u32 key, 32-bit bitmap)` [F] - add later for global ids only.
- **Roaring-Go-style BSI compare (row-at-a-time over the found set)** [F]: O(|F| x bits); use bitmap-at-a-time instead.
- **Binary Interpolative coding, OptPFOR** [R]: smaller but 3-10x slower to decode; not worth it before SIMD-BP128 is saturating the CPU. Revisit only for cold archival segments.
- **Partitioned Elias-Fano for postings** [R]: best space/speed trade in the literature but needs a DP partitioner, three sub-encodings and select structures; BP128 + skip + block-max is simpler and fast on AVX2. Revisit if postings dominate disk.
- **Marisa trie, double-array** [R]: static, no ordered range queries, poor locality.
- **Full ART as the on-disk dictionary** [F]: pointer-based, not mmap-able; FST or front-coded blocks are the on-disk forms.
- **AVX-512 variants** of Roaring/popcount: this CPU has none; stay on AVX2 + POPCNT + BMI2.
- **BSI for text/regex fields or very high-k floats**: wrong tool; use dictionary + ordinals (strings) or the coarse-BSI trick N5 (floats).

---------------------------------------------------------------------------------------------------

## 6. Novel ideas specific to NexusSearch
- **N1 One chunk size for everything.** 65,536 docids = Roaring container = BSI chunk = zone-map super-block = HNSW filter mask window. Every operator is a *chunk iterator*; an empty/NONE container
  for any AND-ed clause at chunk c skips *all* other operators at c (including vector scoring). The final filter bitmap is passed to filtered HNSW as the valid-mask; BITSET containers give O(1) `valid(id)`.
- **N2 Free exact leaf cardinalities** from NXR headers drive the cost-based planner without a statistics job; histograms handle ranges (see 4.3).
- **N3 BSI top-k as an ORDER BY / recency feature extractor.** `TOPK` over the structured result set returns "the 100 most recently modified matching objects" with ~3 ops/slice and no sort; the hybrid ranker can then use rank-in-BSI as its recency feature.
- **N4 Bit-sliced lexical scoring (experimental).** Quantise per-term impacts to 8 bits, store as BSI slices for very frequent terms, sum with Alg. 3.1 (carry-save add of bitmaps), pick top-k with Alg. 4.1 [F]. Compare against block-max WAND; the 2001 paper reports parity with Perry-Willet for few query terms [F]. Likely a win only for filter-heavy queries on huge candidate sets.
- **N5 Coarse BSI + edge verify for floats / high-cardinality columns.** Map values to an 8-12-bit equi-depth bucket id (boundaries = the histogram), BSI over bucket ids (8-12 slices instead of 64), range `[a,b]` = BSI on buckets strictly inside + exact verification of rows in the two edge buckets via the RAW column (<= 2/256 of rows at 8 bits). Gives exactness with O(10) bitmap ops.
- **N6 Zone-map ALL-blocks as RUN containers.** When a block is classified ALL, emit the chunk as a single run `{start, 65535}` without touching data; "recent files" (`modified:>X`) on ingest-ordered data degenerates to a handful of runs plus one SOME chunk.
- **N7 Self-explaining dictionary expansions.** Because prefix/wildcard/range-over-terms produce an ordinal interval, EXPLAIN can state "expanded to 312 terms, df sum 18,442, mode=constant-score" before running anything.
- **N8 WATCH/percolator subscription index** (idea): index subscriptions by their rarest required term in the same dictionary type; a new doc's terms probe the subscription dictionary (term -> bitmap of subscription ids), AND-ing the bitmaps of required terms across the doc's terms. [E; unvalidated]
- **N9 Optional segment-level term existence filter** (binary fuse filter, Graf-Lemire 2022 [R, arXiv 2201.01174 from memory]) to skip whole segments for rare terms before touching the dictionary; only worth it with many (>20) segments.

---------------------------------------------------------------------------------------------------

## 7. Validation and test ideas
1. **Differential bitmap tests**: random sets (densities 1e-4, 1e-3, 1/16, 0.5, 0.99, runs) vs a naive `uint8_t[N]`/Python-set oracle for every op; all 3x3 container-type pairs; exercise boundaries at cardinalities 4095/4096/4097 and runs touching 0 and 65535; lazy-OR followed by a count must equal strict OR.
2. **Interop**: serialise with our code, load with CRoaring/pyroaring (`pip install pyroaring`, tiny wheel; test-only) and vice versa, byte-compare for the 12346/12347 cookies and the offset-header rule (n>=4).
3. **Corruption/fuzz**: flip bytes, truncate, give offsets out of range, nruns that overflow 65535, unsorted arrays: open must reject, never crash (there is no ASan/UBSan on this compiler, so use bounds-checked accessors in debug builds plus a simple byte-mutation loop driven from a test binary).
4. **BSI**: brute-force oracle over columns with nulls, constants {0, 1, 2^k-1, 2^k, 2^k+1, base-1, negative}, round constants (ctz skip), `between` with lo>hi, signed values, doubles including -0.0 and NaN. Already [V] in Python: compare/range/top-k and GE-with-ctz-skip pass on 200+500 random trials; port those tests to C as golden vectors.
5. **TOPK ties**: exactly-k and more-than-k ties; k = |F|; empty F.
6. **Zone-map classification**: property test "NONE blocks contain no match, ALL blocks contain only matches" on random and sorted columns.
7. **Dictionary**: sorted-order invariant; prefix with empty prefix, prefix at 0xFF boundaries (`upper_bound = prefix with last byte+1`, carry); UTF-8 (byte order == code point order, but only after NFC normalisation - enforce at ingest); front-coded `ord <-> term` round trip for every term; wildcard vs regex oracle.
8. **Postings**: pack/unpack for every width 0..32 and every tail length 0..127; `nextGEQ` against binary search; EF vs plain arrays for random monotone sequences; reproduce the EF sizes quoted in 2.4.
9. **Benchmarks (small, local, memory-resident <= ~200 MB)**: bitmap AND/OR ns per container pair; BSI GE on 1M and 10M rows at k=16/32/40 with/without zone maps (target: well under ~1 ms per 1M rows per clause with early exit); BP128 vs Stream VByte decode speed on this AVX2 laptop (the 4 G ints/s of [F] was a 3.4 GHz Haswell desktop - expect a lower figure); dictionary lookup ns and bytes/term for front-coded B in {8,16,32} vs FST on a 100K-term vocabulary.
10. **Planner**: after every EXPLAIN ANALYZE compare estimated vs actual cardinality; alert on ratio > 10x.

---------------------------------------------------------------------------------------------------

## 8. References

Fetched [F]:
- Roaring portable format spec - https://github.com/RoaringBitmap/RoaringFormatSpec
- Lemire, Ssi-Yan-Kai, Kaser, "Consistently faster and smaller compressed bitmaps with Roaring", 2016 - https://arxiv.org/abs/1603.06549
- Lemire, Kaser, Kurz, Deri, O'Hara, Saint-Jacques, Ssi-Yan-Kai, "Roaring Bitmaps: Implementation of an Optimized Software Library" (CRoaring), 2017/2018 - https://arxiv.org/abs/1709.07821
- Lemire, Kurz, Rupp, "Stream VByte: Faster Byte-Oriented Integer Compression", 2017 - https://arxiv.org/abs/1709.08990
- Lemire, Boytsov, "Decoding billions of integers per second through vectorization", 2012/2015 - https://arxiv.org/abs/1209.2137
- Pibiri, Venturini, "Techniques for Inverted Index Compression", ACM CSUR 2021 (abstract only read) - https://arxiv.org/abs/1908.10598
- BurntSushi, "Index 1,600,000,000 Keys with Automata and Rust" - https://burntsushi.net/transducers/
- Leis, Kemper, Neumann, "The Adaptive Radix Tree: ARTful Indexing for Main-Memory Databases", ICDE 2013 (full text) - https://db.in.tum.de/~leis/papers/ART.pdf
- Rinfret, O'Neil, O'Neil, "Bit-Sliced Index Arithmetic", SIGMOD 2001 (full text) - https://www.cs.umb.edu/~poneil/SIGBSTMH.pdf
- RoaringBitmap/roaring Go BSI implementation (summarised) - https://raw.githubusercontent.com/RoaringBitmap/roaring/master/BitSliceIndexing/bsi.go
- Mathiyazhagan, "Direct-Operable SIMD Bit-Slicing: A Framework for Memory-Efficient Predicate Evaluation", 26 Aug 2026 (abstract only) - https://arxiv.org/abs/2608.26368

Recall-only [R] (not fetched; verify before relying; URLs from memory are marked):
- O'Neil, Quass, "Improved Query Performance with Variant Indexes", SIGMOD 1997 - URL not fetched (search ACM DL by title).
- Moerkotte, "Small Materialized Aggregates: A Light Weight Index Structure for Data Warehousing", VLDB 1998 - URL not fetched.
- Vigna, "Quasi-succinct indices", WSDM 2013 - arXiv 1206.4300 (from memory).
- Ottaviano, Venturini, "Partitioned Elias-Fano Indexes", SIGIR 2014 - URL not fetched.
- Mula, Kurz, Lemire, "Faster Population Counts Using AVX2 Instructions" - arXiv 1611.07612 (from memory).
- Martinez-Prieto, Brisaboa, Canovas, Claude, Navarro, "Practical compressed string dictionaries", Information Systems 2016 - URL not fetched.
- Graf, Lemire, "Binary Fuse Filters: Fast and Smaller Than Xor Filters", 2022 - arXiv 2201.01174 (from memory).
- CRoaring frozen-bitmap API (`roaring_bitmap_frozen_serialize` / `_frozen_view`), Lucene BlockTree term dictionary and multi-term rewrite behaviour, Tantivy dictionary layout: from memory; read the repositories before copying any layout. (I also have an unverified recollection that very recent Lucene releases moved the terms *index* from an FST to a trie; check before citing.)

Local verification artefacts (session scratchpad, not in the repo): `bsi_test.py` (compare/range/top-k/float-map/EF sizes) and `bsi_test2.py` (ctz-skip), both passing.
