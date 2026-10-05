/* nx_ast.h - NexusQL abstract syntax tree (see docs/QUERY_LANGUAGE.md).
 *
 * PURPOSE
 *   A schema-free syntax tree produced by nx_parse.h and consumed by the binder. The parser never looks at
 *   field types, so every field name / operator / value combination that is syntactically valid is representable
 *   here; rejecting it is the binder's job.
 *
 * SHAPE
 *   statement (nx_stmt)  = mode + sources + `where` expression + sort keys + limit/offset + facets
 *   expression (nx_node) = AND | OR | NOT | CLAUSE
 *     AND/OR  n-ary, n >= 2, FLATTENED: `a AND (b AND c)` is AND(a,b,c); children are never AND (resp. OR) nodes.
 *     NOT     unary (exactly one child). `NOT NOT a` stays NOT(NOT(a)): no simplification is ever applied.
 *     CLAUSE  [field [params]] op value [^boost]. The ONLY leaf kind: field groups are expanded by the parser
 *             (below), so the binder never sees a "group" node.
 *   value (nx_value)     = WORD STRING REGEX NUMBER DATETIME RANGE ANYOF CALL STAR VECTOR
 *
 * FIELD-GROUP EXPANSION (QUERY_LANGUAGE.md 4.4)
 *   `format:(fbx OR glb)`            -> ONE clause  format : ANYOF[fbx, glb]            (compact any-of form)
 *   `name:(foo AND NOT bar)`         -> AND( name:foo , NOT(name:bar) )                 (each item = field-bound clause)
 *   `year:(>2000 AND <2010)`         -> AND( year:>2000 , year:<2010 )                  (an item may carry its own op)
 *   `f:(a OR b)^2`                   -> ANYOF clause with boost 2; for expanded groups the boost is MULTIPLIED into
 *                                       every produced clause (so a clause's `boost` is always its effective boost).
 *   A group whose items are all plain values joined by OR / ',' (no AND, NOT, per-item op or boost, no nesting
 *   beyond redundant parentheses) collapses to the ANYOF clause; everything else is expanded to AND/OR/NOT of
 *   clauses that share the group's field, params and operator. `f:(a)` is simply `f:a`. Inside a group,
 *   juxtaposition is AND (`name:(foo bar)` = name:foo AND name:bar), `,` is a synonym of OR.
 *
 * OWNERSHIP / LIFETIME
 *   Every node, value, string and array is allocated from the nx_arena handed to nx_query_parse() and lives exactly
 *   as long as that arena. All `const char *` strings are NUL-terminated copies owned by the arena (the query text
 *   is NOT retained). Nothing here is ever freed individually. Pointers inside the tree are never NULL unless a
 *   field's comment says so.
 *
 * SPANS
 *   Every node/value/name carries (offset, length) BYTE spans into the original query text (offsets are valid for the
 *   text passed to nx_query_parse). For a CLAUSE the node span covers the whole term (field..boost); for clauses
 *   produced by group expansion it covers just the item (`b` in `f:(a OR b)`), while field.span still points at the
 *   group's field name and op_span at the group's operator, so a binder error can underline exactly the right thing.
 *
 * THREAD SAFETY
 *   A tree is immutable after parsing: any number of threads may read/print it concurrently. Parsing itself touches only
 *   the caller's arena (no globals).
 */
#ifndef NX_AST_H
#define NX_AST_H

#include "core/nx_config.h"
#include "core/nx_buf.h"
#include "core/nx_arena.h"

typedef struct nx_span { uint32_t off; uint32_t len; } nx_span;

/* NUL-terminated arena string; len excludes the terminator (strings may contain any byte but NUL). */
typedef struct nx_str { const char *s; uint32_t len; } nx_str;
typedef struct nx_name { const char *s; uint32_t len; nx_span span; } nx_name;   /* s == NULL: absent */

/* ------------------------------------------------------------------ operators */
typedef enum nx_op {
    NX_OP_MATCH = 0,   /* `f:v`   type-dependent match          (also: bare value)   */
    NX_OP_EQ,          /* `f=v`   `f:=v`   exact equality                              */
    NX_OP_NE,          /* `f!=v`  `f:!=v`                                              */
    NX_OP_LT,          /* `f<v`   `f:<v`                                               */
    NX_OP_LE,          /* `f<=v`  `f:<=v`                                              */
    NX_OP_GT,          /* `f>v`   `f:>v`                                               */
    NX_OP_GE,          /* `f>=v`  `f:>=v`                                              */
    NX_OP_FUZZY        /* `f~v`   `f:~v`                                               */
} nx_op;

/* ------------------------------------------------------------------ values */
typedef enum nx_value_kind {
    NX_VAL_WORD = 0,   /* bare word (never a keyword / number / date / `*`)                        text                   */
    NX_VAL_STRING,     /* "..." or '...' with escapes already decoded                               text                   */
    NX_VAL_REGEX,      /* /pattern/ims ; `\/` already unescaped to `/`, every other escape kept     regex                  */
    NX_VAL_NUMBER,     /* [+-]digits[.digits][e[+-]digits][suffix]; raw text always kept            number                 */
    NX_VAL_DATETIME,   /* YYYY-MM[-DD[THH:MM[:SS[.f]][Z|+-HH:MM]]] or now/today/yesterday[+-Nunit]  datetime               */
    NX_VAL_RANGE,      /* lo..hi inclusive; either end may be `*` (open: pointer NULL, flag set)    range                  */
    NX_VAL_ANYOF,      /* any-of list from a field group (>= 2 items, never nested)                 anyof                  */
    NX_VAL_CALL,       /* name(arg, ...)                                                            call                   */
    NX_VAL_STAR,       /* `*` alone (exists / match-all)                                            (no payload)           */
    NX_VAL_VECTOR      /* [0.1, 0.2, ...]                                                           vec                    */
} nx_value_kind;

#define NX_REGEX_ICASE    1u    /* flag i */
#define NX_REGEX_DOTALL   2u    /* flag s */
#define NX_REGEX_MULTI    4u    /* flag m */

typedef enum nx_dt_prec {       /* how much of the timestamp was written (partial dates denote ranges) */
    NX_DT_PREC_MONTH = 0,       /* YYYY-MM           : the whole month                                  */
    NX_DT_PREC_DAY,             /* YYYY-MM-DD        : the whole day                                    */
    NX_DT_PREC_MINUTE,          /* ...THH:MM         : one minute                                       */
    NX_DT_PREC_SECOND,          /* ...THH:MM:SS      : one second                                       */
    NX_DT_PREC_FRACTION,        /* ...THH:MM:SS.fff  : an instant (nanosecond resolution)               */
    NX_DT_PREC_YEAR             /* YYYY: the whole year; appended to preserve existing enum values   */
} nx_dt_prec;

typedef enum nx_rel_base { NX_REL_NONE = 0, NX_REL_NOW, NX_REL_TODAY, NX_REL_YESTERDAY } nx_rel_base;
typedef enum nx_rel_unit {
    NX_UNIT_NONE = 0, NX_UNIT_SECOND, NX_UNIT_MINUTE, NX_UNIT_HOUR, NX_UNIT_DAY, NX_UNIT_WEEK, NX_UNIT_MONTH, NX_UNIT_YEAR
} nx_rel_unit;

/* Pre-validated date/time (calendar-checked: month 1-12, day within the month incl. leap years, hour 0-23, ...).
 * Absolute form: rel_base == NX_REL_NONE and year..nanos are valid. Relative form: rel_base != NONE, rel_amount is signed
 * (`now-30d` -> amount -30, unit DAY; no offset -> amount 0, unit NONE) and year..tz are zero. All times are UTC unless
 * has_tz (then tz_minutes is the offset EAST of UTC; `Z` is has_tz with 0). The binder resolves `now` once per query. */
typedef struct nx_datetime {
    nx_rel_base base_rel;     /* NX_REL_NONE for absolute timestamps (named base_rel to keep `rel_base` free for users) */
    nx_dt_prec  prec;
    int32_t     year;
    uint8_t     month, day, hour, minute, second;     /* month/day are 1-based; day/hour/minute/second 0 when not written */
    uint32_t    nanos;
    bool        has_tz;
    int16_t     tz_minutes;
    nx_rel_unit rel_unit;
    int64_t     rel_amount;
} nx_datetime;

typedef struct nx_value nx_value;
struct nx_value {
    nx_value_kind kind;
    nx_span span;
    union {
        nx_str text;                                    /* WORD, STRING                                            */
        struct { nx_str pattern; uint32_t flags; } regex;
        struct {                                        /* NUMBER                                                  */
            nx_str raw;                                 /*   full source text incl. sign and suffix ("-5", "10B")  */
            double d;                                   /*   numeric part as a double (suffix NOT applied)         */
            int64_t i;                                  /*   exact value when has_int                              */
            bool has_int;                               /*   literal was a plain integer that fits in int64        */
            uint32_t suffix_off;                        /*   offset of the suffix inside raw (== raw.len if none)  */
        } number;
        struct { nx_str raw; nx_datetime v; } datetime;
        struct { nx_value *lo, *hi; bool lo_open, hi_open; } range;   /* open end: pointer NULL and flag true     */
        struct { nx_value **items; uint32_t n; } anyof;
        struct { nx_str name; nx_value **args; uint32_t nargs; } call;
        struct { float *v; uint32_t n; } vec;
    } u;
};

/* The unit suffix of a NUMBER ("B", "MB", "k", "" ...), pointing into the arena copy of the raw text. */
NX_ALWAYS_INLINE nx_str nx_number_suffix(const nx_value *v) {
    nx_str s = { v->u.number.raw.s + v->u.number.suffix_off, v->u.number.raw.len - v->u.number.suffix_off };
    return s;
}

/* ------------------------------------------------------------------ expression nodes */
typedef struct nx_param {                               /* semantic(k=50, min=0.3): ... */
    nx_name name;
    nx_value *value;
    nx_span span;                                       /* name .. value */
} nx_param;

typedef struct nx_clause {
    nx_name field;              /* field.s == NULL: bare value, searched in the schema's default fields              */
    nx_param *params;           /* nparams entries (NULL when 0)                                                      */
    uint32_t nparams;
    nx_op op;                   /* NX_OP_MATCH for a bare value                                                       */
    nx_span op_span;            /* `:`, `:>=`, `~` ...; {0,0} for a bare value                                         */
    nx_value *value;            /* never NULL                                                                         */
    double boost;               /* effective boost, 1.0 when absent                                                   */
} nx_clause;

typedef enum nx_node_kind { NX_NODE_AND = 0, NX_NODE_OR, NX_NODE_NOT, NX_NODE_CLAUSE } nx_node_kind;

typedef struct nx_node nx_node;
struct nx_node {
    nx_node_kind kind;
    nx_span span;
    union {
        struct { nx_node **kids; uint32_t n; } list;    /* AND/OR: n >= 2 ; NOT: n == 1                              */
        nx_clause clause;                               /* NX_NODE_CLAUSE                                             */
    } u;
};

/* ------------------------------------------------------------------ statement */
typedef enum nx_stmt_mode { NX_STMT_NORMAL = 0, NX_STMT_EXPLAIN, NX_STMT_EXPLAIN_ANALYZE, NX_STMT_WATCH } nx_stmt_mode;
typedef enum nx_sort_dir { NX_SORT_DEFAULT = 0, NX_SORT_ASC, NX_SORT_DESC } nx_sort_dir;   /* DEFAULT: neither written */

typedef struct nx_sort_key { nx_name field; nx_sort_dir dir; } nx_sort_key;   /* field may be the pseudo-field _score */

typedef struct nx_stmt {
    nx_stmt_mode mode;
    nx_name *sources;           /* SOURCE(a, b): nsources entries, NULL/0 = default (current index)                  */
    uint32_t nsources;
    nx_node *where;             /* NULL for a query without an expression (matches everything)                       */
    nx_sort_key *sort;          /* SORT BY ... ; nsort entries                                                       */
    uint32_t nsort;
    bool has_limit;  int64_t limit;     nx_span limit_span;    /* limit  >= 0 when has_limit; span of the integer     */
    bool has_offset; int64_t offset;    nx_span offset_span;   /* offset >= 0 when has_offset                         */
    nx_name *facets;            /* FACET a, b                                                                         */
    uint32_t nfacets;
    uint32_t nclauses;          /* number of CLAUSE nodes in `where` (after group expansion)                         */
    nx_span span;               /* first token .. last token                                                         */
} nx_stmt;

/* ------------------------------------------------------------------ names */
NX_API const char *nx_op_str(nx_op op);                 /* ":" "=" "!=" "<" "<=" ">" ">=" "~"  (dump spelling)       */
NX_API const char *nx_node_kind_str(nx_node_kind k);    /* "and" "or" "not" "clause"                                 */
NX_API const char *nx_value_kind_str(nx_value_kind k);
NX_API const char *nx_stmt_mode_str(nx_stmt_mode m);    /* "NORMAL" "EXPLAIN" "EXPLAIN ANALYZE" "WATCH"              */

/* ------------------------------------------------------------------ canonical printer
 * Writes a canonical NexusQL text of the tree into `b` (appends; check b->oom afterwards). ROUND-TRIP INVARIANT:
 * for every successfully parsed query q, parse(print(parse(q))) is structurally identical (nx_ast_equal) to parse(q),
 * and print is idempotent (print(parse(print(parse(q)))) == print(parse(q))) provided the canonical text fits the
 * parse limits. Canonical conventions: keywords spelled out (AND/OR/NOT, never && || ! -), explicit AND, minimal
 * parentheses by precedence, ops spelled `:` `=` `!=` `:<` `:<=` `:>` `:>=` `~`, trailing clauses in the fixed order
 * SORT BY, LIMIT, OFFSET, FACET after the optional `EXPLAIN [ANALYZE]|WATCH` and `SOURCE(...)` prefix. */
NX_API void nx_ast_print_stmt(nx_buf *b, const nx_stmt *s);
NX_API void nx_ast_print_node(nx_buf *b, const nx_node *n);
NX_API void nx_ast_print_value(nx_buf *b, const nx_value *v);

/* ------------------------------------------------------------------ S-expression dump (tests / debugging)
 *   stmt   (query [EXPLAIN|EXPLAIN-ANALYZE|WATCH] [(source a b)] [EXPR] [(sort f f:asc g:desc)] [(limit N)] [(offset N)] [(facet a b)])
 *   node   (and X Y ..)  (or X Y ..)  (not X)  (c FIELD OP VALUE [^BOOST])   FIELD is `-` for a bare value, or
 *          `name` / `name{k=VALUE k=VALUE}` with params; OP is one of : = != < <= > >= ~
 *   value  w(word)  s("string")  re(/pat/ims)  n(raw)  dt(raw)  *  range(LO HI) with `*` for an open end,
 *          any(V V ..)  call(name V ..)  vec(f f ..)
 * With spans, every node/value head is followed by `@off,len`, e.g. (c@0,10 type : w(model)@5,5). */
NX_API void nx_ast_dump_sexpr(nx_buf *b, const nx_stmt *s);                    /* no spans  */
NX_API void nx_ast_dump_sexpr_spans(nx_buf *b, const nx_stmt *s);              /* with spans */
NX_API void nx_ast_dump_node(nx_buf *b, const nx_node *n, bool spans);
NX_API void nx_ast_dump_value(nx_buf *b, const nx_value *v, bool spans);

/* ------------------------------------------------------------------ structural equality (spans are ignored) */
NX_API bool nx_ast_equal_value(const nx_value *a, const nx_value *b);
NX_API bool nx_ast_equal_node(const nx_node *a, const nx_node *b);
NX_API bool nx_ast_equal_stmt(const nx_stmt *a, const nx_stmt *b);

#endif /* NX_AST_H */
