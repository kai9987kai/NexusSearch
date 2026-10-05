# 06 - Substring, regex and code search indexes

Status: research note, written 2026-10-01. Audience: NexusSearch implementers (C11, zero dependencies).
Evidence tags used throughout:

- [F] = fetched and read this session (URL in References). Where the fetch tool summarised the page, that is stated.
- [2nd] = third-party or derivative source, claim not independently verified.
- [RO] = recall-only: from memory, no source fetched. Treat numbers as approximate and verify before relying on them.
- [PROPOSAL] = my own design/derivation, not claimed by any source.

Research budget was ~12 fetches; gaps are listed explicitly at the end of "Landscape".

---

## 1. Landscape

| System | Index | Verification | Key facts |
|---|---|---|---|
| Google Code Search / `codesearch` (Russ Cox, 2012) [F] | Doc-level trigram inverted index (trigram -> sorted file IDs), mmap'd | Regex DFA over candidate files | Index is ~20% of corpus (Linux 3.1.3: 420 MB -> 77 MB). "hello world": 36,972 files -> 25 candidates. Regex -> trigram AND/OR query via exact/prefix/suffix/match sets. |
| Zoekt (Sourcegraph) [F] | Positional trigrams (rune offsets), shards are mmap-able files, 32-bit offsets (shard <=4 GB, content <=1 GB) | Check 2 trigrams (first+last) at the right distance, then compare bytes; regex -> substring AND/OR then regex on candidates | Index ~3.5x corpus size; ~1.2x corpus in RAM if postings stay on SSD; "sub-50ms" on Android (~2 GB text); case-insensitive = enumerate case variants of each trigram; rune->byte offset table every 100 runes; ctags for symbols (seccomp sandbox); BM25 scoring option. |
| GitHub Blackbird (2023) [F] | Variable-length "sparse grams" in three indexes: content, symbols, paths; sharded by Git blob SHA | Fetch doc, run regex | 115 TB / 45 M repos -> 28 TB unique -> 25 TB index (incl. compressed content); 15.5 B docs; ~120k docs/s ingest; per-shard p99 ~100 ms; ~640 QPS per 64-core host; doc IDs assigned in rank order so lazy posting iterators return best docs first; trigrams rejected because common ones ("for") give too many false positives. |
| Cursor "fast regex search" (2025 per brief; fetched text carries no date) [F, via summariser] | Sparse n-grams (hash-only dictionary + separate postings file); layer for working-tree changes on top of a Git commit | `rg`-style regex on candidates | Motivation: `rg` invocations >15 s on huge monorepos. Lookup table mmap'd in editor process, postings read by offset from disk. Hash collisions only widen candidate set. Discusses and rejects suffix arrays (one concatenated string, expensive updates) and describes GitHub-style "probabilistic masks" (loc/next masks) as a way to get quadgram selectivity with trigram storage. |
| livegrep (Nelson Elhage, 2015) [F, via Cursor text] | Suffix array over whole corpus | - | Cited as the suffix-array precedent (Linux kernel). |
| ClickHouse | `sparseGrams` functions + text/ngram indexes | - | Uses crc32 of character pairs for weights (per Cursor text [F]). Exact function defaults NOT retrieved (docs page fetched did not surface them) -> [RO]. |
| `danlark1/sparse_ngrams` [F README only] | C++17 implementation "used in GitHub Codesearch"; README says algorithm docs "TBD" | - | Targets "billions of lines with <100ms latency". No algorithm text available. |
| `erogol/ngi` DESIGN.md [2nd] | Third-party Rust re-implementation of Cursor's design | - | Gives concrete algorithm description (monotonic stack, 3..8 length, crc32c weights, 16-byte dictionary entries). Its numbers (index 30-50% smaller than trigram, 10-100x speedups) are unverified claims. |
| ripgrep / Rust `regex` / regex-automata (BurntSushi) [F] | No index; literal prefilters + lazy DFA | - | Engines: Pike VM, bounded backtracker, one-pass DFA, lazy DFA, full DFA, chosen by a "meta engine". Prefilters: memchr on rarest byte, Teddy for multi-literal, prefix/suffix/inner literals. |
| Hyperscan (Intel, NSDI 2019) [F] | No index; regex decomposed into literal strings + small automata | - | Strings matched first (FDR/Teddy SIMD), automata run only when triggered. 87-94% of IDS regexes have an extractable string. Hyperscan-s 40.1x vs PCRE, 10.3x vs RE2-s, 2.3x vs PCRE2 on 1,300 Snort Talos regexes; Hyperscan-m 13.5x faster than RE2-m. |
| Infini-gram mini (arXiv 2506.12229) | FM-index for exact n-gram search at web scale | - | Title seen in search results only; abstract NOT read. Listed only as pointer for FM-index tradeoffs. |

Gaps (not researched, recall-only below): ClickHouse sparseGrams defaults, Roaring/posting compression papers, Universal Ctags internals, Lucene WordDelimiter, Sourcegraph ranking weights, RE2 DFA cache policy, FREE/BEST regex-indexing papers (Cho & Rajagopalan 2002; Hyperscan-era follow-ups).

---

## 2. Techniques

### 2.1 Doc-level trigram index + regex-to-query analysis (Cox 2012) [F]

Core idea: every regex match must contain certain literal substrings. Compute a boolean query over trigrams; intersect posting lists; run the real regex only on survivors.

Analysis (per regex node, computed bottom-up). Five values:
- `emptyable`: can match empty string
- `exact`: finite set of strings the node matches exactly, or UNKNOWN
- `prefix`, `suffix`: sets of possible string prefixes/suffixes
- `match`: boolean trigram query that every match must satisfy

Rules (from the article; I give the rule shapes, sizes are in 2.1.1):
```
literal c        : exact={c}, emptyable=no, match=ANY
e1 e2 (concat)   : exact = cross(exact1, exact2) if both known and small
                   else prefix=prefix(e1) (+ cross if e1 exact), suffix likewise
                   match = match(e1) AND match(e2) AND trigrams(suffix(e1) x prefix(e2))
e1 | e2          : exact = union, prefix = union, suffix = union, match = match1 OR match2
e?  e*           : exact=UNKNOWN, emptyable=yes, match=ANY
e+               : match = match(e)  (e repeated; prefix/suffix of e kept)
class [abc]      : exact = {a,b,c} if small, else UNKNOWN (treat like any-char)
trigrams(string s):  len(s)<3 -> ANY ; else AND of all len-2 windows of 3 bytes
trigrams(set S)  :  OR over s in S of trigrams(s)
```
Information-saving simplifications: `match &= trigrams(prefix)`, `&= trigrams(suffix)`, `&= trigrams(exact)`. Information-discarding (to bound size): drop longer strings in a set when a shorter one is a prefix, truncate longest strings when sets exceed limit, convert oversized `exact` to UNKNOWN, boolean absorption (`abc OR (abc AND def)` -> `abc`).

2.1.1 Size limits. Article text gives the mechanism, not the constants. [RO] In `codesearch/index/regexp.go` I recall `maxExact = 7` (exact strings longer collapse) and `maxSet = 20` (set cardinality) and char classes expand only when small (<=~10 chars). Treat as starting values: `max_exact_len=7, max_set=20, class_expand<=10`.

Data layout (codesearch, from article + source file names [F]): path list, file list, one posting list per trigram (delta-varint file IDs [RO]), trigram->offset table, all mmap'd. Index ~18-20% of corpus.
Gains reported: DATAKIT 2,739 -> 3 files; "hello world" 36,972 -> 25 (~1,479x fewer files, ~100x faster); case-insensitive weaker (599 candidates vs 36,972 corpus; ~10x faster).
Pitfalls: (a) short literals (<3 bytes) give ANY -> full scan; (b) case-insensitive explodes alternatives; (c) common trigrams ("the", "for") make useless lists; (d) UTF-8 classes like `[α-ω]` expand to many byte-alternatives -> UNKNOWN.

### 2.2 Positional trigrams with two-trigram verification (Zoekt) [F]

Posting entry = (doc, [offsets]); for "banana": ban:0, ana:1,3, nan:2. For a literal of length L pick first trigram at offset 0 and last trigram at offset L-3; candidate iff some pair of offsets has distance exactly L-3 in the same doc; then compare bytes (no regex engine needed for pure literals). Case-insensitive = look up all case variants of each trigram (2^3 = 8 per trigram) and compare case-insensitively. UTF-8: offsets are rune offsets + a rune->byte table every 100 runes (skipped when file is ASCII-only).
Layout: varint-encoded posting lists; shards are mmap-able; 32-bit offsets cap a shard at 4 GB. Cost: ~3.5x corpus index. Benefit: far fewer false candidates, no verify read for literals, "sub-50ms" on ~2 GB text.
Pitfall: size. Positional postings are the dominant cost; for NexusSearch's mixed corpora (models, 3D, JSON) I would not pay it by default.

### 2.3 Probabilistic position/next masks (trigram storage, quadgram-like selectivity) [F via Cursor text; origin attribution unverified]

Per posting add 2 bytes: `locMask` (8 bits: set bit `pos mod 8` for each occurrence of the trigram in the doc) and `nextMask` (8-bit Bloom of the byte following each occurrence). For literal "abcd":
```
t1 = "abc", t2 = "bcd"
adjacency test:  ((locMask(t1) << 1) | (locMask(t1) >> 7)) & locMask(t2) != 0   // rotate by 1 in 8 bits
quad test:       nextMask(t1) has bit h('d') = hash8('d')
```
Gain: ~quadgram selectivity at trigram dictionary size, +2 B per (gram, doc) posting. Pitfall (stated by Cursor): Bloom saturates on big docs (all 8 bits set) -> zero filtering. Needs per-doc cap or switch to "no mask" for huge docs.

### 2.4 Sparse n-grams (GitHub 2023, Cursor 2025, ClickHouse) [F for concepts; algorithm details from Cursor text + 2nd-hand ngi]

Idea: give each adjacent byte pair a weight `w(a,b)`. A *sparse n-gram* is a substring whose two border pairs have weights strictly greater than every pair strictly inside it. Variable length, anchored on "rare" pairs. Key properties:

1. **Locality**: validity of gram [i..j] depends only on pair weights inside [i..j]. So every sparse gram of a query literal is also a sparse gram of any document containing that literal. [PROPOSAL derivation, but it is what makes build_all (index) / build_covering (query) consistent.]
2. Index time `build_all`: emit all valid grams with min_len..max_len (ngi: 3..8 [2nd]). Count <= 2m for m pairs.
3. Query time `build_covering`: emit a minimal subset that still covers the literal; "chester" -> {"chest","ster"} instead of 5 trigrams [2nd]. Fewer lookups, longer = rarer grams.

Weight functions:
- crc32c of the 2-byte pair, deterministic pseudo-random; no table needed (ClickHouse and GitHub use crc32 of pairs per Cursor text [F]; ngi uses crc32c [2nd]).
- Frequency-based: "high weight to every pair that is actually very rare, low to every pair that is very frequent", trained on terabytes of open-source code (Cursor text [F]) -> better selectivity, but needs a table. 

`build_all` with a monotonic stack (ngi's description [2nd]; the exact tie handling below is my [PROPOSAL] and is consistent with the "strictly greater inside" definition):
```c
// s[0..n), pair weights w[k] = W(s[k], s[k+1]), k in [0, n-1), m = n-1
// gram for pair interval [i..j] (i<j) is bytes s[i .. j+1], length j-i+2
stack = []                                   // pair indices, strictly decreasing weights
for j in 0..m-1:
    while stack and w[top] <= w[j]:          // popped element sees j; interior is < both
        emit(top, j); pop
    if stack: emit(top, j)                   // top > w[j]
    push j
// emit() filters by len in [min_len, max_len]; adjacent pairs (j = i+1) always emit => ALL trigrams are always present
```
Because every trigram is a valid gram (empty interior), the index is a superset of a trigram index; the query can always fall back to trigram covering.

`build_covering` [PROPOSAL; sources only describe the outcome]: choose a chain of valid grams g1..gk over pair indices with g_{t+1}.start = g_t.end (consecutive grams share a border pair, which enforces adjacency like overlapping trigrams) covering pairs [0, m-1]. Dynamic programming over the <=2m valid grams: `best[end] = min over valid (start,end) of best[start] + cost(gram)` with `cost = log2(1+df(gram))` using df from the dictionary (so choice adapts to the real corpus) or constant 1 (min number of lookups). O(m). Any subset of valid grams is *sound* (never loses true matches); covering only affects selectivity and lookup count.

Data layout (Cursor text [F] + ngi [2nd]):
- Postings file: lists written sequentially.
- Lookup table: sorted by gram hash, entries `{hash u64, offset u32, length u32}` = 16 B, mmap'd, binary search; only hashes stored (no gram text).
- Delta + varint doc IDs [2nd].
Pitfalls: (a) weight function is part of the on-disk format -> store `weight_id` in header; (b) hash-only dictionary: collisions widen results but never break correctness (Cursor [F]); (c) index larger than trigram-only because it is a superset (ngi's "30-50% smaller" claim [2nd] contradicts this reasoning; MEASURE, do not trust); (d) grams spanning long minified lines.

### 2.4b ClickHouse sparseGrams [RO]
Function `sparseGrams(s[, min_ngram_length, max_ngram_length, min_cutoff_length])` and `sparseGramsHashes`; I recall defaults min 3, max 100. Not verified: fetched doc page did not surface it. Look up before copying.

### 2.5 Blackbird engineering (GitHub 2023) [F]
- Dynamic-size grams, "covering ngrams" only at query time; example `/arguments?/` -> `arg`, `rgu`, `gum` + (`ume`,`ment` or `uments`).
- Three independent indexes (content, symbol, path) queried separately.
- Shard by Git blob SHA: deduplicates identical content, balances load. Doc IDs by rank; compaction sorts posting lists by score; lazy iterators short-circuit intersections. Delta encoding against similar repos cut crawled docs by >50%.
- Final ranking after permission filter at aggregation layer.

### 2.6 Suffix arrays / FM-index tradeoffs [F via Cursor; rest RO]
- Suffix array: exact substring and char-range via binary search; one concatenated text; costly updates (Cursor [F]). Size [RO]: 4 B per text byte (32-bit SA) = 4x corpus, worse than a trigram index.
- FM-index [RO]: BWT + rank structure, can be ~corpus size or smaller, supports count/locate in O(m) + occ; slow locate, complex build, poor for regex (needs literals anyway). Infini-gram mini (arXiv 2506.12229) is a web-scale instance; not read.
- Conclusion for an updateable, multi-field engine: inverted gram index wins on updates (immutable segments, merge = sorted-run merge). Suffix structure only makes sense for tiny static fields.

### 2.7 Literal extraction, rare byte, memmem, Teddy (ripgrep / regex-automata / Hyperscan) [F]

Literal extraction (ripgrep): prefix literals (`foo|bar`), suffix (`[ab]foo[yz]` -> afooy, afooz, bfooy, bfooz), inner (`\w+foo\d+` -> `foo`). When a literal match fully decides the pattern the regex engine is not run. regex-automata optimisation pass [F]: drop literals <2 bytes, drop a literal that is only ASCII space, cap sequence size; `bar[a-z]` -> inexact "bar" instead of 26 variants. [RO] regex-syntax extractor defaults: limit_class 10, limit_repeat 10, limit_literal_len 100, limit_total 250.

Rare byte heuristic: pick the least frequent byte of the literal from a 256-entry frequency-rank table, `memchr` for it, then verify around it. Matters most for UTF-8 (frequent lead bytes 0xD0/0xD1 in Cyrillic) [F].

Case-insensitive literals: enumerate case variants of a short prefix and feed all to a multi-pattern matcher; ripgrep example: `PM_RESUME` with -i -> 16 prefix alternatives (PM_RE, PM_Re...) into Teddy [F].

Teddy (aho-corasick README [F]):
```
Per fingerprint byte f (N in {1,2,3} bytes):
   lo_mask[f][16], hi_mask[f][16]    // 16-entry nibble tables; bit b of entry v set <=> bucket b has a pattern whose f-th byte has that nibble = v
Scan 16/32-byte chunk C:
   lo = C & 0x0F ; hi = (C >> 4) & 0x0F       // psrlw + and
   for f: R_f = pshufb(lo_mask[f], lo) & pshufb(hi_mask[f], hi)   // bit set => bucket may match fingerprint byte f at this lane
   align R_f by f lanes (carry from previous chunk; AVX2 needs cross-128-bit-lane alignr workaround)
   R = R_0 & (R_1 shifted 1) & (R_2 shifted 2)
   if R != 0: for each set lane -> bucket bits -> verify every pattern in those buckets (memcmp)
```
8 buckets in "slim" (32 B/iter AVX2); "fat" Teddy uses 16 buckets with 16-bit lanes and scans only 16 B/iter. Pattern count practical cap ~64 [RO: aho-corasick caps at 64]. Hyperscan's paper says its PSHUFB-based small string-set matcher (<80 strings; PSHUFB over 2-8 4-bit regions in the suffix of each string) [F]; its larger-set matcher FDR is bucketed shift-or [F]: DP assigns patterns to buckets to minimise false positives, "super characters" to avoid short patterns polluting a bucket; FDR beats DFC by 1.1-3.2x (random content) and Aho-Corasick by 4.2-8.8x [F].
Pitfall: Teddy degrades when fingerprints are common bytes (high verify rate) -> require min fingerprint selectivity; >~64 patterns -> use Aho-Corasick/lazy DFA.

### 2.8 Regex engine architecture (regex-automata meta engine, RE2, Hyperscan) [F]

Engine ladder in regex-automata [F]:
1. Literal-only regex -> pure substring search, no automaton.
2. Prefilter (prefix literals via memchr/memmem or Teddy) finds candidate start; or inner-literal / reverse-suffix / reverse-inner strategies.
3. Lazy DFA (forward) finds match end; reverse lazy DFA finds start. If captures needed: run Pike VM or bounded backtracker only on the matched span.
4. Pike VM = universal fallback (Unicode word boundary, lazy DFA cache thrash). Bounded backtracker ~2x faster than Pike VM but capped by `len(regex) * len(haystack)` visited-set size. One-pass DFA: anchored only.
5. Lazy DFA: cache exhaustion -> quit with error -> meta engine falls back to Pike VM. Non-ASCII word boundary unsupported in lazy DFA.
UTF-8 compile [F]: char classes compiled to byte-sequence automata (e.g. U+0080-07FF -> `[C2-DF][80-BF]`); minimal-DFA-style suffix sharing cut `\w` from 3,564 to ~311 NFA states. Old ripgrep data [F]: `\w{5}\s+\w{5}\s+...` ~250 NFA states ASCII vs ~77,000 Unicode.
Hyperscan decomposition [F]: regex -> `FAn str_n ... FA1 str_1 FA0`; literals scanned first; each automaton switched on only when its neighbouring literal fires; `(R)?` and `(R)*` treated as single FA; `(R)+ = (R)(R)*`. NFA bit-parallel up to 512 states (128-bit masks, extendable to 512) [F]. Regex-invocation reduction from prefilter: 4.6x-182x on Snort ET-Open (500-2,500 regexes) [F].
Features to avoid for linear-time guarantee: back-references, arbitrary lookaround.

RE2-style lazy DFA specifics [RO]: leftmost-first semantics via priority-ordered NFA state lists in each DFA state; matches reported one byte late (delay by 1 byte) to support `$` and `\b`; start states keyed by look-behind context; byte equivalence classes to shrink transition rows; cache reset when full, give up (fallback) if it resets too often per byte processed (regex-automata: `minimum_cache_clear_count`, `minimum_bytes_per_state`). Default lazy-DFA cache ~2 MiB [RO].

### 2.9 Identifier-aware tokenisation, symbols, ranking [RO unless noted]
- Zoekt ranking signals [F]: match count, word-boundary alignment, recency, filename length, tokenizer context (comment vs literal), symbol definition; optional BM25 where matches are the terms. Symbols from ctags in a seccomp sandbox [F].
- Universal Ctags uses per-language parsers incl. regex "optlib" definitions [RO]; tree-sitter-free symbol extraction is feasible with a lexer state machine (comments/strings/braces) + per-language line rules.
- Lucene WordDelimiter(Graph)Filter [RO] splits at case changes, letter-digit transitions, non-alnum; options `preserveOriginal`, `catenateAll` -> emit whole identifier AND parts at overlapping positions.

---

## 3. Adopt for NexusSearch (ordered; implement in this order)

All code C11, no dependencies, 64-bit types only (`uint64_t`, `size_t`; Windows `long` is 32-bit). SIMD behind `#if defined(__x86_64__)||defined(_M_X64)` with runtime dispatch (`__builtin_cpu_supports("avx2")` on GCC/Clang, `__cpuid` on MSVC) and a scalar fallback for every kernel. Use function-level `__attribute__((target("avx2")))` so the TU still compiles without `-mavx2`. This machine has AVX2/BMI2/POPCNT, no AVX-512.

### Step 0 - Byte kernels (`nx_bytes.c`)
- `nx_memchr`, `nx_memrchr`, `nx_count_newlines` (cmpeq + movemask + popcnt), `nx_memmem`.
- `nx_memmem` = rare-byte + first/last-byte SIMD variant [RO: the "generic SIMD" substring search by Wojciech Muła]: broadcast `a=needle[0]`, `b=needle[k-1]`; per 32-byte block `m = movemask(cmpeq(load(h+i),A) & cmpeq(load(h+i+k-1),B))`; for each set bit, `memcmp` the middle. Select which two needle positions to compare by a 256-entry byte-rarity rank (ripgrep heuristic [F]). Default: use two rare positions, not necessarily first/last.
- Scalar Two-Way or Horspool for needles < 32 B when SIMD unavailable.

### Step 1 - Regex front end and reference engine (`nx_re.c`)
1. Parser: RE2-compatible syntax subset -> AST -> HIR. Support: literals, `.`, classes (ASCII + Unicode general categories/scripts via compact range tables, optional), `* + ? {m,n}` greedy/lazy, groups (capturing / non-capturing), alternation, anchors `^ $ \A \z \b \B`, flags `i m s x u`. Reject back-references and lookaround with a clear error (linear-time guarantee). Repetition bound cap: `{m,n}` n <= 1000, NFA node cap 2^20 (return error, never OOM).
2. Compile to Thompson NFA with UTF-8 byte-range automata for classes (suffix-sharing, as regex-automata [F]); `.` in Unicode mode = valid UTF-8 sequences excluding `\n`.
3. **Pike VM** first, as the always-correct oracle and fallback: O(m*n), thread list with sparse set, captures optional.
4. **Lazy DFA** second: forward DFA (finds leftmost-first match END) + reverse DFA anchored at the end (finds START), both over byte equivalence classes.
   ```c
   typedef struct {
     uint32_t *trans;        // [nstates << stride_shift]; value = next_state_id | flags, UNKNOWN=0xFFFFFFFF
     uint8_t   cls[256];     // byte -> class id; class nclasses = EOI
     uint32_t  stride_shift, nstates, dead, start[6];  // start state per look-behind context (text start, line start, after word char, ...)
     size_t    bytes, byte_limit; uint32_t clears;
     /* state interning: open-addressing hash of (ordered NFA state list + look flags) -> id */
   } nx_ldfa;
   // hot loop: s = trans[(s<<shift)+cls[*p++]]; if (s >= SPECIAL) { slow path: unknown -> determinize one state; match/dead -> handle }  (unroll x4)
   ```
   Defaults: `byte_limit = 4 MiB` per thread cache [PROPOSAL; Rust ~2 MiB RO], on full: clear cache; if cleared > 3 times while processing < ~10 bytes per created state, abandon and run Pike VM [RO on constants]. Semantics: leftmost-first (Perl/RE2/Rust default) so results can be differentially tested against Python `re` on the shared subset.
5. Search is buffer-oriented, not line-oriented: run `(?m)`-aware DFA over the whole file buffer and expand a hit to its line only when reporting (lines found by `memrchr`/`memchr` around the hit; line numbers by `nx_count_newlines` computed lazily for returned hits only).

### Step 2 - Literal extraction (`nx_lit.c`), shared by prefilter AND index planner
Output type `Seq = { list of (bytes, exact:bool) } | INFINITE`. Extract prefix, suffix and inner sequences from HIR, with limits (start values from regex-syntax [RO]): `limit_class=10, limit_repeat=10, limit_literal_len=100, limit_total=250`; after extraction: drop literals shorter than 2 bytes (regex-automata [F]), minimise (remove strings having a shorter member as prefix for prefix-seqs), cross-product exact sets only while product <= limit_total, otherwise mark inexact. Case-insensitive: expand each ASCII letter to both cases while product stays <= 16 (matches ripgrep's 16-variant `PM_RE` example [F]), else go inexact.

### Step 3 - Gram index v1: doc-level folded trigrams (ship this first, 1 field)
- Unit: one object field (`content`, `path`, `symbols`, `tensor_names`, ...). One dictionary + one postings file per field per immutable segment.
- Gram extraction over case-folded bytes: ASCII lowercase; for non-ASCII, per-code-point Unicode simple case folding (valid UTF-8 only; invalid bytes pass through). Per-code-point folding is a monoid homomorphism, so `fold(doc) contains fold(literal)` whenever `doc contains literal` -> filter is sound for both case-sensitive and -i queries. Pitfall: folded form can change byte length (e.g. U+212A KELVIN SIGN 3 bytes -> 'k' 1 byte), so operate on the folded stream, never on original offsets.
- Skip any gram containing `\n` or `\0` (queries only use newline-free segments of required literals: splitting a required literal at `\n` keeps it sound).
- Dictionary entry (16 B, sorted by hash): `{uint64 hash; uint32 post_off_in_8B_units; uint32 df;}` -> df available for planning without touching postings. Lookup: top-16-bit "fence table" (65,536 x u32 = 256 KiB) + short binary/interpolation search inside the bucket (hash is uniform) [PROPOSAL].
- Postings: doc IDs sorted ascending; df < 128: delta-varint; larger: 128-doc blocks, bit-packed deltas + per-block `(base, bits, max_doc)` skip entry; df > N/16: raw bitset or Roaring-style containers [RO: Roaring, Lucene 128-doc blocks]. Postings 8-byte aligned.
- Stop-grams [PROPOSAL]: if df > 0.5*N, keep dictionary entry (df only) but store no postings; planner treats as ANY. Removes the largest lists ("for", "the") and the "common trigram" failure mode.
- Per-file guards [RO on constants, from codesearch: skip binary/invalid UTF-8, huge lines, cap distinct trigrams/file ~20k]: do not gram-index files that are binary, > 64 MiB [PROPOSAL], or whose distinct-gram count > 50,000 (minified JS, data dumps); mark them `scan_only` so they are still found by filename/filters and by scanning when explicitly selected.
- Build: parallel gram emission (pool of 8), pairs `(hash, doc)` partitioned by top 8 hash bits into 256 buckets, sorted per bucket, then compressed; spill sorted runs at 64 MiB and k-way merge (SPIMI-style) to stay inside 15 GB RAM / 3 GB disk.
- Query decomposition: Cox analysis (2.1) over the HIR with `max_exact_len=7, max_set=20` [RO]. Evaluate the AND/OR tree by intersecting smallest-df first (galloping search), OR via union; result = `candidate bitmap`.

### Step 4 - Verification and the planner hook
- `content~/re/` and `"substring"` clauses return `(candidate bitmap, exactness flag)`. Pure literal queries with no case-insensitivity and no gram truncation are still verified by `nx_memmem` (cheap).
- Cost-based choice index-vs-scan per clause [PROPOSAL]:
  ```
  cand_est   = min(df_i) bounded by N * prod(df_i / N)         // report both bounds in EXPLAIN
  cost_index = sum(posting_bytes_i)/R_dec + cand_est*avg_doc_bytes/R_ver
  cost_scan  = bytes(filter_bitmap)/R_scan
  use index iff cost_index < cost_scan
  ```
  `R_*` = throughput constants measured by a ~50 ms micro-benchmark at first run and stored (adaptive). Start values: R_dec ~1 GB/s, R_scan ~2 GB/s with a literal prefilter, ~0.3 GB/s plain DFA, R_ver ~0.5 GB/s (all [PROPOSAL] to be replaced by measurement).
- The structured-filter bitmap (type/language/modified) is intersected FIRST. If |filter| is small, skip gram lookups entirely and scan; if a gram posting is longer than 16x the current candidate count, do not read it - verify instead.
- Verification parallelism: chunks of 64 candidate docs to the worker pool; per-thread DFA cache; abort early on top-K when docIDs are static-rank ordered (below).

### Step 5 - Sparse n-gram upgrade (same dictionary/posting format)
- Header fields: `weight_id` (0 = crc32c(pair), 1 = trained rank table), `min_len=3`, `max_len=8` (ngi [2nd]; make configurable up to 16), `fold_id`.
- Weight = `crc32c(a,b)` via `_mm_crc32_u16` when available else table-driven (identical result - the format depends on it). Optional trained table: 65,536-entry `uint8` rank of pair frequency (64 KiB) in the segment header, trained on the first 64 MiB sample then frozen per collection (a retrain implies reindex). Weight = `(255 - rank) << 24 | crc32c_low24` so frequent pairs get low weight, rare high.
- Index: `build_all` (stack algorithm 2.4) with dedup per doc (sort+unique of 64-bit hashes). Query: covering DP using real df. Report in EXPLAIN: grams chosen, df, estimated and actual candidates.
- Acceptance gate: ship sparse mode only if on the project's reference corpora (see 6.5) it lowers median candidates/true-hit ratio by >= 2x vs trigram AND index size grows <= 1.6x. If not, keep trigrams (cheap rollback because trigrams are a subset).

### Step 6 - Multi-literal prefilter (Teddy AVX2) + Aho-Corasick fallback
- Teddy slim, AVX2, fingerprint N = min(3, shortest pattern), 8 buckets, <= 64 patterns; patterns grouped into buckets by shared first bytes (cheap greedy) [PROPOSAL; Hyperscan uses a DP, 2.7]. Verify with `memcmp`. Scalar/SSSE3 fallback.
- > 64 patterns or very short (<2 B) literals: do not use Teddy; compile alternation into the lazy DFA (it already is an automaton over the literals) or a contiguous Aho-Corasick.

### Step 7 - Segments, freshness, WATCH
- Gram files live inside immutable segments (same lifecycle as other indexes). Tombstones are a live-docs bitmap ANDed at the end; they are never edited into postings.
- Merge = k-way merge of sorted dictionaries, concatenating posting lists with doc-ID rebase; must reuse the same weight table/`weight_id` (otherwise re-gram).
- Freshness (Cursor-style layer on a base [F]): recently changed files are NOT gram-indexed immediately; keep a "dirty set" scanned directly with the verifier on every query; flush to a new segment when dirty bytes > 32 MiB or > 5 s idle [PROPOSAL].
- WATCH with regexes: share one prefilter across subscriptions (see Novel #3).

### Step 8 - Symbols, identifier tokens, ranking
- Identifier tokeniser for a BM25 `identifiers` field: scan `[A-Za-z_][A-Za-z0-9_$]*` and emit (a) the whole lowercased identifier, (b) split parts at lower->UPPER, UPPER->UPPER+lower (`HTTPServer` -> `http`,`server`), letter<->digit, `_`, `-`, with parts at the same position as the whole (Lucene WordDelimiter style [RO]). Query `thread pool` and `ThreadPool`, `thread_pool`, `THREAD_POOL` should all hit.
- Symbol extraction without tree-sitter: per-language rule table (data file, plugin-extensible). Pass 1: lexer state machine (code / line comment / block comment / string / raw string / char) tracking brace depth and indent. Pass 2: rules anchored at depth/indent where a definition is legal. Emit `(name, kind, line, parent)`. Starting rules:
  - C/C++: depth-0 `ident ( ... ) {`, `typedef`, `struct|union|enum NAME {`, `#define NAME`.
  - Python: `^\s*(def|class)\s+NAME`; JS/TS: `(function|class|interface|type|enum|const|let)\s+NAME`; Go: `^func (\(...\) )?NAME`, `^type NAME`; Rust: `(fn|struct|enum|trait|mod|const|static|type) NAME`, `impl ... for NAME`; Java/C#: modifiers + type + NAME + `(`; Markdown: `^#+ title`; JSON: key paths.
  Accept some false positives; symbol-match is a ranking feature, not a filter.
- Result features and starting weights [PROPOSAL; tune on a labelled query set]: symbol-definition match 3.0, filename stem exact 2.5, path-component match 1.5, word-boundary match 1.0, substring match 0.5, match inside comment 0.4 / string 0.7, test/vendor/generated path x0.3, shorter path small bonus, recency bonus. Combine as BM25F-style: weighted tf = sum(weight * count), `k1=1.2, b=0.75` over doc length. Zoekt also offers BM25 where matches act as terms [F].
- Static rank order for doc IDs within a segment (non-test, non-vendor, shallow path, recent first) so lazy posting iterators allow top-K early termination (Blackbird [F]).

---

## 4. Skip and why

- Positional trigrams (Zoekt) as default: ~3.5x corpus [F]; too costly for a multi-type local engine. Optional later per-field flag for the `symbols`/`path` fields only.
- 8-bit loc/next masks: saturate on big docs, add 2 B/posting, and sparse grams already give selectivity [F]. Revisit only if sparse mode fails its acceptance gate.
- Suffix array / FM-index for content: rebuild cost on update, 4x corpus for SA [RO], poor regex story [F]. A suffix array over the tiny `symbols`/`path` name dictionaries is possible but the gram index covers the same queries with shared code.
- Full-DFA compilation, one-pass DFA, bounded backtracker (initially): the lazy DFA + Pike VM ladder is enough; add the backtracker/one-pass only when captures become a measured bottleneck (regex-automata uses them for captures [F]).
- Back-references/lookaround: break linear time and defeat gram planning. Provide a `--pcre-lite` fallback only via a plugin, never in the core.
- Hyperscan/Vectorscan/RE2 binding: violates zero-dependency; adopt their ideas (decomposition, Teddy, lazy DFA) instead.
- Unicode normalisation beyond simple case folding inside the gram index (NFC/NFKC, full case folding): changes byte lengths unpredictably. Keep gram index on simple-fold bytes; do language-aware normalisation in the BM25 text field instead.
- Learned/neural n-gram selection: needs training corpora (TB-scale in Cursor's case [F]); not feasible here (3 GB disk). Use the self-trained 64 KiB rank table.
- AVX-512 Teddy variants: not available on this CPU.

---

## 5. Novel ideas specific to NexusSearch

1. **Result-set-driven gram planning.** Because every clause already becomes a bitmap, the gram plan consumes the *current candidate bitmap* as its starting set: tiny filters turn index lookups into direct scans; huge posting lists are skipped by size ratio (3, step 4). Public designs (Zoekt/Cursor/GitHub) index code only and have no structured filter algebra to exploit.
2. **Index-time weights, query-time df.** Grams are *selected* at index time by the (cheap, locality-preserving) weight function, but the *covering* is chosen at query time by DP over the real per-gram df stored in the dictionary. Cursor/GitHub texts describe a weight-only covering [F]; the df-driven variant is a [PROPOSAL] and makes planning self-calibrating with stale or absent frequency tables.
3. **Regex WATCH subscriptions via shared literal prefilter.** Extract required literal sets from all active subscriptions, merge into one Teddy/Aho-Corasick matcher over each new/changed document (Hyperscan's multi-regex insight: strings first, automata only when triggered [F]); map literal -> subscription ids; run only the DFA of subscriptions whose literal hit. N watchers cost ~1 scan instead of N. Hyperscan reports a 4.6x-182x reduction in regex invocations from prefiltering [F].
4. **Multi-type gram fields.** Same engine, different field extractors: safetensors/GGUF/ONNX tensor and metadata names (`blk\.\d+\.attn_.*`), glTF/FBX node, mesh and material names, JSON key paths (`$.users[*].email`), archive entries, model-card text. Enables queries like `type:model tensor~/attn_(q|k|v)_proj/ AND format:gguf` with the same planner and EXPLAIN.
5. **Identifier-squashed gram stream.** For the `symbols` field also gram-index the lowercased identifier with `_`/`-`/case boundaries removed ("threadpool"), so `name~"threadpool"` matches `ThreadPool`, `thread_pool`, `THREAD_POOL` without a regex.
6. **EXPLAIN ANALYZE for regex.** Show: extracted literals, why a clause is non-indexable ("no literal >= 3 bytes", "class > 10", "set > 20"), chosen grams with df, estimated vs actual candidates, verify ratio (hits/candidates), bytes scanned, DFA cache clears and engine used (literal / prefilter+DFA / Pike). A persistently low verify ratio (< 2%) auto-flags a stale weight table for retraining.
7. **Semantic x regex composition.** Gram plan yields a bitmap; filtered vector search takes it as the filter. If the bitmap is < ~1-2% of N, brute-force the vectors of those docs instead of traversing HNSW [PROPOSAL; typical filtered-ANN practice, RO].
8. **Adaptive indexing of regex workloads.** Log literals that repeatedly fall back to scan (no usable gram) and propose adding a dedicated field/extractor (e.g. a numeric-literal or version-string token field).

---

## 6. Validation / test ideas

1. **Soundness invariant (no false negatives).** For random corpora and regexes: `matches(re, corpus) subseteq candidates(plan(re))`. Run for every mode (trigram, sparse, covering, case-insensitive, dirty layer, after merge, after tombstones). This is the single most important test; run it in a fuzz loop.
2. **Query-tree soundness by generation.** From the HIR, *generate* strings that match the regex (random walk through the AST with bounded repeats), embed in random text, assert the gram query evaluates TRUE on the gram set of the text. Covers the Cox exact/prefix/suffix algebra incl. alternation, classes, `?`, `*`, `+`, `{m,n}`.
3. **Sparse-gram locality property test.** For random strings and random substrings: `grams_all(literal) subseteq grams_all(doc)` and `covering(literal) subseteq grams_all(literal)`; every covering chain spans pairs `[0, m-1]` and consecutive grams share a border pair; `|build_all| <= 2m`; all trigrams present.
4. **Differential testing of the engine.** Leftmost-first semantics -> compare span output against Python 3.12 `re` on the common subset (no backrefs/lookaround/possessive; ASCII and UTF-8) over random patterns from a small alphabet `{a,b,c,.,[ab],|,*,+,?,(,),{1,3}}` and random haystacks (length 0..64, alphabet `{a,b,c,\n}`). Also cross-check lazy DFA vs Pike VM vs literal fast path. Note semantic traps: Python `.` and `\b` differences in Unicode mode; `$` before trailing `\n`; empty-match iteration rules.
5. **Reference corpora already on this machine** (no downloads): the CPython `Lib/` (~tens of MB, `.py`), MSYS2 `/ucrt64/include` and `/usr/include` (~100 MB of C headers), the NEXUS repo itself. Never fetch the Linux kernel (disk). Measure per query set: candidates / hits, index bytes / corpus bytes, build MB/s, p50/p99 latency, vs a brute-force `nx_memmem`+DFA scan and vs `rg` if installed (optional).
6. **Adversarial regex suite.** `(a+)+b`, `(x+x+)+y`, `(a|aa)*c`, `.{1000}`, `\p{L}{100}`, 1,000-way word alternation, `(?i)` on 50-char literals, empty alternations, huge classes. Assert: linear-time (time grows ~linearly with haystack), bounded memory (DFA cache <= limit, NFA cap errors cleanly), planner returns "scan" with an EXPLAIN reason instead of exploding.
7. **Teddy tests.** Randomised: pattern sets of 1-64 patterns, lengths 1-12, random haystack with planted matches at every lane offset 0..63 and across 16/32-byte chunk boundaries; compare vs naive multi-memmem. Check AVX2 vs SSSE3 vs scalar outputs identical.
8. **Format/robustness.** Truncated or corrupted dictionary/postings must fail closed (checksum per block), never read OOB; fuzz hash-collision handling by forcing a 8-bit hash (collisions only widen). Test Windows `long`=32-bit hazards: files > 4 GiB offsets use `uint64_t`; mmap via `CreateFileMapping`.
9. **Cost model calibration test.** For 200 random queries, record chosen strategy vs the measured-best strategy (index vs scan); report regret; refit `R_*` constants.
10. **Tokeniser golden tests.** `HTTPServer`, `getHTTPResponseCode`, `parse2JSON`, `snake_case_ID`, `kebab-case`, `ÜberTest`, `x86_64`, `0xDEADBEEF` -> expected sub-tokens + whole token.

---

## 7. Defaults summary (copy into config)

```
gram.min_len=3  gram.max_len=8  gram.fold=simple  gram.skip_newline=1
gram.hash=64-bit  gram.weight=crc32c(pair)  gram.stop_df_ratio=0.5
gram.max_distinct_per_doc=50000  gram.max_file_bytes=64MiB
regex.max_exact_len=7  regex.max_set=20  regex.class_expand=10  regex.max_repeat=1000  regex.nfa_cap=1<<20
literal.limit_class=10 limit_repeat=10 limit_literal_len=100 limit_total=250  min_literal_len=2
dfa.cache_bytes=4MiB  dfa.clear_giveup=3
teddy.max_patterns=64  teddy.buckets=8  teddy.fingerprint=1..3
verify.chunk_docs=64  dirty.flush_bytes=32MiB  dirty.flush_idle=5s
posting.block=128  dict.entry=16B  dict.fence_bits=16
```
Values tagged [RO] or [PROPOSAL] above are starting points to be validated by the benchmark harness in 6.5, not facts.

---

## 8. References

Fetched and read (tags in text):
1. Russ Cox, "Regular Expression Matching with a Trigram Index", 2012. https://swtch.com/~rsc/regexp/regexp4.html
2. Zoekt design doc (Sourcegraph). https://raw.githubusercontent.com/sourcegraph/zoekt/main/doc/design.md
3. GitHub Engineering, "The technology behind GitHub's new code search" (2023). https://github.blog/engineering/architecture-optimization/the-technology-behind-githubs-new-code-search/
4. Cursor, "Fast regex search" (2025 per brief). https://cursor.com/blog/fast-regex-search (read via fetch summariser; details of weight tables/masks should be re-read in the original before coding)
5. `danlark1/sparse_ngrams` (C++17, used in GitHub code search; README only, algorithm docs "TBD"). https://github.com/danlark1/sparse_ngrams
6. `erogol/ngi` DESIGN.md [2nd, third-party re-implementation, claims unverified]. https://github.com/erogol/ngi/blob/main/DESIGN.md
7. Andrew Gallant (BurntSushi), "ripgrep is faster than {grep, ag, git grep, ...}", 2016. https://burntsushi.net/ripgrep/
8. BurntSushi, "Regex engine internals as a library" (regex-automata). https://burntsushi.net/regex-internals/
9. BurntSushi, Teddy README (aho-corasick). https://raw.githubusercontent.com/BurntSushi/aho-corasick/master/src/packed/teddy/README.md
10. X. Wang, Y. Hong, H. Chang, K. Park, G. Langdale, J. Hu, H. Zhu, "Hyperscan: A Fast Multi-pattern Regex Matcher for Modern CPUs", NSDI 2019. https://www.usenix.org/system/files/nsdi19-wang-xiang.pdf (full text read)

Seen only as titles in search results (not read): Infini-gram mini, arXiv 2506.12229 https://arxiv.org/pdf/2506.12229 ; HN thread on GitHub sparse grams https://news.ycombinator.com/item?id=34682472 ; `eliosai/sngram`, `querymt/qndx` (third-party sparse-gram repos).

Recall-only items (no source fetched, verify before use): ClickHouse `sparseGrams` signature/defaults; codesearch constants (maxExact, maxSet, per-file trigram cap, max line length); regex-syntax literal-extractor limits; lazy-DFA cache defaults and give-up heuristics; Muła SIMD substring search; Roaring bitmaps and Lucene 128-doc postings blocks; FM-index sizing; Universal Ctags optlib; Lucene WordDelimiter; Zobel-Moffat-Sacks-Davis 1993 (cited in the Cursor post [F] as the original n-gram-index paper).
