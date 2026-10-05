# NexusQL — query language specification (v1)

NexusQL generalises the MTG engine's `name:"black lotus" colour:b cmc<4` syntax to arbitrary typed objects.
A query is parsed **without** knowledge of the schema into a generic AST (`src/query/nx_ast.h`); a separate
**binder** resolves field names/types, converts values (units, dates) and rejects type errors with suggestions.
The grammar therefore never depends on field types.

## 1. Examples (all must parse; binding depends on the schema)

```
type:model parameters:<10B format:onnx
type:code language:c modified:>2026-01-01 "thread pool"
type:3d polygons:<100000 format:(fbx OR glb) name~"castle"
type:document AND ("WebGPU" OR "WebNN") AND modified:>2026-08-01 NOT archived:true
semantic:"small neural networks suitable for running locally" AND ram:<8GB AND format:(onnx OR gguf)
type:experiment model:supermix* accuracy:>0.80 ram:<2GB
type:paper year:>2024 semantic:"memory consolidation during sleep"
content:/thread_?pool\s*\(/ path:src/** -path:test*
name:cas* size:1MB..5MB modified:>=now-7d
EXPLAIN type:model AND size:<500MB AND author:kai
EXPLAIN ANALYZE type:model parameters:<1B SORT BY modified DESC LIMIT 20
WATCH type:model runtime:webgpu parameters:<1B
SOURCE(local, models) semantic:"webgpu inference" modified:>2026-01-01
```

## 2. Lexical structure

* Whitespace separates tokens. Comments are not supported.
* **Keywords** (UPPERCASE only; quote them to search for the word): `AND OR NOT EXPLAIN ANALYZE WATCH SOURCE SORT BY ASC DESC LIMIT OFFSET FACET`.
  Symbolic forms: `&&` = AND, `||` = OR, `!` or a prefix `-` (immediately before a term/clause/`(`) = NOT.
* **String**: `"..."` with escapes `\" \\ \n \t \uXXXX`; also `'...'`. Unterminated string is a parse error pointing at the opening quote.
* **Regex literal**: `/.../` with optional flags `i`, `s`, `m` (e.g. `/foo.*bar/i`); `\/` escapes the delimiter. A `/` starts a regex only
  in value position (after `:`/`=`/`~`/an operator) or at the start of a clause; otherwise it is part of a word (`src/**`).
* **Word**: a maximal run of characters other than whitespace, `( ) " ' : < > = ! ~ ^ ,` and the sequence `..`.
  Words may contain `* ? - . / \ _ @ # + %` and any non-ASCII byte. A leading `-` makes NOT only when followed by a non-space and
  at a clause boundary. A word of the form `[+-]?digits[.digits][e[+-]digits][suffix]` is a **Number token** (value + raw suffix text,
  e.g. `10B`, `500MB`, `1.5GB`, `3k`, `0.80`, `-5`); the suffix is interpreted by the binder according to the field's unit.
* **Date/time token** (recognised lexically so `-` and `:` inside do not split): `YYYY`, `YYYY-MM`, `YYYY-MM-DD`,
  `YYYY-MM-DDTHH:MM[:SS[.fff]][Z|±HH:MM]`, and relative forms `now`, `today`, `yesterday`, `now-30d`, `now+2h`
  (units `s m h d w mo y`). Partial dates denote ranges (`2026-03` = the whole month) — the binder expands them.
* Punctuation tokens: `( ) : = != < <= > >= ~ ^ , ..`

## 3. Grammar (EBNF; precedence NOT > AND > OR; juxtaposition = AND)

```
statement  := [ EXPLAIN [ANALYZE] | WATCH ] [ SOURCE '(' ident {',' ident} ')' ] [ expr ] { clause }
clause     := SORT BY sortkey {',' sortkey} | LIMIT int | OFFSET int | FACET ident {',' ident}
sortkey    := ident [ASC|DESC]            ; ident may be the pseudo-field _score
expr       := or_expr
or_expr    := and_expr { (OR | '||') and_expr }
and_expr   := unary { [AND | '&&'] unary }          ; implicit AND when two unary expressions are adjacent
unary      := (NOT | '!' | '-') unary | primary
primary    := '(' expr ')' | term
term       := [ field [ params ] opspec ] value [ boost ]    |   value [ boost ]       ; bare value = default-field search
field      := ident                                    ; dotted names allowed: a.b.c
params     := '(' ident '=' value { ',' ident '=' value } ')'   ; e.g. semantic(k=50,min=0.3):"..."
opspec     := ':' [cmp] | cmp | '~'                    ; field:<10B  ==  field<10B ;  name~"castle"  ==  name:~"castle"
cmp        := '=' | '!=' | '<' | '<=' | '>' | '>='
value      := word | string | regex | number | datetime | range | group | call | '*'
range      := scalar '..' scalar                       ; inclusive both ends; either side may be '*' (open)
group      := '(' value { [OR | ','] value } ')'       ; any-of list: format:(onnx OR gguf) ; or a boolean sub-expression, see 4.4
call       := ident '(' [ value { ',' value } ] ')'    ; substr("x") regex(/x/) phrase("a b") near("a b", 5) prefix(x) fuzzy(x, 2) exists()
boost      := '^' number
```

`:` after a field means "matches" with type-dependent semantics (§4). `=`/`!=` always mean exact (equality) match.
The parser never rejects an unknown field or operator/type combination — that is the binder's job.

## 4. Semantics by field type (binder)

| field type | `f:v` | `f="v"` | `f:pre*` | `f~v` | `f<,<=,>,>=,a..b` | other |
|---|---|---|---|---|---|---|
| `keyword` | equals, case/diacritic-insensitive | exact bytes | prefix/wildcard over terms | fuzzy over terms (auto edit distance by length: 0 for ≤2, 1 for ≤5, else 2) | lexicographic range | `f:/re/` regex over terms; `f:*` exists |
| `text` | BM25 term; `f:"a b"` phrase; scored | — | prefix expansion (capped) | fuzzy term expansion, scored by edit distance | — | `f:near("a b",n)` proximity |
| `int`/`float`/`size`/`count` | equals | equals | — | — | numeric ranges (exact) | `f:*` exists |
| `date` | equals / partial-date range | exact instant | — | — | ranges, relative dates | |
| `bool` | `true`/`false`/`yes`/`no`/`1`/`0` | | | | | |
| `code` (text + trigram) | as `text` | | | | | `f:substr("x")` literal substring, `f:/re/` regex (trigram-accelerated, verified) |
| `vector` | `f:"text"` ⇒ embed query text, ANN search | | | | | `f:[0.1,0.2,..]`, `similar:<id>` |

`semantic:"..."` is sugar for the schema's default vector field. Params: `k` (candidate count, default 100), `min` (min similarity),
`ef` (search effort), `field`.

### 4.1 Units
Numeric suffixes depend on the field's `unit` option: `bytes` → `B KB MB GB TB` (powers of 1024, case-insensitive, also `KiB…`);
`count` → `k m b t` (powers of 1000; `10B` = 10 billion); `duration` → `ms s m h d`; `percent`; none → no suffix allowed.
### 4.2 Dates
UTC. `modified:>2026-01-01` ⇒ `> 2026-01-01T00:00:00Z`. `modified:2026-03` ⇒ `[2026-03-01, 2026-04-01)`. `>=now-7d` etc. resolve at query start (`now` is fixed per query).
### 4.3 Defaults
A bare value searches the schema's `default_fields` (text fields; OR across them with per-field weights). A query with no scoring clause (only
filters) returns results ordered by the index's `default_sort` (default `_id ASC`); with any text/fuzzy/semantic clause it is ranked (see ranking spec).
### 4.4 Boolean groups as values
`format:(fbx OR glb)` ≡ `format:fbx OR format:glb`; `name:(foo AND bar)` ≡ `name:foo AND name:bar`; `-` and `NOT` inside a group apply to the field-bound clause.
The parser expands groups into field-bound sub-expressions so the binder sees only clauses.

## 5. Statement forms
* `EXPLAIN` — return the plan with estimated cardinalities and costs, do not execute. `EXPLAIN ANALYZE` — execute, return plan with actual rows/time per node.
* `WATCH` — register a live subscription (percolator): evaluated against each newly committed segment only.
* `SOURCE(a, b)` — federate across named sources (default: the current index).
* `SORT BY f [ASC|DESC], …`, `LIMIT n` (default 20, max configurable), `OFFSET n`, `FACET f, …` (top-term counts for keyword/bool/date-bucket fields).

## 6. AST (informative; authoritative in `src/query/nx_ast.h`)
Node kinds: `AND`, `OR`, `NOT` (n-ary, flattened), `CLAUSE{field?, params[], op, value, boost, span}`. Value kinds: `WORD`, `STRING`, `REGEX{pattern,flags}`,
`NUMBER{double, int64?, raw_suffix}`, `DATETIME{raw}`, `RANGE{lo,hi,lo_open,hi_open}`, `ANYOF[values]`, `CALL{name,args}`, `STAR`, `VECTOR{floats}`.
Every node carries `(offset, length)` spans for error reporting. Statement node carries mode (`NORMAL|EXPLAIN|EXPLAIN_ANALYZE|WATCH`), sources, sort keys, limit, offset, facets.
Limits (enforced, configurable): query length ≤ 64 KiB, nesting depth ≤ 64, clauses ≤ 4096, any-of list ≤ 4096, string ≤ 16 KiB.

## 7. Errors
`nx_error{code=NX_ERR_PARSE, pos, len, msg}`; messages are specific ("expected a value after ':' ", "unterminated string started here", "unbalanced '('"),
and the span points at the offending token. Binder errors add "did you mean `modified`?" using edit distance over schema fields, and list valid operators for the field type.
