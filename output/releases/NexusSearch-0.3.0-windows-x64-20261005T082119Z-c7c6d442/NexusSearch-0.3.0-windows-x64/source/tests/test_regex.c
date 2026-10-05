#include "nx_test.h"
#include "index/nx_regex.h"

static void expect_match(const char *pattern, const char *text, uint32_t flags, bool want) {
    nx_regex *re = NULL; nx_error e;
    nx_status status = nx_regex_compile(nx_slice_cstr(pattern), flags, &re, &e);
    NX_CHECK_OK(status);
    if (status == NX_OK) {
        bool got = false;
        NX_CHECK_OK(nx_regex_match(re, nx_slice_cstr(text), 0, &got));
        NX_CHECK_MSG(got == want, "pattern %s on %s expected %d got %d", pattern, text, want, got);
    }
    nx_regex_free(re);
}
static void test_regex_language(void) {
    expect_match("thread_?pool\\s*\\(", "a thread_pool ( x)", 0, true);
    expect_match("^(ab|cd)+[0-9]{2,4}$", "abcd12", 0, true);
    expect_match("^(ab|cd)+[0-9]{2,4}$", "abcd1", 0, false);
    expect_match("a.*b", "a\nb", 0, false);
    expect_match("a.*b", "a\nb", NX_RE_DOTALL, true);
    expect_match("^alpha$", "x\nALPHA\nz", NX_RE_MULTILINE | NX_RE_ICASE, true);
    expect_match("^[^a-z]+$", "ABC", NX_RE_ICASE, false);
    expect_match("^\\w+\\s\\d+$", "a_B 123", 0, true);
    expect_match("^(a?)*$", "aaaa", 0, true);
    expect_match("", "anything", 0, true);
    expect_match("a{0}", "", 0, true);
    expect_match("a{2,}", "baaaaa", 0, true);
}
typedef struct oracle_atom { char c, quantifier; } oracle_atom;
static bool oracle(const oracle_atom *atoms, size_t n, const char *text, size_t length, size_t at) {
    if (!n) return at == length;
    size_t minimum = atoms[0].quantifier == '?' || atoms[0].quantifier == '*' ? 0 : 1;
    size_t maximum = atoms[0].quantifier == '*' || atoms[0].quantifier == '+' ? length - at : 1;
    if (maximum > length - at) maximum = length - at;
    size_t matched = 0;
    while (matched < maximum && (atoms[0].c == '.' || text[at + matched] == atoms[0].c)) matched++;
    for (size_t i = minimum; i <= matched; i++) if (oracle(atoms + 1,n - 1,text,length,at+i)) return true;
    return false;
}
static void test_regex_differential(void) {
    nx_rng rng = nx_test_rng(341);
    for (size_t it = 0; it < nx_test_iters(2500); it++) {
        oracle_atom atoms[5]; char pattern[20] = "^", text[12]; size_t pn = 1;
        size_t count = (size_t)(nx_rng_u64(&rng) % 5) + 1, n = (size_t)(nx_rng_u64(&rng) % 10);
        for (size_t i = 0; i < count; i++) {
            atoms[i].c = "abc."[nx_rng_u64(&rng) % 4]; atoms[i].quantifier = " ?*+"[nx_rng_u64(&rng) % 4];
            pattern[pn++] = atoms[i].c;
            if (atoms[i].quantifier != ' ') pattern[pn++] = atoms[i].quantifier;
        }
        pattern[pn++] = '$'; pattern[pn] = 0;
        for (size_t i = 0; i < n; i++) text[i] = "abc"[nx_rng_u64(&rng) % 3];
        text[n] = 0;
        expect_match(pattern,text,0,oracle(atoms,count,text,n,0));
    }
}
static void test_regex_rejections_limits_mutation(void) {
    const char *bad[] = {"[", "[]", "[z-a]", "[\\d-a]", "(", ")", "a**", "a{3,2}", "a{999999}",
                         "\\", "\\1", "(?=a)", "a+?", "[a-\\w]", "\\xxy"};
    for (size_t i = 0; i < NX_ARRAY_LEN(bad); i++) {
        nx_regex *re = NULL;
        NX_CHECK(nx_regex_compile(nx_slice_cstr(bad[i]),0,&re,NULL) != NX_OK); NX_CHECK(re == NULL);
    }
    nx_regex *re = NULL;
    NX_CHECK_OK(nx_regex_compile(nx_slice_cstr("(a|aa)*b"),0,&re,NULL));
    bool matched = true;
    NX_CHECK_EQ_I(nx_regex_match(re,nx_slice_cstr("aaaaaaaaaaaaaaaaaaaaaaaaa"),5,&matched),NX_ERR_LIMIT);
    NX_CHECK(!matched); nx_regex_free(re);
    nx_rng rng = nx_test_rng(442);
    for (size_t i = 0; i < nx_test_iters(2000); i++) {
        char input[48] = "^(ab|cd)+[0-9]{2,4}.*$"; size_t n = strlen(input);
        if (i % 2) input[nx_rng_u64(&rng) % n] = (char)(nx_rng_u64(&rng) % 256);
        else n = (size_t)(nx_rng_u64(&rng) % n);
        nx_status st = nx_regex_compile(nx_slice_make(input,n),0,&re,NULL);
        if (st == NX_OK) {
            st = nx_regex_match(re,nx_slice_cstr("abcd1244xx"),100000,&matched);
            NX_CHECK(st == NX_OK || st == NX_ERR_LIMIT); nx_regex_free(re);
        } else NX_CHECK(st == NX_ERR_PARSE || st == NX_ERR_UNSUPPORTED || st == NX_ERR_LIMIT);
    }
}
static nx_status regex_oom(void *ctx) {
    (void)ctx; nx_regex *re = NULL;
    nx_status st = nx_regex_compile(nx_slice_cstr("(ab|c)+[0-9]{2,4}"),0,&re,NULL);
    bool matched;
    if (st == NX_OK) st = nx_regex_match(re,nx_slice_cstr("ccab124"),0,&matched);
    nx_regex_free(re); return st;
}
static void test_regex_oom(void) { nx_test_oom_sweep(regex_oom,NULL,20); }
int main(void) {
    NX_RUN(test_regex_language); NX_RUN(test_regex_differential);
    NX_RUN(test_regex_rejections_limits_mutation); NX_RUN(test_regex_oom);
    return nx_test_summary();
}
