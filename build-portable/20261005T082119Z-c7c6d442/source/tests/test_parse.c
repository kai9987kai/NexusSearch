#include "query/nx_parse.h"
#include "nx_test.h"

static const char *const examples[] = {
    "type:model parameters:<10B format:onnx",
    "type:code language:c modified:>2026-01-01 \"thread pool\"",
    "type:3d polygons:<100000 format:(fbx OR glb) name~\"castle\"",
    "type:document AND (\"WebGPU\" OR \"WebNN\") AND modified:>2026-08-01 NOT archived:true",
    "semantic:\"small neural networks suitable for running locally\" AND ram:<8GB AND format:(onnx OR gguf)",
    "type:experiment model:supermix* accuracy:>0.80 ram:<2GB",
    "type:paper year:>2024 semantic:\"memory consolidation during sleep\"",
    "content:/thread_?pool\\s*\\(/ path:src/** -path:test*",
    "name:cas* size:1MB..5MB modified:>=now-7d",
    "EXPLAIN type:model AND size:<500MB AND author:kai",
    "EXPLAIN ANALYZE type:model parameters:<1B SORT BY modified DESC LIMIT 20",
    "WATCH type:model runtime:webgpu parameters:<1B",
    "SOURCE(local, models) semantic:\"webgpu inference\" modified:>2026-01-01",
    "semantic(k=50,min=0.3,field=embedding):\"cats\"^2",
    "year:(>2000 AND <2010) name:(foo AND NOT bar)^2",
    "f:((a OR b),c) AND x:(a b) OR NOT NOT q",
    "code:near(\"a b\",5) x:exists() y:prefix(foo) z:fuzzy(x,2)",
    "vec:[0.1,-0.2,3e-2] date:2024-02-29T23:59:59.123456789+01:30",
    "x:*..4 y:-5..* a:/a\\/b\\d+/ims t:'it\\\'s'",
    "SORT BY _score DESC,a ASC LIMIT 0 OFFSET 12 FACET type,author",
    "a:now+2h b:today-1mo c:yesterday d:2026-03 x:1e-5GB",
    "x:\"\\uD83D\\uDE80\\n\\t\\\\\\\"\"\t字:搜索",
    "a || b && !c -d", "", "*", "f:(a OR b)^2", "f:(a^2 OR b)^3"
};

static void roundtrip(const char *q) {
    nx_arena a;
    nx_arena_init(&a, 256);
    nx_error e;
    nx_stmt *s = NULL, *t = NULL;
    nx_status st = nx_query_parse(&a, q, strlen(q), NULL, &s, &e);
    NX_CHECK_MSG(st == NX_OK, "parse '%s': %s at %lld", q, e.msg, (long long)e.pos);
    if (st == NX_OK) {
        nx_buf b, c, dump;
        nx_buf_init(&b); nx_buf_init(&c); nx_buf_init(&dump);
        nx_ast_print_stmt(&b, s);
        st = nx_query_parse(&a, nx_buf_cstr(&b), b.len, NULL, &t, &e);
        NX_CHECK_MSG(st == NX_OK, "canonical '%s': %s", nx_buf_cstr(&b), e.msg);
        if (st == NX_OK) {
            NX_CHECK_MSG(nx_ast_equal_stmt(s, t), "not equal: '%s' -> '%s'", q, nx_buf_cstr(&b));
            nx_ast_print_stmt(&c, t);
            NX_CHECK_EQ_S(nx_buf_cstr(&b), nx_buf_cstr(&c));
        }
        nx_ast_dump_sexpr(&dump, s);
        NX_CHECK(dump.len > 0 && !dump.oom);
        nx_buf_clear(&dump);
        nx_ast_dump_sexpr_spans(&dump, s);
        NX_CHECK(dump.len > 0 && !dump.oom);
        nx_buf_free(&b); nx_buf_free(&c); nx_buf_free(&dump);
    }
    nx_arena_free(&a);
}

static void test_examples(void) {
    for (size_t i = 0; i < NX_ARRAY_LEN(examples); i++) roundtrip(examples[i]);
}

static void test_shape_and_values(void) {
    nx_arena a;
    nx_arena_init(&a, 0);
    nx_stmt *s = NULL;
    const char *q = "a AND (b AND c) OR NOT d";
    NX_CHECK_OK(nx_query_parse(&a, q, strlen(q), NULL, &s, NULL));
    if (s) {
        NX_CHECK_EQ_I(s->where->kind, NX_NODE_OR);
        NX_CHECK_EQ_U(s->where->u.list.n, 2);
        NX_CHECK_EQ_U(s->where->u.list.kids[0]->u.list.n, 3);
        NX_CHECK_EQ_U(s->nclauses, 4);
    }
    q = "format:(fbx OR glb)";
    NX_CHECK_OK(nx_query_parse(&a, q, strlen(q), NULL, &s, NULL));
    if (s) {
        NX_CHECK_EQ_I(s->where->kind, NX_NODE_CLAUSE);
        NX_CHECK_EQ_I(s->where->u.clause.value->kind, NX_VAL_ANYOF);
        NX_CHECK_EQ_U(s->nclauses, 1);
    }
    q = "f:((a OR b) AND NOT c)^2";
    NX_CHECK_OK(nx_query_parse(&a, q, strlen(q), NULL, &s, NULL));
    if (s) {
        NX_CHECK_EQ_I(s->where->kind, NX_NODE_AND);
        NX_CHECK_EQ_U(s->nclauses, 3);
        NX_CHECK_EQ_S(s->where->u.list.kids[1]->u.list.kids[0]->u.clause.field.s, "f");
        NX_CHECK_NEAR(s->where->u.list.kids[1]->u.list.kids[0]->u.clause.boost, 2, 0);
    }
    q = "x:-9223372036854775808 y:9223372036854775808 z:1.5GB";
    NX_CHECK_OK(nx_query_parse(&a, q, strlen(q), NULL, &s, NULL));
    if (s) {
        const nx_node *n = s->where;
        NX_CHECK(n->u.list.kids[0]->u.clause.value->u.number.has_int);
        NX_CHECK_EQ_I(n->u.list.kids[0]->u.clause.value->u.number.i, INT64_MIN);
        NX_CHECK(!n->u.list.kids[1]->u.clause.value->u.number.has_int);
        NX_CHECK_EQ_S(nx_number_suffix(n->u.list.kids[2]->u.clause.value).s, "GB");
    }
    nx_arena_free(&a);
}

static void test_rejections_and_rollback(void) {
    static const char *const bad[] = {
        "x:", "x:>", "(x", "x)", "a OR", "AND a", "x:\"abc", "x:/abc", "x:/a/z",
        "x:\"\\q\"", "x:\"\\u0000\"", "x:\"\\uD800\"", "x:\"\\uDC00\"", "x:(a AND)",
        "x:()", "x:1..", "x:1..2..3", "x:1e9999", "x:2023-02-29", "x:2026-13",
        "x:2026-02-01T24:01", "x:now-9223372036854775809d", "x:[1,,2]", "x:[1MB]",
        "LIMIT -1", "LIMIT 1.5", "LIMIT 2MB", "LIMIT 1 LIMIT 2", "SORT a", "FACET", "SOURCE()",
        "x^nan", "x^1e9999", "x^2MB", "x:foo(1,)", "semantic(k=):x", "NOT", "- x"
    };
    nx_arena a;
    nx_arena_init(&a, 64);
    char *sentinel = nx_arena_strdup(&a, "preserve");
    size_t before = a.total;
    for (size_t i = 0; i < NX_ARRAY_LEN(bad); i++) {
        nx_stmt *s = (nx_stmt *)(uintptr_t)1;
        nx_error e;
        nx_status st = nx_query_parse(&a, bad[i], strlen(bad[i]), NULL, &s, &e);
        NX_CHECK_MSG(st == NX_ERR_PARSE, "expected parse error for '%s', got %s", bad[i], nx_status_str(st));
        NX_CHECK(s == NULL);
        NX_CHECK_EQ_U(a.total, before);
        NX_CHECK(e.pos >= 0 && (uint64_t)e.pos <= strlen(bad[i]));
        NX_CHECK(e.msg[0] != 0);
        NX_CHECK_EQ_S(sentinel, "preserve");
    }
    nx_stmt *s = NULL;
    nx_error e;
    NX_CHECK_EQ_I(nx_query_parse(&a, "x:\"abc", 6, NULL, &s, &e), NX_ERR_PARSE);
    NX_CHECK_EQ_I(e.pos, 2);
    NX_CHECK_EQ_I(nx_query_parse(&a, "x\0y", 3, NULL, &s, &e), NX_ERR_PARSE);
    nx_arena_free(&a);
}

static void test_limits(void) {
    nx_parse_limits lim = nx_parse_limits_default();
    nx_arena a;
    nx_arena_init(&a, 0);
    nx_stmt *s;
    lim.max_query_bytes = 3;
    NX_CHECK_EQ_I(nx_query_parse(&a, "abcd", 4, &lim, &s, NULL), NX_ERR_LIMIT);
    lim = nx_parse_limits_default(); lim.max_depth = 4;
    NX_CHECK_EQ_I(nx_query_parse(&a, "((((((x))))))", 13, &lim, &s, NULL), NX_ERR_LIMIT);
    lim = nx_parse_limits_default(); lim.max_clauses = 2;
    NX_CHECK_EQ_I(nx_query_parse(&a, "a b c", 5, &lim, &s, NULL), NX_ERR_LIMIT);
    lim = nx_parse_limits_default(); lim.max_list_items = 2;
    NX_CHECK_EQ_I(nx_query_parse(&a, "f:(a,b,c)", 9, &lim, &s, NULL), NX_ERR_LIMIT);
    lim = nx_parse_limits_default(); lim.max_string_bytes = 2;
    NX_CHECK_EQ_I(nx_query_parse(&a, "\"abc\"", 5, &lim, &s, NULL), NX_ERR_LIMIT);
    lim = nx_parse_limits_default(); lim.max_arena_bytes = 32;
    NX_CHECK_EQ_I(nx_query_parse(&a, "x", 1, &lim, &s, NULL), NX_ERR_LIMIT);
    NX_CHECK(s == NULL);
    nx_arena_free(&a);
}

static nx_status oom_scenario(void *ctx) {
    (void)ctx;
    nx_arena a;
    nx_arena_init(&a, 64);
    nx_stmt *s = NULL;
    const char *q = "EXPLAIN ANALYZE SOURCE(local,other) semantic(k=50,min=0.3):\"cats\" AND f:(a OR b) AND x:[1,2,3] SORT BY x DESC LIMIT 5 FACET x,y";
    nx_status st = nx_query_parse(&a, q, strlen(q), NULL, &s, NULL);
    if (st != NX_OK) NX_CHECK(s == NULL);
    if (st == NX_OK) {
        nx_buf b;
        nx_buf_init(&b);
        nx_ast_print_stmt(&b, s);
        if (b.oom) st = NX_ERR_NOMEM;
        nx_buf_free(&b);
    }
    nx_arena_free(&a);
    return st;
}
static void test_oom(void) { nx_test_oom_sweep(oom_scenario, NULL, 512); }

static void test_mutations(void) {
    nx_rng rng = nx_test_rng(321);
    for (size_t it = 0; it < nx_test_iters(2500); it++) {
        const char *src = examples[it % NX_ARRAY_LEN(examples)];
        size_t n = strlen(src);
        char q[512];
        memcpy(q, src, n);
        if (n) {
            size_t pos = (size_t)(nx_rng_u64(&rng) % n);
            if (it % 3 == 0) n = pos;
            else if (it % 3 == 1) q[pos] = (char)(nx_rng_u64(&rng) & 255u);
            else { memmove(q + pos + 1, q + pos, n - pos); q[pos] = '('; n++; }
        }
        nx_arena a;
        nx_arena_init(&a, 256);
        nx_stmt *s = NULL;
        nx_status st = nx_query_parse(&a, q, n, NULL, &s, NULL);
        NX_CHECK(st == NX_OK || st == NX_ERR_PARSE || st == NX_ERR_LIMIT);
        NX_CHECK((st == NX_OK) == (s != NULL));
        if (st == NX_OK) {
            nx_buf b;
            nx_buf_init(&b);
            nx_ast_print_stmt(&b, s);
            nx_stmt *t = NULL;
            NX_CHECK_OK(nx_query_parse(&a, (const char *)b.data, b.len, NULL, &t, NULL));
            if (t) NX_CHECK(nx_ast_equal_stmt(s, t));
            nx_buf_free(&b);
        }
        nx_arena_free(&a);
    }
}

static void test_float_roundtrip(void) {
    nx_rng rng = nx_test_rng(942);
    for (size_t i = 0; i < 1000; i++) {
        double boost = (double)(nx_rng_u64(&rng) % 1000000) / 997.0;
        char text[80]; (void)snprintf(text,sizeof text,"value^%.17g",boost);
        roundtrip(text);
    }
}

static void test_year_precision(void) {
    nx_arena a; nx_arena_init(&a,0); nx_stmt *s = NULL;
    const char *q = "modified:2024 LIMIT 2024";
    NX_CHECK_OK(nx_query_parse(&a,q,strlen(q),NULL,&s,NULL));
    if (s) {
        NX_CHECK_EQ_I(s->where->u.clause.value->kind,NX_VAL_DATETIME);
        NX_CHECK_EQ_I(s->where->u.clause.value->u.datetime.v.prec,NX_DT_PREC_YEAR);
        NX_CHECK_EQ_I(s->limit,2024);
    }
    nx_arena_free(&a);
}

int main(void) {
    NX_RUN(test_examples);
    NX_RUN(test_shape_and_values);
    NX_RUN(test_rejections_and_rollback);
    NX_RUN(test_limits);
    NX_RUN(test_oom);
    NX_RUN(test_mutations);
    NX_RUN(test_float_roundtrip);
    NX_RUN(test_year_precision);
    return nx_test_summary();
}
