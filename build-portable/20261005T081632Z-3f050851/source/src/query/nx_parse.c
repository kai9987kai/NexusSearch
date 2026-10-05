#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include "query/nx_parse.h"
#include <math.h>
#include <float.h>
#include <locale.h>

#ifdef NX_WINDOWS
typedef _locale_t numeric_locale;
#else
typedef locale_t numeric_locale;
#endif

typedef enum token_kind {
    T_END, T_WORD, T_STRING, T_REGEX, T_LP, T_RP, T_LB, T_RB, T_COLON,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_TILDE, T_CARET, T_COMMA, T_DOTS,
    T_MINUS, T_AND, T_OR, T_NOT, T_EXPLAIN, T_ANALYZE, T_WATCH, T_SOURCE,
    T_SORT, T_BY, T_ASC, T_DESC, T_LIMIT, T_OFFSET, T_FACET
} token_kind;

typedef struct token { token_kind kind; nx_span span; } token;
typedef struct parser {
    nx_cursor input;
    nx_arena *arena;
    nx_parse_limits lim;
    nx_error *error;
    nx_status status;
    token tok;
    uint32_t end, depth;
    size_t bytes;
    numeric_locale locale;   /* per-parse C-runtime locale; never changes global locale */
} parser;
typedef struct array { void *data; uint32_t n, cap; } array;
typedef struct expression { nx_node *node; bool simple; } expression;

static bool digit(uint8_t c) { return c >= '0' && c <= '9'; }
static bool alpha(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool space(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
static uint8_t peek(const parser *p, size_t ahead) {
    return ahead < nx_cursor_left(&p->input) ? p->input.p[p->input.pos + ahead] : 0;
}
static nx_span span_between(uint32_t off, uint32_t end) { nx_span s = {off, end - off}; return s; }
static void fail_at(parser *p, nx_status st, nx_span s, const char *msg) {
    if (p->status != NX_OK) return;
    p->status = st;
    nx_error_set(p->error, st, s.off, s.len, "%s", msg);
}
static void syntax(parser *p, const char *msg) { fail_at(p, NX_ERR_PARSE, p->tok.span, msg); }
static void *alloc_bytes(parser *p, size_t n, size_t align) {
    size_t total;
    if (p->status != NX_OK) return NULL;
    if (nx_add_overflow(p->bytes, n, &total) || total > p->lim.max_arena_bytes) {
        fail_at(p, NX_ERR_LIMIT, p->tok.span, "query arena budget exceeded");
        return NULL;
    }
    void *v = nx_arena_zalloc(p->arena, n, align);
    if (!v) fail_at(p, NX_ERR_NOMEM, p->tok.span, "out of memory parsing query");
    else p->bytes = total;
    return v;
}
#define NEW(p, type) ((type *)alloc_bytes((p), sizeof(type), _Alignof(type)))
static void *append(parser *p, array *a, size_t elem) {
    if (p->status != NX_OK) return NULL;
    if (a->n >= p->lim.max_list_items) {
        fail_at(p, NX_ERR_LIMIT, p->tok.span, "query list limit exceeded");
        return NULL;
    }
    if (a->n == a->cap) {
        uint32_t cap = a->cap ? a->cap : 4;
        if (a->cap) cap = a->cap > UINT32_MAX / 2 ? UINT32_MAX : a->cap * 2;
        if (cap > p->lim.max_list_items) cap = p->lim.max_list_items;
        size_t bytes;
        if (nx_mul_overflow(cap, elem, &bytes)) { syntax(p, "list size overflow"); return NULL; }
        void *data = alloc_bytes(p, bytes, _Alignof(max_align_t));
        if (!data) return NULL;
        if (a->n) memcpy(data, a->data, (size_t)a->n * elem);
        a->data = data; a->cap = cap;
    }
    void *item = (uint8_t *)a->data + (size_t)a->n * elem;
    a->n++;
    return item;
}
static nx_str copy_text(parser *p, const char *s, size_t n) {
    nx_str result = {NULL, 0};
    if (n > p->lim.max_string_bytes) {
        fail_at(p, NX_ERR_LIMIT, p->tok.span, "query string limit exceeded");
        return result;
    }
    size_t bytes;
    if (nx_add_overflow(n, 1, &bytes)) { syntax(p, "string size overflow"); return result; }
    char *v = (char *)alloc_bytes(p, bytes, 1);
    if (v) { memcpy(v, s, n); v[n] = 0; result.s = v; result.len = (uint32_t)n; }
    return result;
}
static const char *token_text(const parser *p, token t) { return (const char *)p->input.p + t.span.off; }
static bool token_is(const parser *p, token t, const char *word) {
    size_t n = strlen(word);
    return t.span.len == n && memcmp(token_text(p, t), word, n) == 0;
}
static bool date_start(const parser *p) {
    return digit(peek(p, 0)) && digit(peek(p, 1)) && digit(peek(p, 2)) && digit(peek(p, 3)) && peek(p, 4) == '-';
}
static bool word_stop(parser *p, bool date) {
    uint8_t c = peek(p, 0);
    if (!c || space(c)) return true;
    if (strchr("()[]\"'<>=!~^,", c)) return true;
    if (c == ':' && !date) return true;
    return (c == '.' && peek(p, 1) == '.') || (c == '&' && peek(p, 1) == '&') ||
           (c == '|' && peek(p, 1) == '|');
}
static void next(parser *p) {
    if (p->status != NX_OK) return;
    p->end = p->tok.span.off + p->tok.span.len;
    while (space(peek(p, 0))) (void)nx_rd_u8(&p->input);
    uint32_t start = (uint32_t)p->input.pos;
    p->tok.span = span_between(start, start);
    p->tok.kind = T_END;
    if (!nx_cursor_left(&p->input)) return;
    bool dt = date_start(p);
    uint8_t c = nx_rd_u8(&p->input);
    token_kind kind = T_WORD;
    switch (c) {
    case '(': kind = T_LP; break; case ')': kind = T_RP; break;
    case '[': kind = T_LB; break; case ']': kind = T_RB; break;
    case ':': kind = T_COLON; break; case '=': kind = T_EQ; break;
    case '~': kind = T_TILDE; break; case '^': kind = T_CARET; break;
    case ',': kind = T_COMMA; break; case '-': kind = T_MINUS; break;
    case '!': kind = peek(p, 0) == '=' ? T_NE : T_NOT; if (kind == T_NE) (void)nx_rd_u8(&p->input); break;
    case '<': kind = peek(p, 0) == '=' ? T_LE : T_LT; if (kind == T_LE) (void)nx_rd_u8(&p->input); break;
    case '>': kind = peek(p, 0) == '=' ? T_GE : T_GT; if (kind == T_GE) (void)nx_rd_u8(&p->input); break;
    case '&': if (peek(p, 0) == '&') { kind = T_AND; (void)nx_rd_u8(&p->input); } break;
    case '|': if (peek(p, 0) == '|') { kind = T_OR; (void)nx_rd_u8(&p->input); } break;
    case '.': if (peek(p, 0) == '.') { kind = T_DOTS; (void)nx_rd_u8(&p->input); } break;
    case '\'': case '"': case '/': {
        kind = c == '/' ? T_REGEX : T_STRING;
        bool closed = false;
        while (nx_cursor_left(&p->input)) {
            uint8_t v = nx_rd_u8(&p->input);
            if (v == c) { closed = true; break; }
            if (v == '\\' && nx_cursor_left(&p->input)) (void)nx_rd_u8(&p->input);
        }
        if (!closed) {
            p->tok.span = span_between(start, (uint32_t)p->input.pos);
            syntax(p, c == '/' ? "unterminated regex started here" : "unterminated string started here");
            return;
        }
        if (c == '/') while (alpha(peek(p, 0))) (void)nx_rd_u8(&p->input);
        break;
    }
    default: break;
    }
    if (kind == T_WORD) while (!word_stop(p, dt)) (void)nx_rd_u8(&p->input);
    p->tok.kind = kind;
    p->tok.span = span_between(start, (uint32_t)p->input.pos);
    if (kind == T_WORD) {
        static const struct { const char *s; token_kind k; } keywords[] = {
            {"AND", T_AND}, {"OR", T_OR}, {"NOT", T_NOT}, {"EXPLAIN", T_EXPLAIN},
            {"ANALYZE", T_ANALYZE}, {"WATCH", T_WATCH}, {"SOURCE", T_SOURCE},
            {"SORT", T_SORT}, {"BY", T_BY}, {"ASC", T_ASC}, {"DESC", T_DESC},
            {"LIMIT", T_LIMIT}, {"OFFSET", T_OFFSET}, {"FACET", T_FACET}
        };
        for (size_t i = 0; i < NX_ARRAY_LEN(keywords); i++) {
            if (token_is(p, p->tok, keywords[i].s)) { p->tok.kind = keywords[i].k; break; }
        }
    }
}
static bool take(parser *p, token_kind k) { if (p->tok.kind != k) return false; next(p); return true; }
static bool expect(parser *p, token_kind k, const char *msg) {
    if (p->status != NX_OK) return false;
    if (!take(p, k)) { syntax(p, msg); return false; }
    return p->status == NX_OK;
}
static bool enter(parser *p) {
    if (p->depth >= p->lim.max_depth) { fail_at(p, NX_ERR_LIMIT, p->tok.span, "query nesting limit exceeded"); return false; }
    p->depth++; return true;
}
static bool ident(parser *p, token t) {
    if (t.kind != T_WORD || !t.span.len) return false;
    const uint8_t *s = (const uint8_t *)token_text(p, t);
    if (!(alpha(s[0]) || s[0] == '_' || s[0] >= 128)) return false;
    for (uint32_t i = 1; i < t.span.len; i++) {
        if (!(alpha(s[i]) || digit(s[i]) || s[i] == '_' || s[i] == '.' || s[i] == '-' || s[i] >= 128)) return false;
    }
    return true;
}
static nx_name name(parser *p) {
    nx_name n = {0};
    if (!ident(p, p->tok)) { syntax(p, "expected an identifier"); return n; }
    n.span = p->tok.span;
    nx_str s = copy_text(p, token_text(p, p->tok), p->tok.span.len);
    n.s = s.s; n.len = s.len;
    next(p);
    return n;
}
static bool comparator(token_kind t) { return t >= T_EQ && t <= T_TILDE; }
static nx_op operator_read(parser *p, nx_span *span) {
    uint32_t start = p->tok.span.off;
    nx_op op = NX_OP_MATCH;
    bool colon = take(p, T_COLON);
    if (comparator(p->tok.kind)) {
        switch (p->tok.kind) {
        case T_EQ: op = NX_OP_EQ; break; case T_NE: op = NX_OP_NE; break;
        case T_LT: op = NX_OP_LT; break; case T_LE: op = NX_OP_LE; break;
        case T_GT: op = NX_OP_GT; break; case T_GE: op = NX_OP_GE; break;
        case T_TILDE: op = NX_OP_FUZZY; break; default: break;
        }
        next(p);
    } else if (!colon) syntax(p, "expected a field operator");
    *span = span_between(start, p->end);
    return op;
}

/* Conversion uses a per-parse C numeric locale for correctly rounded decimal
 * input. Locale objects are C-runtime-owned and released on every exit. */
static bool number_parse(parser *p, nx_value *v, nx_str raw) {
    size_t i = 0, n = raw.len;
    bool neg = false;
    if (i < n && (raw.s[i] == '+' || raw.s[i] == '-')) { neg = raw.s[i] == '-'; i++; }
    size_t begin = i;
    while (i < n && digit((uint8_t)raw.s[i])) i++;
    if (i == begin) return false;
    bool integer = true;
    if (i + 1 < n && raw.s[i] == '.' && digit((uint8_t)raw.s[i + 1])) {
        integer = false; i++;
        while (i < n && digit((uint8_t)raw.s[i])) i++;
    }
    size_t mant_end = i;
    if (i < n && (raw.s[i] == 'e' || raw.s[i] == 'E')) {
        size_t e = i + 1;
        if (e < n && (raw.s[e] == '+' || raw.s[e] == '-')) e++;
        if (e < n && digit((uint8_t)raw.s[e])) {
            integer = false;
            while (e < n && digit((uint8_t)raw.s[e])) {
                e++;
            }
            i = e;
        }
    }
    for (size_t k = i; k < n; k++) {
        uint8_t c = (uint8_t)raw.s[k];
        if (!(alpha(c) || c == '%' || c >= 128)) return false;
    }
    if (!p->locale) {
#ifdef NX_WINDOWS
        p->locale = _create_locale(LC_NUMERIC, "C");
#else
        p->locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
#endif
        if (!p->locale) { fail_at(p, NX_ERR_NOMEM, v->span, "cannot create numeric locale"); return true; }
    }
    char *end = NULL;
#ifdef NX_WINDOWS
    double d = _strtod_l(raw.s, &end, p->locale);
#else
    double d = strtod_l(raw.s, &end, p->locale);
#endif
    if ((size_t)(end - raw.s) != i) { syntax(p, "invalid numeric literal"); return true; }
    if (!(d >= -DBL_MAX && d <= DBL_MAX)) { syntax(p, "number is outside the finite double range"); return true; }
    v->kind = NX_VAL_NUMBER;
    v->u.number.raw = raw;
    v->u.number.d = d;
    v->u.number.suffix_off = (uint32_t)i;
    if (integer) {
        uint64_t value = 0, max = neg ? UINT64_C(9223372036854775808) : (uint64_t)INT64_MAX;
        bool fits = true;
        for (size_t k = begin; k < mant_end; k++) {
            uint32_t dig = (uint32_t)(raw.s[k] - '0');
            if (value > (max - dig) / 10) { fits = false; break; }
            value = value * 10 + dig;
        }
        if (fits) {
            v->u.number.has_int = true;
            v->u.number.i = neg ? (value == UINT64_C(9223372036854775808) ? INT64_MIN : -(int64_t)value) : (int64_t)value;
        }
    }
    return true;
}
static int decimal(const char *s, size_t n, size_t *pos, size_t count) {
    if (*pos > n || count > n - *pos) return -1;
    int result = 0;
    for (size_t j = 0; j < count; j++) {
        uint8_t c = (uint8_t)s[*pos + j];
        if (!digit(c)) return -1;
        result = result * 10 + c - '0';
    }
    *pos += count;
    return result;
}
static bool char_at(const char *s, size_t n, size_t *i, char c) {
    if (*i >= n || s[*i] != c) return false;
    (*i)++; return true;
}
static bool datetime_parse(parser *p, nx_value *v, nx_str raw) {
    nx_datetime dt = {0};
    const char *s = raw.s;
    size_t n = raw.len, i = 0;
    if (n >= 3 && memcmp(s, "now", 3) == 0 && (n == 3 || s[3] == '+' || s[3] == '-')) {
        dt.base_rel = NX_REL_NOW; i = 3;
    } else if (n >= 5 && memcmp(s, "today", 5) == 0 && (n == 5 || s[5] == '+' || s[5] == '-')) {
        dt.base_rel = NX_REL_TODAY; i = 5;
    } else if (n >= 9 && memcmp(s, "yesterday", 9) == 0 && (n == 9 || s[9] == '+' || s[9] == '-')) {
        dt.base_rel = NX_REL_YESTERDAY; i = 9;
    }
    if (dt.base_rel != NX_REL_NONE) {
        if (i < n) {
            bool negative = s[i++] == '-';
            size_t first = i;
            uint64_t amount = 0, max = negative ? UINT64_C(9223372036854775808) : (uint64_t)INT64_MAX;
            while (i < n && digit((uint8_t)s[i])) {
                uint32_t d = (uint32_t)(s[i++] - '0');
                if (amount > (max - d) / 10) goto invalid;
                amount = amount * 10 + d;
            }
            if (i == first) goto invalid;
            if (n - i == 2 && s[i] == 'm' && s[i + 1] == 'o') dt.rel_unit = NX_UNIT_MONTH;
            else if (n - i == 1) {
                switch (s[i]) {
                case 's': dt.rel_unit = NX_UNIT_SECOND; break; case 'm': dt.rel_unit = NX_UNIT_MINUTE; break;
                case 'h': dt.rel_unit = NX_UNIT_HOUR; break; case 'd': dt.rel_unit = NX_UNIT_DAY; break;
                case 'w': dt.rel_unit = NX_UNIT_WEEK; break; case 'y': dt.rel_unit = NX_UNIT_YEAR; break;
                default: goto invalid;
                }
            } else goto invalid;
            dt.rel_amount = negative ? (amount == UINT64_C(9223372036854775808) ? INT64_MIN : -(int64_t)amount) : (int64_t)amount;
        }
    } else {
        if (n == 4 && digit((uint8_t)s[0]) && digit((uint8_t)s[1]) && digit((uint8_t)s[2]) && digit((uint8_t)s[3])) {
            dt.year = decimal(s, n, &i, 4);
            if (dt.year < 1) goto invalid;
            dt.prec = NX_DT_PREC_YEAR;
            v->kind = NX_VAL_DATETIME; v->u.datetime.raw = raw; v->u.datetime.v = dt;
            return true;
        }
        if (n < 5 || s[4] != '-' || !digit((uint8_t)s[0]) || !digit((uint8_t)s[1]) ||
            !digit((uint8_t)s[2]) || !digit((uint8_t)s[3])) return false;
        int year = decimal(s, n, &i, 4);
        if (year < 1 || !char_at(s, n, &i, '-')) goto invalid;
        int month = decimal(s, n, &i, 2);
        if (month < 1 || month > 12) goto invalid;
        dt.year = year; dt.month = (uint8_t)month; dt.prec = NX_DT_PREC_MONTH;
        if (i < n) {
            if (!char_at(s, n, &i, '-')) goto invalid;
            int day = decimal(s, n, &i, 2);
            static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            int maxday = days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 1 : 0);
            if (day < 1 || day > maxday) goto invalid;
            dt.day = (uint8_t)day; dt.prec = NX_DT_PREC_DAY;
        }
        if (i < n) {
            if (!char_at(s, n, &i, 'T')) goto invalid;
            int hour = decimal(s, n, &i, 2);
            if (hour < 0 || hour > 23 || !char_at(s, n, &i, ':')) goto invalid;
            int minute = decimal(s, n, &i, 2);
            if (minute < 0 || minute > 59) goto invalid;
            dt.hour = (uint8_t)hour; dt.minute = (uint8_t)minute; dt.prec = NX_DT_PREC_MINUTE;
            if (char_at(s, n, &i, ':')) {
                int second = decimal(s, n, &i, 2);
                if (second < 0 || second > 59) goto invalid;
                dt.second = (uint8_t)second; dt.prec = NX_DT_PREC_SECOND;
                if (char_at(s, n, &i, '.')) {
                    uint32_t count = 0;
                    while (i < n && digit((uint8_t)s[i])) {
                        if (count == 9) goto invalid;
                        dt.nanos = dt.nanos * 10 + (uint32_t)(s[i++] - '0'); count++;
                    }
                    if (!count) goto invalid;
                    while (count++ < 9) dt.nanos *= 10;
                    dt.prec = NX_DT_PREC_FRACTION;
                }
            }
            if (char_at(s, n, &i, 'Z')) dt.has_tz = true;
            else if (i < n && (s[i] == '+' || s[i] == '-')) {
                bool negative = s[i++] == '-';
                int th = decimal(s, n, &i, 2);
                if (th < 0 || th > 23 || !char_at(s, n, &i, ':')) goto invalid;
                int tm = decimal(s, n, &i, 2);
                if (tm < 0 || tm > 59) goto invalid;
                dt.has_tz = true; dt.tz_minutes = (int16_t)((negative ? -1 : 1) * (th * 60 + tm));
            }
        }
        if (i != n) goto invalid;
    }
    v->kind = NX_VAL_DATETIME; v->u.datetime.raw = raw; v->u.datetime.v = dt;
    return true;
invalid:
    fail_at(p, NX_ERR_PARSE, v->span, "invalid calendar date or relative date/time");
    return true;
}
static int hex(uint8_t c) {
    if (digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool unicode4(const char *s, size_t n, size_t *i, uint32_t *out) {
    if (*i > n || n - *i < 4) return false;
    uint32_t v = 0;
    for (size_t j = 0; j < 4; j++) {
        int h = hex((uint8_t)s[(*i)++]);
        if (h < 0) return false;
        v = v * 16 + (uint32_t)h;
    }
    *out = v; return true;
}
static nx_str decode_string(parser *p, token t, bool regex, uint32_t *flags) {
    nx_str result = {0};
    const char *s = token_text(p, t);
    size_t n = t.span.len, end = n - 1;
    if (regex) {
        while (end > 0 && s[end] != '/') end--;
        for (size_t j = end + 1; j < n; j++) {
            uint32_t bit = s[j] == 'i' ? NX_REGEX_ICASE : s[j] == 's' ? NX_REGEX_DOTALL : s[j] == 'm' ? NX_REGEX_MULTI : 0;
            if (!bit || (*flags & bit)) { fail_at(p, NX_ERR_PARSE, t.span, "unknown or repeated regex flag"); return result; }
            *flags |= bit;
        }
    }
    size_t cap = NX_MIN(end, (size_t)p->lim.max_string_bytes), used = 0;
    char *text = (char *)alloc_bytes(p, cap + 1, 1);
    if (!text) return result;
    for (size_t i = 1; i < end;) {
        uint32_t cp = (uint8_t)s[i++];
        uint8_t encoded[4];
        size_t count = 1;
        encoded[0] = (uint8_t)cp;
        if (cp == '\\') {
            if (i >= end) goto invalid;
            char c = s[i++];
            if (regex) {
                if (c == '/') encoded[0] = '/';
                else { encoded[0] = '\\'; encoded[1] = (uint8_t)c; count = 2; }
            } else {
                switch (c) {
                case '\\': case '\'': case '"': encoded[0] = (uint8_t)c; break;
                case 'n': encoded[0] = '\n'; break; case 't': encoded[0] = '\t'; break;
                case 'u':
                    if (!unicode4(s, end, &i, &cp) || cp == 0) goto invalid;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t low;
                        if (i + 2 > end || s[i] != '\\' || s[i + 1] != 'u') goto invalid;
                        i += 2;
                        if (!unicode4(s, end, &i, &low) || low < 0xDC00 || low > 0xDFFF) goto invalid;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) goto invalid;
                    if (cp < 0x80) encoded[0] = (uint8_t)cp;
                    else if (cp < 0x800) { encoded[0] = (uint8_t)(0xC0 | (cp >> 6)); encoded[1] = (uint8_t)(0x80 | (cp & 63)); count = 2; }
                    else if (cp < 0x10000) { encoded[0] = (uint8_t)(0xE0 | (cp >> 12)); encoded[1] = (uint8_t)(0x80 | ((cp >> 6) & 63)); encoded[2] = (uint8_t)(0x80 | (cp & 63)); count = 3; }
                    else { encoded[0] = (uint8_t)(0xF0 | (cp >> 18)); encoded[1] = (uint8_t)(0x80 | ((cp >> 12) & 63)); encoded[2] = (uint8_t)(0x80 | ((cp >> 6) & 63)); encoded[3] = (uint8_t)(0x80 | (cp & 63)); count = 4; }
                    break;
                default: goto invalid;
                }
            }
        }
        if (count > cap - used) { fail_at(p, NX_ERR_LIMIT, t.span, "query string limit exceeded"); return result; }
        memcpy(text + used, encoded, count); used += count;
    }
    text[used] = 0; result.s = text; result.len = (uint32_t)used;
    return result;
invalid:
    fail_at(p, NX_ERR_PARSE, t.span, "invalid string escape (NUL and unpaired surrogates are forbidden)");
    return result;
}

static nx_value *value_parse(parser *p);
static nx_value *numeric_value(parser *p) {
    token t = p->tok;
    if (t.kind == T_MINUS) {
        next(p);
        if (p->tok.kind != T_WORD || t.span.off + t.span.len != p->tok.span.off) { syntax(p, "expected a number after '-'"); return NULL; }
        t.span.len += p->tok.span.len;
    } else if (t.kind != T_WORD) { syntax(p, "expected a number"); return NULL; }
    nx_value *v = NEW(p, nx_value);
    if (!v) return NULL;
    v->span = t.span;
    nx_str raw = copy_text(p, token_text(p, t), t.span.len);
    if (!raw.s) return NULL;
    if (!number_parse(p, v, raw)) { syntax(p, "expected a number"); return NULL; }
    next(p);
    return p->status == NX_OK ? v : NULL;
}
static nx_value *scalar_parse(parser *p) {
    if (p->status != NX_OK) return NULL;
    token t = p->tok;
    if (t.kind == T_MINUS) return numeric_value(p);
    nx_value *v = NEW(p, nx_value);
    if (!v) return NULL;
    v->span = t.span;
    if (t.kind == T_STRING || t.kind == T_REGEX) {
        if (t.kind == T_STRING) { v->kind = NX_VAL_STRING; v->u.text = decode_string(p, t, false, NULL); }
        else { v->kind = NX_VAL_REGEX; v->u.regex.pattern = decode_string(p, t, true, &v->u.regex.flags); }
        next(p);
    } else if (t.kind == T_LB) {
        if (!enter(p)) return NULL;
        next(p);
        array floats = {0};
        while (p->status == NX_OK && p->tok.kind != T_RB) {
            nx_value *number = numeric_value(p);
            if (!number) break;
            if (number->u.number.suffix_off != number->u.number.raw.len || !isfinite((float)number->u.number.d)) {
                fail_at(p, NX_ERR_PARSE, number->span, "vector entries must be finite float numbers without units"); break;
            }
            float *slot = (float *)append(p, &floats, sizeof(float));
            if (slot) *slot = (float)number->u.number.d;
            if (!take(p, T_COMMA)) break;
            if (p->tok.kind == T_RB) { syntax(p, "expected a vector entry after ','"); break; }
        }
        (void)expect(p, T_RB, "expected ']' after vector");
        p->depth--;
        v->kind = NX_VAL_VECTOR; v->u.vec.v = (float *)floats.data; v->u.vec.n = floats.n;
        v->span = span_between(t.span.off, p->end);
    } else if (t.kind == T_WORD) {
        nx_str raw = copy_text(p, token_text(p, t), t.span.len);
        if (!raw.s) return NULL;
        next(p);
        if (p->tok.kind == T_LP && ident(p, t)) {
            if (!enter(p)) return NULL;
            next(p);
            array args = {0};
            while (p->status == NX_OK && p->tok.kind != T_RP) {
                nx_value *arg = value_parse(p);
                if (!arg) break;
                nx_value **slot = (nx_value **)append(p, &args, sizeof(nx_value *));
                if (slot) *slot = arg;
                if (!take(p, T_COMMA)) break;
                if (p->tok.kind == T_RP) { syntax(p, "expected an argument after ','"); break; }
            }
            (void)expect(p, T_RP, "expected ')' after call arguments");
            p->depth--;
            v->kind = NX_VAL_CALL; v->u.call.name = raw; v->u.call.args = (nx_value **)args.data; v->u.call.nargs = args.n;
            v->span = span_between(t.span.off, p->end);
        } else if (raw.len == 1 && raw.s[0] == '*') v->kind = NX_VAL_STAR;
        else if (!datetime_parse(p, v, raw) && !number_parse(p, v, raw)) { v->kind = NX_VAL_WORD; v->u.text = raw; }
    } else if (t.kind == T_LP) {
        /* Value-only groups in call arguments/params have any-of semantics. */
        if (!enter(p)) return NULL;
        next(p);
        array items = {0};
        while (p->status == NX_OK && p->tok.kind != T_RP) {
            nx_value *item = value_parse(p);
            if (!item) break;
            nx_value **slot = (nx_value **)append(p, &items, sizeof(nx_value *));
            if (slot) *slot = item;
            if (!take(p, T_COMMA) && !take(p, T_OR)) break;
            if (p->tok.kind == T_RP) { syntax(p, "expected a value after list separator"); break; }
        }
        if (!items.n) syntax(p, "empty value group");
        (void)expect(p, T_RP, "expected ')' after value group");
        p->depth--;
        if (items.n == 1) return ((nx_value **)items.data)[0];
        v->kind = NX_VAL_ANYOF; v->u.anyof.items = (nx_value **)items.data; v->u.anyof.n = items.n;
        v->span = span_between(t.span.off, p->end);
    } else { syntax(p, "expected a value"); return NULL; }
    return p->status == NX_OK ? v : NULL;
}
static nx_value *value_parse(parser *p) {
    nx_value *v = scalar_parse(p);
    if (!v || !take(p, T_DOTS)) return v;
    nx_value *hi = scalar_parse(p);
    if (!hi) return NULL;
    if (v->kind >= NX_VAL_RANGE && v->kind != NX_VAL_STAR) { syntax(p, "range endpoint must be a scalar"); return NULL; }
    if (hi->kind >= NX_VAL_RANGE && hi->kind != NX_VAL_STAR) { syntax(p, "range endpoint must be a scalar"); return NULL; }
    nx_value *range = NEW(p, nx_value);
    if (!range) return NULL;
    range->kind = NX_VAL_RANGE; range->span = span_between(v->span.off, hi->span.off + hi->span.len);
    range->u.range.lo_open = v->kind == NX_VAL_STAR; range->u.range.hi_open = hi->kind == NX_VAL_STAR;
    range->u.range.lo = range->u.range.lo_open ? NULL : v;
    range->u.range.hi = range->u.range.hi_open ? NULL : hi;
    return range;
}

static nx_node *clause_node(parser *p, nx_clause clause, nx_span span) {
    nx_node *n = NEW(p, nx_node);
    if (n) { n->kind = NX_NODE_CLAUSE; n->u.clause = clause; n->span = span; }
    return n;
}
static void add_child(parser *p, array *kids, nx_node *n, nx_node_kind kind) {
    if (!n) return;
    if (n->kind == kind) {
        for (uint32_t i = 0; i < n->u.list.n && p->status == NX_OK; i++) {
            nx_node **slot = (nx_node **)append(p, kids, sizeof(nx_node *));
            if (slot) *slot = n->u.list.kids[i];
        }
    } else {
        nx_node **slot = (nx_node **)append(p, kids, sizeof(nx_node *));
        if (slot) *slot = n;
    }
}
static nx_node *list_node(parser *p, array kids, nx_node_kind kind, nx_span span) {
    if (p->status != NX_OK || !kids.n) return NULL;
    if (kids.n == 1 && kind != NX_NODE_NOT) return ((nx_node **)kids.data)[0];
    nx_node *n = NEW(p, nx_node);
    if (n) { n->kind = kind; n->span = span; n->u.list.kids = (nx_node **)kids.data; n->u.list.n = kids.n; }
    return n;
}
static void boost_node(parser *p, nx_node *n, double factor) {
    if (!n || p->status != NX_OK) return;
    if (n->kind == NX_NODE_CLAUSE) {
        n->u.clause.boost *= factor;
        if (!(n->u.clause.boost >= -DBL_MAX && n->u.clause.boost <= DBL_MAX))
            fail_at(p, NX_ERR_PARSE, n->span, "effective boost is not finite");
    } else for (uint32_t i = 0; i < n->u.list.n; i++) boost_node(p, n->u.list.kids[i], factor);
}
static bool boost_read(parser *p, nx_node *n) {
    if (!take(p, T_CARET)) return false;
    nx_value *v = numeric_value(p);
    if (v) {
        if (v->u.number.suffix_off != v->u.number.raw.len) syntax(p, "boost must be a number without units");
        else boost_node(p, n, v->u.number.d);
    }
    return true;
}
static bool starts_unary(token_kind t, bool bound) {
    return t == T_WORD || t == T_STRING || t == T_REGEX || t == T_LP || t == T_LB || t == T_NOT || t == T_MINUS ||
           (bound && (comparator(t) || t == T_COLON));
}
static expression or_expr(parser *p, const nx_clause *bound);
static bool params_ahead(parser *p) {
    parser probe = *p;
    probe.error = NULL;
    next(&probe);
    if (probe.tok.kind != T_LP) return false;
    next(&probe);
    if (!ident(&probe, probe.tok)) return false;
    next(&probe);
    return probe.tok.kind == T_EQ;
}
static void params_parse(parser *p, nx_clause *clause) {
    array params = {0};
    if (!enter(p)) return;
    next(p);
    do {
        nx_param param = {0};
        uint32_t start = p->tok.span.off;
        param.name = name(p);
        if (!expect(p, T_EQ, "expected '=' after parameter name")) break;
        param.value = value_parse(p);
        if (!param.value) break;
        param.span = span_between(start, p->end);
        nx_param *slot = (nx_param *)append(p, &params, sizeof(nx_param));
        if (slot) *slot = param;
    } while (take(p, T_COMMA) && p->status == NX_OK);
    (void)expect(p, T_RP, "expected ')' after parameters");
    p->depth--;
    clause->params = (nx_param *)params.data; clause->nparams = params.n;
}
static expression term(parser *p, const nx_clause *bound) {
    expression r = {NULL, true};
    uint32_t start = p->tok.span.off;
    nx_clause clause = {0};
    clause.boost = 1;
    bool field = false;
    if (bound) {
        clause = *bound;
        if (comparator(p->tok.kind) || p->tok.kind == T_COLON) {
            clause.op = operator_read(p, &clause.op_span); r.simple = false;
        }
    } else if (ident(p, p->tok)) {
        parser probe = *p;
        probe.error = NULL;
        next(&probe);
        bool params = probe.tok.kind == T_LP && params_ahead(p);
        if (probe.tok.kind == T_COLON || comparator(probe.tok.kind) || params) {
            field = true;
            clause.field = name(p);
            if (params) params_parse(p, &clause);
            clause.op = operator_read(p, &clause.op_span);
        }
    }
    if (p->status != NX_OK) return r;
    if (field && p->tok.kind == T_LP) {
        if (!enter(p)) return r;
        nx_span opening = p->tok.span;
        next(p);
        r = or_expr(p, &clause);
        if (p->status == NX_OK && p->tok.kind != T_RP) fail_at(p, NX_ERR_PARSE, opening, "unbalanced '(' in field group");
        (void)expect(p, T_RP, "expected ')' after field group");
        p->depth--;
        if (!r.node || p->status != NX_OK) return r;
        if (r.simple && r.node->kind == NX_NODE_OR) {
            nx_value *v = NEW(p, nx_value);
            if (!v) return r;
            uint32_t n = r.node->u.list.n;
            size_t bytes;
            if (nx_mul_overflow(n, sizeof(nx_value *), &bytes)) { syntax(p, "list size overflow"); return r; }
            v->kind = NX_VAL_ANYOF; v->span = span_between(opening.off, p->end);
            v->u.anyof.items = (nx_value **)alloc_bytes(p, bytes, _Alignof(nx_value *));
            if (!v->u.anyof.items) return r;
            v->u.anyof.n = n;
            for (uint32_t i = 0; i < n; i++) v->u.anyof.items[i] = r.node->u.list.kids[i]->u.clause.value;
            clause.value = v;
            r.node = clause_node(p, clause, span_between(start, p->end));
        }
        (void)boost_read(p, r.node);
        if (r.node && r.node->kind == NX_NODE_CLAUSE) r.node->span = span_between(start, p->end);
        r.simple = false;
        return r;
    }
    clause.value = value_parse(p);
    if (!clause.value) return r;
    r.node = clause_node(p, clause, span_between(start, p->end));
    if (boost_read(p, r.node)) r.simple = false;
    if (r.node) r.node->span = span_between(start, p->end);
    return r;
}
static expression unary(parser *p, const nx_clause *bound) {
    expression r = {NULL, false};
    if (p->status != NX_OK) return r;
    token t = p->tok;
    if (t.kind == T_NOT || t.kind == T_MINUS) {
        if (t.kind == T_MINUS && (p->input.pos >= p->input.n || space(peek(p, 0)))) {
            syntax(p, "prefix '-' must immediately precede a term"); return r;
        }
        if (!enter(p)) return r;
        next(p);
        expression child = unary(p, bound);
        p->depth--;
        if (!child.node) return r;
        array kids = {0};
        nx_node **slot = (nx_node **)append(p, &kids, sizeof(nx_node *));
        if (slot) *slot = child.node;
        r.node = list_node(p, kids, NX_NODE_NOT, span_between(t.span.off, p->end));
        return r;
    }
    if (t.kind == T_LP) {
        if (!enter(p)) return r;
        next(p);
        r = or_expr(p, bound);
        if (p->status == NX_OK && p->tok.kind != T_RP) fail_at(p, NX_ERR_PARSE, t.span, "unbalanced '('");
        (void)expect(p, T_RP, "expected ')' after expression");
        p->depth--;
        if (bound && boost_read(p, r.node)) r.simple = false;
        return r;
    }
    return term(p, bound);
}
static expression and_expr(parser *p, const nx_clause *bound) {
    uint32_t start = p->tok.span.off;
    expression result = unary(p, bound);
    if (!result.node) return result;
    array kids = {0};
    while (p->status == NX_OK && (p->tok.kind == T_AND || starts_unary(p->tok.kind, bound != NULL))) {
        if (!kids.n) add_child(p, &kids, result.node, NX_NODE_AND);
        (void)take(p, T_AND);
        expression right = unary(p, bound);
        add_child(p, &kids, right.node, NX_NODE_AND);
        result.simple = false;
    }
    if (kids.n) result.node = list_node(p, kids, NX_NODE_AND, span_between(start, p->end));
    return result;
}
static expression or_expr(parser *p, const nx_clause *bound) {
    uint32_t start = p->tok.span.off;
    expression result = and_expr(p, bound);
    if (!result.node) return result;
    array kids = {0};
    while (p->status == NX_OK && (p->tok.kind == T_OR || (bound && p->tok.kind == T_COMMA))) {
        if (!kids.n) add_child(p, &kids, result.node, NX_NODE_OR);
        next(p);
        expression right = and_expr(p, bound);
        add_child(p, &kids, right.node, NX_NODE_OR);
        result.simple = result.simple && right.simple;
    }
    if (kids.n) result.node = list_node(p, kids, NX_NODE_OR, span_between(start, p->end));
    return result;
}
static uint32_t count_clauses(parser *p, const nx_node *n) {
    if (!n || p->status != NX_OK) return 0;
    if (n->kind == NX_NODE_CLAUSE) return 1;
    uint32_t count = 0;
    for (uint32_t i = 0; i < n->u.list.n && p->status == NX_OK; i++) {
        uint32_t add = count_clauses(p, n->u.list.kids[i]);
        if (add > p->lim.max_clauses - count) { fail_at(p, NX_ERR_LIMIT, n->span, "query clause limit exceeded"); return 0; }
        count += add;
    }
    return count;
}
static void integer_clause(parser *p, bool *has, int64_t *value, nx_span *span) {
    if (*has) { syntax(p, "duplicate LIMIT or OFFSET clause"); return; }
    next(p);
    nx_value *v = numeric_value(p);
    if (!v) return;
    if (!v->u.number.has_int || v->u.number.i < 0 || v->u.number.suffix_off != v->u.number.raw.len) {
        fail_at(p, NX_ERR_PARSE, v->span, "LIMIT and OFFSET require a non-negative int64 integer"); return;
    }
    *has = true; *value = v->u.number.i; *span = v->span;
}
nx_parse_limits nx_parse_limits_default(void) {
    nx_parse_limits lim = {64u * 1024u, 64, 4096, 4096, 16u * 1024u, 16u * 1024u * 1024u};
    return lim;
}
nx_status nx_query_parse(nx_arena *arena, const char *text, size_t len,
                         const nx_parse_limits *limits, nx_stmt **out, nx_error *error) {
    nx_error_clear(error);
    if (out) *out = NULL;
    nx_parse_limits lim = limits ? *limits : nx_parse_limits_default();
    if (!arena || !out || (!text && len) || !lim.max_depth || lim.max_depth > 256 ||
        !lim.max_clauses || !lim.max_list_items || !lim.max_string_bytes || !lim.max_arena_bytes ||
        lim.max_query_bytes > UINT32_MAX) {
        nx_error_set(error, NX_ERR_INVALID, -1, 0, "invalid parser arguments or limits (maximum supported depth is 256)");
        return NX_ERR_INVALID;
    }
    if (len > lim.max_query_bytes) {
        nx_error_set(error, NX_ERR_LIMIT, 0, 0, "query length limit exceeded"); return NX_ERR_LIMIT;
    }
    for (size_t i = 0; i < len; i++) if (!text[i]) {
        nx_error_set(error, NX_ERR_PARSE, (int64_t)i, 1, "NUL byte in query"); return NX_ERR_PARSE;
    }
    parser p = {0};
    p.input = nx_cursor_make(nx_slice_make(text ? text : "", len));
    p.arena = arena; p.lim = lim; p.error = error;
    nx_arena_mark mark = nx_arena_save(arena);
    next(&p);
    uint32_t first = p.tok.span.off;
    nx_stmt *s = NEW(&p, nx_stmt);
    if (!s) goto done;
    if (take(&p, T_EXPLAIN)) s->mode = take(&p, T_ANALYZE) ? NX_STMT_EXPLAIN_ANALYZE : NX_STMT_EXPLAIN;
    else if (take(&p, T_WATCH)) s->mode = NX_STMT_WATCH;
    if (take(&p, T_SOURCE)) {
        array sources = {0};
        if (expect(&p, T_LP, "expected '(' after SOURCE")) {
            do {
                nx_name v = name(&p);
                nx_name *slot = (nx_name *)append(&p, &sources, sizeof(nx_name));
                if (slot) *slot = v;
            } while (take(&p, T_COMMA) && p.status == NX_OK);
            (void)expect(&p, T_RP, "expected ')' after sources");
        }
        s->sources = (nx_name *)sources.data; s->nsources = sources.n;
    }
    if (starts_unary(p.tok.kind, false) && p.status == NX_OK) s->where = or_expr(&p, NULL).node;
    array sort = {0}, facets = {0};
    while (p.status == NX_OK && p.tok.kind != T_END) {
        if (take(&p, T_SORT)) {
            if (!expect(&p, T_BY, "expected BY after SORT")) break;
            do {
                nx_sort_key key = {0};
                key.field = name(&p);
                if (take(&p, T_ASC)) key.dir = NX_SORT_ASC;
                else if (take(&p, T_DESC)) key.dir = NX_SORT_DESC;
                nx_sort_key *slot = (nx_sort_key *)append(&p, &sort, sizeof(nx_sort_key));
                if (slot) *slot = key;
            } while (take(&p, T_COMMA) && p.status == NX_OK);
        } else if (p.tok.kind == T_LIMIT) integer_clause(&p, &s->has_limit, &s->limit, &s->limit_span);
        else if (p.tok.kind == T_OFFSET) integer_clause(&p, &s->has_offset, &s->offset, &s->offset_span);
        else if (take(&p, T_FACET)) {
            do {
                nx_name v = name(&p);
                nx_name *slot = (nx_name *)append(&p, &facets, sizeof(nx_name));
                if (slot) *slot = v;
            } while (take(&p, T_COMMA) && p.status == NX_OK);
        } else syntax(&p, "unexpected token after expression; expected SORT, LIMIT, OFFSET or FACET");
    }
    s->sort = (nx_sort_key *)sort.data; s->nsort = sort.n;
    s->facets = (nx_name *)facets.data; s->nfacets = facets.n;
    s->span = span_between(first, p.end >= first ? p.end : first);
    s->nclauses = count_clauses(&p, s->where);
    if (s->nclauses > lim.max_clauses) fail_at(&p, NX_ERR_LIMIT, s->span, "query clause limit exceeded");
done:
    if (p.locale) {
#ifdef NX_WINDOWS
        _free_locale(p.locale);
#else
        freelocale(p.locale);
#endif
    }
    if (p.status != NX_OK) nx_arena_restore(arena, mark);
    else *out = s;
    return p.status;
}
