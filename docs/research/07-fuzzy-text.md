# 07 - Fuzzy matching, typo tolerance and Unicode normalisation

Scope: how NexusSearch (C11, zero dependencies, AVX2 but no AVX-512, 64-bit Windows where `long` is 32-bit) should do typo-tolerant
term matching, search-as-you-type, ranking by typo count, and the Unicode pipeline (validation, folding, tokenisation, CJK, spoofing).
Written 2026-10-02. Companion notes: 05 (front-coded term dictionary with dense ordinals, Roaring, expansion cap 1,024 terms) and
06 (trigram index) - this note builds on both.

Evidence tags: **[F]** fetched and read in this session (URL in References); **[S]** seen only in a search-result snippet;
**[R]** recall-only (no source fetched - verify before relying on it); **[E]** our own engineering estimate or design (not a literature claim).
Numbers without a tag are from a fetched page.

---------------------------------------------------------------------------------------------------

## 1. Landscape

| System | Typo mechanism | Defaults that matter | Ranking by typos |
|---|---|---|---|
| **SymSpell** (Wolf Garbe) | symmetric-delete: precomputed delete-variants of dictionary terms in a hash map; query generates its own deletes only [F] | max dictionary edit distance 2 [F]; `prefixLength` caps the index (README claims ">90% memory reduction") [F]; default value 7 [R] | pick lowest distance, then highest frequency count (dictionary entries carry a count) [F] |
| **Lucene `FuzzyQuery`** | Levenshtein DFA intersected with sorted term dictionary (FST) [R for the mechanism]; Damerau/OSA with transpositions on by default [F] | hard cap 2 edits ("higher distances ... will match a significant amount of the term dictionary") [F]; `maxExpansions` default 50, `prefixLength` default 0 [R] | terms scored by edit distance via `TopTermsBlendedFreqScoringRewrite` (blended doc-freq) [F]; terms of length 1-2 "will sometimes not match" because the distance is scaled by length [F] |
| **Tantivy** | `levenshtein-automata` crate: Schulz-Mihov parametric DFA, optional transposition [F]; intersected with its FST | distances 1..4 supported; DFA build for the word "Levenshtein": ~36 us (d=1), ~100 us (d=2), ~1.4 ms (d=3), ~8 ms (d=4, with transposition) [F] | by distance |
| **Meilisearch** | bounded edit distance against its word FST; typo is a *ranking rule* in a bucket sort [F for behaviour; internals R] | 1 typo from 5 chars, 2 typos from 9 chars; a typo on the first character counts as **two** typos; 1-4 char query words get prefix matching only; `disableOnWords`/`disableOnAttributes` empty [F] | `typo` rule sorts by increasing typo count (0, then 1, then 2) [F] |
| **Typesense** | Damerau-Levenshtein, fallback-style [F] | `num_typos=2`, `min_len_1typo=4`, `min_len_2typo=7`, `typo_tokens_threshold=1` (typo search only if fewer than N results), `drop_tokens_threshold=1`, `split_join_tokens=fallback`, `prefix=true`, `max_candidates=4` (non-exhaustive) [F] | `_text_match` score; `prioritize_exact_match=true` [F] |
| **BK-tree** | metric tree over the dictionary | needs a true metric (OSA is not one [R]) | n/a |

Two opposite philosophies, both valid: **Meilisearch** always searches typo variants and lets the ranking rule order them; **Typesense**
gates typo expansion on a result-count threshold. NexusSearch needs both modes because the planner (EXPLAIN) wants to know when
expansion is worth its cost.

Academic anchors: Schulz & Mihov universal Levenshtein automata (2002; the crate README cites it) [F]; Mihov & Schulz, "Fast approximate
search in large dictionaries" (Comput. Linguistics 2004) [S]; Gerdjikov, Mihov, Mitankin, Schulz, "Good parts first" (arXiv 1301.0722) which
splits the pattern, aligns promising substrings *exactly* with the lexicon and then extends both ways, reporting "significant efficiency
improvements" (no numbers on the abstract page) [F]; Myers 1999 and Hyyro 2001-2005 bit-vector edit distance [F via blog, S].
Recent (2023) arXiv 2308.01976 (Microsoft AppSource) shows learned typo correction needs domain-specific *synthetic* training data and
query logs; it is not a dictionary-based method and does not compare against SymSpell on its abstract page [F] - irrelevant for a local-first engine without logs.
Unicode state of the art: Unicode **18.0.0** (CaseFolding-18.0.0.txt dated 2026-02-03 [F]; UTS #39 v18.0.0 revision 34 dated 2026-08-27 [F]).
Python 3.12's `unicodedata` is Unicode 15.0 [R], so the table generator must read the UCD files, not Python's module.
Fast UTF-8 validation: Keiser & Lemire "lookup" algorithm, "more than 10 times" faster than common library routines, "less than one instruction per byte" [F].

---------------------------------------------------------------------------------------------------

## 2. Techniques

### 2.1 Bit-parallel edit distance (Myers 1999, Hyyro 2001/2003) - the verification kernel

Source: G. Myers, JACM 46(3), 1999 [R: doi 10.1145/316542.316550]; H. Hyyro's reformulation, Damerau extension and banded/diagonal
tiling [S, F via blog]; walkthrough https://curiouscoding.nl/posts/approximate-string-matching/ [F]; reference implementation edlib [R].
Core idea: encode the DP column as vertical delta bit-vectors (`Pv` = +1 deltas, `Mv` = -1 deltas); one text char updates the whole
column (m <= 64 rows) in ~15 word operations ("approximately 15 operations per column", both formulations) [F]. A pattern longer than w
is split in word-blocks with a carry of the horizontal delta `hin/hout in {-1,0,+1}` between blocks [F].

Pattern = the query term (<= 64 code points), text = a dictionary term. Global distance, with early exit and an optional "prefix distance"
(min over j of D[m][j], which is what as-you-type needs):

```c
typedef struct { uint32_t cp[64]; uint64_t mask[64]; int n; } nx_peq;      // tiny map: code point -> row mask (<=64 distinct)
static int myers(const nx_peq *P, int m, const uint32_t *t, int n, int k, int prefix_mode) {
  uint64_t Pv = (m == 64) ? ~0ull : ((1ull << m) - 1), Mv = 0, hi = 1ull << (m - 1);
  int score = m, best = m;                         // score = D[m][j]
  if (!prefix_mode && (m > n ? m - n : n - m) > k) return k + 1;
  for (int j = 0; j < n; j++) {
    uint64_t Eq = peq_get(P, t[j]);                // 0 if char not in pattern
    uint64_t Xv = Eq | Mv;
    uint64_t Xh = (((Eq & Pv) + Pv) ^ Pv) | Eq;
    uint64_t Ph = Mv | ~(Xh | Pv), Mh = Pv & Xh;
    if (Ph & hi) score++; else if (Mh & hi) score--;
    Ph = (Ph << 1) | 1;                            // global distance: D[0][j] = j  (semi-global search: no "| 1")
    Mh <<= 1;
    Pv = Mh | ~(Xv | Ph);  Mv = Ph & Xv;
    if (score < best) best = score;
    if (prefix_mode) { if (best <= k) return best; }
    if (score - (n - 1 - j) > k && best > k) return k + 1;   // D[m][.] can fall by at most 1 per column
  }
  return prefix_mode ? best : score;
}
```
Junk bits above row m-1 never propagate downward (shifts and carries go up), so no masking is needed beyond reading bit m-1.
Multi-block version (edlib-style, [R]): per 64-row block `Xv=Eq|Mv; if(hin<0) Eq|=1; Xh=...; hout = (Ph&hi)?+1 : (Mh&hi)?-1 : 0; Ph<<=1; Mh<<=1;
if(hin<0) Mh|=1; else if(hin>0) Ph|=1; ...`. Use it only for terms 65..256 code points; fuzzy on longer terms is disabled (exact/prefix only).

**Damerau / transpositions (decision).** Use **OSA** (restricted Damerau; what Lucene does [F]; Typesense also calls its metric Damerau-Levenshtein [F]).
Hyyro's bit-vector Damerau extension exists [S] but I cannot reproduce its recurrence from a fetched source, so **do not hand-derive it**.
Correct-by-construction hybrid [E]: OSA <= Lev <= 2*OSA (a transposition costs 1 in OSA, 2 in Lev). So: run `myers(..., k_lev = 2k)`;
`lev <= k` accept (dist = lev); `lev > 2k` reject; otherwise run an exact banded OSA DP (Ukkonen cutoff, O(k*n), width 2k+1).
For k=1 the middle case (lev == 2) is just "same length, differ in exactly two adjacent positions, swapped" - an O(n) check. Few candidates
reach the DP, so cost is negligible. Later, replace by Hyyro's Damerau variant once validated differentially (section 7).

Gains: edlib/Myers is the standard fastest method when k/m is large [S]; for our k <= 2, m ~ 5-12 the win is that verification of one candidate is
~50 ns [E: ~15 ops x ~10 chars at ~3 GHz] instead of a ~100-cell DP. Pitfalls: Peq lookup for non-ASCII - use the 64-slot open-addressing map above,
not a 0x110000 array; compute on *folded code points*, never bytes; the `| 1` shift-in differs between global and search mode (classic bug).

### 2.2 SymSpell symmetric delete (Garbe) - candidate generation

Source: https://github.com/wolfgarbe/SymSpell (2012-) [F]. Idea: instead of generating deletes+transposes+replaces+inserts of the *query*
(~3 million variants for a 5-letter word at distance 3), precompute only **deletes** of every dictionary term and generate only deletes of the query:
"transposes + replaces + inserts of the input term are transformed into deletes of the dictionary term" - 25 deletes cover that word [F].
If ed(q,t) <= d then the delete-neighbourhoods del<=d(q) and del<=d(t) intersect (a substitution = one delete on each side; an insertion = a delete on the longer side;
two substitutions need 2+2 deletes, so a "1+1" split only gives d=1 - the full d-deep index is required for d=2).
Claims by the author [F]: ~1,000,000x faster than Norvig (dict 29,157, d=3), 1,870x faster than a BK-tree (dict 500,000, d=3), 0.033 ms/word at d=2 and
0.180 ms/word at d=3. These are the author's own benchmarks on his hardware - treat as an order of magnitude only. A third-party comparison snippet
reports 0.39 ms/word vs 44.15 ms/word for a Damerau-Levenshtein trie [S].
Pitfall named by the author: index size/precalculation explode with edit distance [F]. Variants per term for a prefix of length p with <=2 deletes
are `1 + p + C(p,2)`: p=5 -> 16, 6 -> 22, **7 -> 29**, 8 -> 37, 10 -> 56 [E, arithmetic]. Prefix capping is not obviously sound when q and t have different
lengths (the 7-prefix of the longer string reaches further); it works because the delete sets still intersect (e.g. "abcdefgh" vs "bcdefgh", d=1 share "bcdefg"),
but verify empirically against brute force before trusting P < 12 (test 7.2).

**Nexus adaptation ("NXFZ" sidecar, built per immutable segment per fuzzy-enabled text field).** Store hash-only entries and let Myers resolve collisions:
```
HDR  : magic 'NXFZ' | u32 ver | u8 P(7) | u8 d1_min_qlen(5) | u8 d2_min_qlen(9) | u8 hash_bits(32) | u32 nterms | u64 nentries | u8 dir_bits | u64 dir_off | u64 ent_off
DIR  : u32 start[2^dir_bits + 1]           // bucket start index by (hash >> (32 - dir_bits)); dir_bits = ceil(log2(nentries/8))
ENT  : { u32 hash ; u32 term_ord }[nentries], sorted by (hash, ord), unique   // 8 B/entry; v2: delta+bitpack to ~5 B
```
Which variants a term contributes (query tiers 5/9 imply term tiers): term length 4-6 -> deletes of the whole term with <=1 deletion; length >= 7 -> deletes of its first P=7 chars with <=2
(the `d1_min-1` and `d2_min-2` rule, since a d-edit match can differ in length by d). Terms shorter than 4 are exact-only. Expected ~21 entries/term for a natural vocabulary [E]
-> 1M terms ~ 170 MB uncompressed, ~105 MB packed; typical Nexus fields (1e4-1e5 terms) -> 0.2-2 M entries, <= 17 MB. Build only if 2,000 <= nterms <= 2,000,000
(below: brute-force Myers over length buckets, ~0.6 ms for 20k terms [E]; above: d=1 only, or the trigram path in 2.4). Sidecar is a pure function of the term dictionary, so it is
rebuilt at merge time and never mutated (matches immutable segments in the design).
Lookup:
```
expand(q, cps[L]):  d = L<5 ? 0 : L<9 ? 1 : 2                   // tiers [F: Meilisearch 5/9]; configurable per field
  if d == 0: return exact(q)
  seen = {}                                                    // small open-addressing set of ords
  for v in variants(cps[:min(L,P)], <= d deletes):             // includes v == the prefix itself (0 deletes)
      for e in bucket(hash32(v)) with e.hash == hash32(v): seen.add(e.ord)
  for ord in seen: t = term(ord)                               // front-coded block decode (note 05)
      if |len(t)-L| > d: continue
      dist = osa_verify(q, t, d); cost = dist + (q[0] != t[0] ? 1 : 0)    // first-char typo = 2 [F: Meilisearch]
      if cost <= d: out.add(ord, dist, cost)
  keep best 50 by (cost asc, df desc)                           // maxExpansions 50 [R: Lucene default]
```

### 2.3 Levenshtein automata (Schulz-Mihov; Lucene, Tantivy)

Source: crate README https://github.com/tantivy-search/levenshtein-automata [F] (Schulz & Mihov 2002 parametric construction; builder `new(max_distance, transposition)`,
`build_dfa(word)`; construction costs listed in section 1 [F]). Idea: a *universal* automaton for distance n is precomputed once (states = sets of (offset, errors) positions relative to the
current char, input = characteristic bit-vectors of the next 2n+1 chars of the query); instantiating it for a concrete word is cheap. The DFA is then intersected with the sorted
dictionary: Lucene/Tantivy walk the FST in lock-step; on a sorted array one uses "next accepted string >= current" seeks (each a lower_bound over the block heads, note 05) [R for the Lucene
mechanism]. Gains: no per-term verification and no index larger than the dictionary; cost is proportional to matched terms x log N plus gaps. Pitfalls: generating the
parametric tables (Lucene ships Python-generated classes for n=1,2 +/- transpositions [R]) is intricate; the walk wants a trie/FST, our front-coded blocks need a block-seek
protocol; build time grows steeply with n (8 ms at d=4 [F]), which is why every engine caps at d=2 [F].

### 2.4 q-gram count filter over the term dictionary (reuse note 06's trigram index)

Source: Ukkonen 1992 q-gram lemma [R]. With q=3 and `^^`/`$$` padding a string of length n has n+2 trigrams; one edit destroys at most q of them, so
ed(s,t) <= d => |common trigrams| >= max(|s|,|t|) + q - 1 - q*d (d=1: >= max-1; d=2: >= max-4; for n=9: 5 of 11). Candidate generation = ScanCount/merge of the
term-level trigram postings (trigram -> term ordinals), then Myers verify. Use when the term vocabulary is above the sidecar cap (code identifiers, file paths) or a segment is too small
to deserve a sidecar. Weak for n < 6 at d=2 (then forbid d=2 anyway). This costs no extra index if note 06's trigram dictionary is keyed on terms as well as on raw text [E].

### 2.5 Prefix search / search-as-you-type

Last query token is a prefix (Typesense `prefix=true` default [F]). In a front-coded sorted dictionary all terms with a prefix are a contiguous ordinal range `[lo,hi)` (note 05): one
lower_bound pair, no expansion list. Meilisearch: query words of 1-4 chars get no typos [F]. Policy: `|q| < 5` -> prefix only; `|q| >= 5` -> exact-prefix range **plus** whole-word
fuzzy expansion of q (users notice typos at the end of a word, so q is usually near-complete). If the prefix range holds > 1,024 terms, switch to constant-score union (note 05, Lucene
rewrite [R]); in typeahead mode cap candidates at 8 (Typesense non-exhaustive default is 4 [F]) [E].
Prefix-distance via `prefix_mode` in 2.1 verifies "q is within d edits of some prefix of t" in one pass.

### 2.6 Ranking by typo count and score explanations

Meilisearch: typo ranking rule = ascending typo count, exact first [F]; Typesense: `prioritize_exact_match` [F]; Lucene: fuzzy terms weighted by distance relative to length [F; the exact formula
`boost = 1 - d/min(|q|,|t|)` is [R]]. NexusSearch hybrid ranker input per matched query term: `(term_ord, dist, cost, was_prefix)`:
`w = max(0, 1 - dist / min(|q|,|t|))` (0.8 for d=1,L=5; 0.78 for d=2,L=9) multiplies the term's BM25 contribution; IDF uses the *blended* df = max df over the expansion
group (Lucene's blended-frequency rewrite [F, name]) so that a rare misspelling in the corpus does not out-score the correct word. `typos_total = sum(cost)` is exposed as a separate feature
(for Meilisearch-style bucket tie-breaks and for EXPLAIN: `castel -> castle d=1 w=0.83 df=1204`).

### 2.7 Jaro-Winkler and phonetic keys (names)

Jaro-Winkler [R: Winkler 1990]: m = matching chars within window floor(max(|a|,|b|)/2)-1, t = half the transposed matches; `jaro = (m/|a| + m/|b| + (m-t)/m)/3`;
`jw = jaro + l*p*(1-jaro)` with prefix length l <= 4, p = 0.1, boost applied only if jaro > 0.7. It is *not* a candidate generator (no index); use it only to **re-rank**
the <= 50 verified expansions or the top-K results of `name~` queries. Phonetic keys (Soundex, Metaphone) [R] are English-biased and conflate too much for identifiers: skip (section 4).

### 2.8 Compound handling (split/join)

SymSpell `LookupCompound` handles a wrongly inserted or omitted space [F]; Typesense `split_join_tokens=fallback` [F]. Cheap in Nexus: join adjacent OOV tokens if the concatenation
is a dictionary term (`castle wall` -> `castlewall`); split an OOV token into two dictionary terms of >= 3 chars each (O(L) dictionary probes). Run only when the plain query returns
under `typo_tokens_threshold` hits (gating).

### 2.9 UTF-8 validation (Keiser-Lemire lookup algorithm)

Source: arXiv 2010.03090, "Validating UTF-8 In Less Than One Instruction Per Byte" [F]: >10x faster than common validators, <1 instr/byte. Mechanism [R]: three 16-entry nibble lookups
(`vpshufb`) - high nibble of the previous byte, low nibble of the previous byte, high nibble of the current byte - ANDed, yielding bit-flags for the error classes (too short, too long,
overlong 2/3/4, surrogate, too large, two continuations); a separate check that 3-/4-byte leaders are followed by the right number of continuations. Implementation plan: (a) scalar DFA fallback
(Hoehrmann-style table, ~9 states [R]); (b) AVX2, 32 bytes/iteration, with an ASCII fast path (`_mm256_movemask_epi8` == 0 -> skip); runtime dispatch via `__builtin_cpu_supports("avx2")` / cpuid
(no AVX-512 on this CPU). Tail < 32 bytes: copy to a zero-padded 32-byte buffer. Reference implementations: simdutf, simdjson [R]. Policy: invalid input is replaced by U+FFFD at ingest
(documented lossy) or the document is rejected, per source policy; never index raw invalid bytes.

### 2.10 Folding tables: what to embed and how to generate it

Standards: UAX #15 (normalisation) [R]; CaseFolding.txt statuses C (common), F (full, string growth e.g. sharp-s), S (simple), T (Turkic) [F]; Default_Ignorable_Code_Point in
DerivedCoreProperties.txt [R]. UTS #39 recommends NFKC + casefold for identifier comparison [F]. Do **not** implement full NFC (needs ~1k-pair composition table and canonical composition
logic) - search keys do not need to be NFC. **Design [E]:** key form `fold(x) = reorder_ccc( NFKD( CF( NFKD( strip(x) ) ) ) )` iterated to a per-code-point fixed point by the generator, where
* `CF` = CaseFolding statuses **C + F** (no T, so no Turkic dotless-i locale behaviour; U+0130 folds to `i` + U+0307, then the mark is stripped);
* NFKD uses UnicodeData.txt decomposition mappings (field 5, including `<compat>`) - no composition table; Hangul syllables are decomposed algorithmically (SBase/LBase arithmetic [R]);
* **strip set** (diacritic folding) = combining marks in U+0300-036F, U+1AB0-1AFF, U+1DC0-1DFF, U+FE20-FE2F plus optional Hebrew points U+0591-05C7 and Arabic harakat U+064B-065F.
  Never strip all Mn: Japanese voicing marks U+3099/309A, Indic vowel signs/virama and Thai marks are meaning-bearing;
* **extra letters** with no decomposition derived from character *names* by the generator: `LATIN (SMALL|CAPITAL) LETTER X WITH (STROKE|BAR|...)` -> `x` (o-stroke, d-stroke, l-stroke, h-bar, t-stroke), plus ae, oe, th, eth by an explicit 6-line list;
  Katakana U+30A1-30F6 -> Hiragana (subtract 0x60);
* **remove** Default_Ignorable code points (zero-width joiners/space, soft hyphen, variation selectors, bidi controls) from search keys;
* canonical reorder only for runs of non-zero-ccc code points that survive the strip (Hebrew/Arabic/Indic): insertion sort by ccc over a run capped at 30 non-starters (Stream-Safe Text Format limit, UAX #15 [R]).

Per-code-point (stateless) mapping means the hot path is a table lookup: two-stage table, `u16 idx1[0x110000 >> 7]` (8,704 entries, 17 KB) -> `u16 blk[unique][128]` (identity block deduplicated) -> value
`u32 = off:21 | len:5 | flags:6` into a `u32 seq[]` pool; ASCII short-circuit (`A-Z` -> `a-z` with one OR, no table). Max expansion of one code point under NFKD is 18 code points (U+FDFA) [R]; allocate
`18 * n_cp` or process chunked, so a hostile 1 MB input cannot blow memory. Expected table size: tens of KB [E] - the generator must print the real size and fail CI above a budget (say 96 KB).

Generator `tools/gen_unicode.py` (Python 3, stdlib only, ~150 lines [E]): download `UnicodeData.txt`, `CaseFolding.txt`, `DerivedCoreProperties.txt`, `confusables.txt` (UTS #39 data) for a pinned Unicode version
(18.0.0 today [F]); delete after generation (a few MB total [R]). Emit `src/unicode/nx_unicode_tables.inc` stamped with `NX_UNICODE_VERSION`; write the version into every segment header so a
future table upgrade is detected (Unicode's normalisation stability policy means existing characters' normalisation never changes [R], so only *new* characters differ).

### 2.11 Grapheme clusters vs code points; CJK

Edit distance and tier thresholds count **code points of the folded key** - for Latin/Greek/Cyrillic after the strip step code point == user-perceived character. Extended grapheme clusters
(UAX #29 [R], Grapheme_Cluster_Break table ~ a few thousand ranges [R]) are for UI only (highlight spans, cursor, truncation). Fuzzy is **disabled by default** for Thai, Lao, Khmer, Myanmar and
Indic scripts (no word breaks, mark-heavy clusters) - trigram/exact only.
CJK (Han, Hiragana, Katakana, Hangul): no spaces. Lucene's approach is overlapping **bigrams** of adjacent CJK characters (CJKBigramFilter, optional unigrams) [R]. Nexus: split text into runs by
Script property; Latin-class runs use letter/digit/apostrophe word breaks (simplified UAX #29 rules, not the full table); CJK runs emit overlapping bigrams **plus unigrams** (a 1-char query
needs the unigram: bigrams starting with X form a range, bigrams ending with X do not); a CJK phrase `ABC` = positional phrase over bigrams AB, BC. No edit-distance fuzz on CJK
(distance over bigrams is meaningless); approximate matching there = bigram Dice overlap through the trigram/bigram index (note 06).

### 2.12 Security of normalisation (homoglyphs, spoofing)

UTS #39 [F]: `skeleton(s)` = NFD -> remove Default_Ignorable -> map each code point to its confusables prototype -> NFD again; "mixed-script" = empty resolved script set; whole-script
confusables (all-Cyrillic "аррӏе" vs Latin "apple") are not caught by mixed-script checks, hence skeletons. Rules for NexusSearch:
1. **Never fold confusables for ranking** (would conflate Cyrillic `а` with Latin `a` and break Russian queries). Store `skeleton(name)` as a hidden field `name.skel` for **spoof detection only**;
   a result whose skeleton equals the query skeleton but whose `fold` differs gets `spoof=true` and a UI warning.
2. Fold forms are index keys, **never paths**: NFKC/NFKD maps fullwidth solidus U+FF0F to `/`; normalise-then-validate bugs enable path traversal. Validate paths on the raw, UTF-8-validated bytes.
3. Strip/flag bidi controls (U+202A-202E, U+2066-2069) and zero-width characters when *displaying* names ("Trojan Source", arXiv 2111.00169 [R]).
4. Bound work: combining-mark runs capped at 30 (stream-safe), expansion factor 18 per code point, max token length 256 code points, max expansions 50 per term.
5. Typosquatting is the same machinery: `d <= 1` neighbours of a popular name (section 6).

---------------------------------------------------------------------------------------------------

## 3. Data layout summary

* `NXTD` dictionary (note 05): `ord -> term`; fuzzy never copies terms.
* `NXFZ` sidecar (2.2): `hash32 -> ord` entries, per field, per segment; optional; header records P and tier bounds.
* Per-segment `unicode_version` and `fold_profile` (`basic|latin_fold|cjk`) in the segment header; tokens stored already folded.
* Stored original text is kept; highlighting maps folded offsets back to original offsets (keep a `u32 orig_off[]` per token position or recompute by re-running the folder on the stored snippet).

---------------------------------------------------------------------------------------------------

## 4. Adopt for NexusSearch (ordered; all C11, no deps)

| Step | Deliverable | Parameters / acceptance |
|---|---|---|
| 0 | `nx_utf8.c`: scalar validate/decode/encode; AVX2 Keiser-Lemire validate behind runtime dispatch | differential fuzz vs scalar and vs Python `bytes.decode`; >= 5 GB/s on ASCII-heavy input [E target] |
| 1 | `tools/gen_unicode.py` + `nx_fold.c`: fold table (2.10), `nx_fold(utf8_in, out, mode)`; tokenizer with script runs + CJK bigram/unigram | Unicode 18.0.0 pinned; table <= 96 KB; fold idempotent |
| 2 | `nx_edit.c`: reference DP (full OSA) for tests; Ukkonen banded OSA; Myers 64-bit (global + prefix + early exit); block Myers for 65..256; hybrid OSA verify | k <= 2; term <= 256 cps; Myers single-word verify <= 100 ns per candidate on 10-char terms [E] |
| 3 | `NXFZ` sidecar builder (at flush/merge) + `expand()` (2.2) + brute-force scan path for tiny dictionaries + trigram path (2.4) for big vocabularies | P=7, tiers 5/9 [F: Meilisearch], max 50 expansions, first-char typo costs +1, caps 2k..2M terms |
| 4 | Planner integration: fuzzy clause = `FuzzyExpand(term_set) -> bitmap union`; cost = lookups + verifications; `fuzzy=auto|always|off` with Typesense-style gate `typo_tokens_threshold=1` (expand a bare word only if its exact df < 1) | EXPLAIN prints `variants=…, candidates=…, verified=…, matched=…, df_sum=…`; `name~"x"`, `~1`, `~2`, `~0` syntax (Lucene precedent [R]) forces fuzzy regardless of the gate |
| 5 | Ranker features: `typos_total`, per-term weight `w`, blended IDF, first-char penalty; Jaro-Winkler rerank for `name~` on name fields | weights as 2.6; exact < 1 typo < 2 typos must hold as an invariant |
| 6 | Prefix: ordinal range + constant-score fallback + `prefix_mode` Myers; typeahead cap 8 | prefix-only below 5 chars |
| 7 | Security: `skeleton()` (confusables.txt via generator), `name.skel` field, `spoof` flag, bidi/ZW display sanitiser | see 2.12 |
| 8 (stretch) | Prefix-delete sidecar (section 6, idea 3); AVX2 lane-parallel Myers (4 dictionary terms per `__m256i` against one query pattern); Hyyro Damerau bit-vector | only if profiling shows verification dominates |

Default parameter sheet: tiers 5/9 (per-field override; Typesense uses 4/7 [F]); max distance 2; metric OSA; unit = folded code point; first-char typo cost +1;
P=7; `max_expansions=50` (typeahead 8); prefix range cap 1,024 terms; sidecar built for 2,000 <= nterms <= 2,000,000; fuzzy term length <= 256 cps; combining run cap 30;
fuzzy disabled for CJK/Thai/Lao/Khmer/Myanmar/Indic; casefold = C+F (no Turkic).

---------------------------------------------------------------------------------------------------

## 5. Skip and why

* **BK-tree**: needs a metric (OSA breaks the triangle inequality [R]); author's benchmark has SymSpell 1,870x faster at 500k terms, d=3 [F].
* **Full Schulz-Mihov DFA + FST/seek walk in v1**: powerful (Tantivy/Lucene) but the parametric-table generator and block-seek protocol are heavy; SymSpell sidecar + Myers delivers the same results with simple code. Revisit if vocabularies exceed ~5M terms/field.
* **d >= 3, true Damerau**: Lucene itself says d>2 "not useful" [F]; build cost explodes (8 ms DFA at d=4 [F]).
* **Neural/learned spell correction** (arXiv 2308.01976 class [F]): requires domain query logs and training; contradicts zero-dependency local-first design.
* **Soundex/Metaphone**: English-only, low precision on identifiers and model names [R].
* **Full NFC/NFKC composition, UCA collation, ICU-style bidi**: not needed for matching; saves ~tens of KB and a lot of code. Display-time bidi is the UI's job.
* **Locale-sensitive casefold (Turkic)**: ambiguous per document; skip T mappings [F: T is "not typical defaults"].
* **Good-parts-first / Pass-Join partition indexes** [F/R]: need a bidirectionally extensible lexicon or give weak selectivity on 3-char partitions at d=2; the delete-hash is more selective.
* **Transliteration (pinyin, romaji)**: separate analyzer topic.

---------------------------------------------------------------------------------------------------

## 6. Novel ideas specific to NexusSearch

1. **Filter-aware fuzzy expansion.** The planner already has the structured result set as a Roaring bitmap (`type:model AND ...`). Before a fuzzy term is OR-ed in, compute
   `|posting(candidate) AND filter|` (Roaring AND-cardinality is cheap) and **drop expansions with zero hits inside the filter**. `type:model name~"lama"` keeps `llama` and drops a
   high-df non-model word; typo tolerance becomes domain-aware, and EXPLAIN reports exact post-filter cardinalities instead of dictionary-wide ones.
2. **Dictionary as a free spell-checker for the query language.** Field names and enumerated values (`format:onxx`, `modifed:`) are tiny dictionaries: brute-force Myers over a few hundred entries
   gives "did you mean `format:onnx`" on empty results at ~microseconds; the same for unknown functions/operators. No sidecar needed.
3. **Prefix-delete sidecar with ordinal ranges.** Because the dictionary is sorted, each distinct prefix of length 4-8 maps to a contiguous ordinal range `[lo,hi)`.
   Index the delete-variants of *distinct prefixes* (not terms) and store `(hash, lo, hi)`: typo-tolerant search-as-you-type (`casl` -> `castle*`) with an index that is
   much smaller than the per-term sidecar and results that are already ranges (cheap union via note 05's constant-score path).
4. **Typosquat / spoof facet for model & package registries.** With federated or plugin-ingested sources (AI model hubs) `typosquat_of:<name>` = results with d <= 1 (or equal `skeleton`)
   of a more popular name (higher `downloads` facet). Same machinery as fuzzy plus the Roaring facet; a security feature competitors do not expose.
5. **Frequency prior from the user's own corpus and history** (SymSpell dictionaries carry counts [F]): among equal-cost expansions prefer higher df, then terms the user previously clicked
   (local, private, stored in a small side file) - no cloud logs needed, unlike learned correctors [F].
6. **Segment-lifecycle-aware cost model.** Tiny NRT segments (< 2,000 terms) skip the sidecar (brute-force scan), merged segments get one; the planner picks `scan | sidecar | trigram` per segment and prints it in EXPLAIN.
7. **Identifier-aware units.** For `code`/`file` names split camelCase/snake_case/kebab and letter-digit boundaries *before* tiering so `llama3.1-8b` and `llama 3.1 8B` fuzzy-match per sub-token (distance is computed per sub-token, tier by sub-token length).

---------------------------------------------------------------------------------------------------

## 7. Validation and test ideas

1. **Edit-distance kernels (differential).** 1e7 random pairs per alphabet (sizes 2, 4, 26, mixed Unicode), lengths 0..70 (hit 63/64/65, 127/128/129 for block carries), k in 0..3:
   Myers == full DP (Levenshtein); hybrid OSA verify == full OSA DP; prefix-mode == min over prefixes of the DP; early exit never changes the answer when result <= k.
2. **Sidecar recall.** Dictionary of 100k words (synthetic + a small word list); queries = dictionary words with 1-2 random edits (insert/delete/substitute/transposition) and length mixes 4..14.
   `expand()` must equal brute-force Myers over the whole dictionary (recall 100% for tiers actually in force); sweep P in {5,6,7,8,12} to quantify the prefix-cap loss
   (this settles the soundness doubt in 2.2). Record candidates/query, verified/query, p50/p99 latency (targets [E]: d=1 < 100 us, d=2 < 1 ms on 1M terms; SymSpell's own 0.033 ms/word at d=2 [F] is the best-case reference), and bytes/term.
3. **UTF-8.** Random byte fuzz vs Python `bytes.decode('utf-8')` (accept/reject must agree); crafted overlong, surrogate (ED A0..BF), > U+10FFFF (F4 90+), truncated sequences at every
   offset 0..63 inside 32-byte blocks; scalar vs AVX2 equality.
4. **Folding.** Generator output vs an independent oracle: for every code point assigned in both Unicode 15.0 (Python 3.12 `unicodedata` [R]) and 18.0.0, `fold(cp)` must equal
   iterate(`unicodedata.normalize('NFKD', s.casefold())`) followed by the documented strip/extra-letter rules; check idempotence `fold(fold(x)) == fold(x)` over all code points; for the canonical-ordering
   path run the Unicode `NormalizationTest.txt` NFKD column [R]; golden strings: `Straße`->`strasse`, `İstanbul`->`istanbul`, `Ørsted`->`orsted`, `ｆｕｌｌｗｉｄｔｈ`, `ﬁ`, `が`/`か゛` equal, Hebrew/Arabic pointed vs unpointed.
5. **Hostile input.** 1 MB of combining marks (Zalgo) and 1 MB of U+FDFA must fold in bounded time/memory (limits: run cap 30, expansion 18x); no output buffer overrun (we have no ASan on this compiler,
   so use explicit canary bytes around buffers and `assert` bounds in debug builds).
6. **Security.** Skeleton equality for `paypal` / `раураl` (Cyrillic) and a whole-script pair; mixed-script detection; path-validation test that `％２ｅ`/`／` never survive into a filesystem path.
7. **Ranking.** Golden queries asserting exact < 1-typo < 2-typo order; first-letter typo excluded below length 9; blended-IDF test (rare misspelled term must not outrank the correct word); typo-bucket EXPLAIN output stable.
8. **Quality set.** Mutate 1,000 real titles/filenames with 1-2 typos at the rates in the tier table; report success@1/@10 with typo tolerance on/off, and false-positive rate on short words (length 4-6).
9. **CJK.** Round-trip phrase tests (`ABC` phrase over bigrams), 1-char query via unigrams, fullwidth/halfwidth Latin equal, katakana/hiragana fold.

---------------------------------------------------------------------------------------------------

## 8. References

[F] fetched and read; [S] snippet only; [R] recall-only (URL given for locating, not verified).

* [F] SymSpell README, Wolf Garbe: https://github.com/wolfgarbe/SymSpell
* [S] SymSpell vs BK-tree (blog): https://seekstorm.com/blog/symspell-vs-bk-tree/
* [S] Comparison SymSpell vs Damerau-Levenshtein trie (0.39 ms vs 44.15 ms): https://www.researchgate.net/publication/377449168_A_Comparison_Between_SymSpell_and_a_Combination_of_Damerau-Levenshtein_Distance_with_the_Trie_Data_Structure
* [F] Meilisearch typo tolerance settings: https://www.meilisearch.com/docs/learn/relevancy/typo_tolerance_settings
* [F] Typesense search parameters: https://typesense.org/docs/latest/api/search.html
* [F] Lucene FuzzyQuery (9.10): https://lucene.apache.org/core/9_10_0/core/org/apache/lucene/search/FuzzyQuery.html
* [F] levenshtein-automata (Tantivy, Schulz-Mihov): https://github.com/tantivy-search/levenshtein-automata ; author blog https://fulmicoton.com/posts/levenshtein/ [S via README]
* [S] Mihov & Schulz, Fast approximate search in large dictionaries: https://www.cis.uni-muenchen.de/people/Schulz/Pub/aspaperCISreport.pdf
* [F] Gerdjikov, Mihov, Mitankin, Schulz, Good parts first: https://arxiv.org/abs/1301.0722
* [F] Myers/Hyyro walkthrough (Groot Koerkamp): https://curiouscoding.nl/posts/approximate-string-matching/
* [R] Myers, JACM 46(3) 1999, doi 10.1145/316542.316550 ; edlib https://github.com/Martinsos/edlib
* [S] Hyyro, bit-vector algorithm for Levenshtein and Damerau distances: https://www.researchgate.net/publication/220673231_A_Bit-Vector_Algorithm_for_Computing_Levenshtein_and_Damerau_Edit_Distances ; Bit-parallel approximate string matching with transposition: https://www.sciencedirect.com/science/article/pii/S157086670400053X
* [F] Keiser & Lemire, Validating UTF-8 in less than one instruction per byte: https://arxiv.org/abs/2010.03090 ; code [R] https://github.com/simdutf/simdutf
* [F] Ubrangala et al. 2023, typo-tolerant spell checkers in marketplaces: https://arxiv.org/abs/2308.01976
* [F] UTS #39 Unicode Security Mechanisms (18.0.0, 2026-08-27): https://www.unicode.org/reports/tr39/
* [F] CaseFolding-18.0.0.txt (2026-02-03): https://www.unicode.org/Public/UCD/latest/ucd/CaseFolding.txt
* [R] UAX #15: https://www.unicode.org/reports/tr15/ ; UAX #29: https://www.unicode.org/reports/tr29/ ; UAX #44 and `DerivedCoreProperties.txt`, `UnicodeData.txt`, `NormalizationTest.txt`: https://www.unicode.org/Public/UCD/latest/ucd/ ; `confusables.txt`: https://www.unicode.org/Public/security/latest/confusables.txt
* [R] Lucene CJKBigramFilter; Ukkonen 1992 q-gram lemma; Winkler 1990 Jaro-Winkler; Boucher & Anderson "Trojan Source" arXiv 2111.00169
* Companion notes: 05-bitmaps-columns-dicts.md (front-coded dictionary, ordinals, FST), 06-regex-code-search.md (trigram index)
