#include "core/nx_json.h"
#include "core/nx_utf8.h"

typedef struct json_context {
    nx_arena *arena;
    nx_slice input;
    nx_json_limits limits;
    size_t pos, nodes, strings, work;
    nx_status status;
    nx_error *error;
    nx_buf *output;
} json_context;

nx_json_limits nx_json_default_limits(void) {
    nx_json_limits l = {16 * 1024 * 1024, 100000, 64, 16 * 1024 * 1024, 64 * 1024 * 1024};
    return l;
}
static bool problem(json_context *p, nx_status s, const char *message) {
    if (p->status == NX_OK) {
        p->status = s;
        nx_error_set(p->error, s, (int64_t)p->pos, 1, "%s", message);
    }
    return false;
}
static bool work(json_context *p, size_t n) {
    if (p->status != NX_OK) return false;
    if (n > p->limits.max_work - p->work) return problem(p, NX_ERR_LIMIT, "JSON work limit exceeded");
    p->work += n; return true;
}
static bool string_bytes(json_context *p, size_t n) {
    if (n > p->limits.max_string_bytes - p->strings) return problem(p, NX_ERR_LIMIT, "JSON string limit exceeded");
    p->strings += n; return true;
}
static void *allocate(json_context *p, size_t n, size_t alignment) {
    void *v = nx_arena_zalloc(p->arena, n, alignment);
    if (!v) problem(p, NX_ERR_NOMEM, "JSON allocation failed");
    return v;
}
static nx_slice copy(json_context *p, nx_slice s) {
    size_t n;
    if (nx_add_overflow(s.n, 1, &n)) { problem(p, NX_ERR_LIMIT, "JSON string too large"); return nx_slice_make(NULL, 0); }
    uint8_t *bytes = (uint8_t *)allocate(p, n, 1);
    if (bytes && s.n) memcpy(bytes, s.p, s.n);
    return nx_slice_make(bytes, bytes ? s.n : 0);
}
static bool ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool digit(uint8_t c) { return c >= '0' && c <= '9'; }
static void whitespace(json_context *p) {
    while (p->pos < p->input.n && ws(p->input.p[p->pos]) && work(p, 1)) p->pos++;
}
static bool take(json_context *p, uint8_t c) {
    if (p->pos < p->input.n && p->input.p[p->pos] == c && work(p, 1)) { p->pos++; return true; }
    return false;
}
static int hex(uint8_t c) {
    if (digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool unicode4(json_context *p, uint32_t *cp) {
    if (p->input.n - p->pos < 4) return problem(p, NX_ERR_PARSE, "truncated Unicode escape");
    *cp = 0;
    for (size_t i = 0; i < 4; i++) {
        int x = hex(p->input.p[p->pos++]);
        if (x < 0) return problem(p, NX_ERR_PARSE, "invalid Unicode escape");
        *cp = *cp * 16 + (uint32_t)x;
    }
    return work(p, 4);
}
static bool parse_string(json_context *p, nx_slice *out) {
    *out = nx_slice_make(NULL, 0);
    if (!take(p, '"')) return problem(p, NX_ERR_PARSE, "expected a quoted string");
    nx_buf bytes; nx_buf_init(&bytes); bool closed = false;
    while (p->pos < p->input.n && p->status == NX_OK) {
        if (take(p, '"')) { closed = true; break; }
        size_t start = p->pos; uint32_t cp;
        if (take(p, '\\')) {
            if (p->pos == p->input.n) { problem(p, NX_ERR_PARSE, "trailing string escape"); break; }
            uint8_t esc = p->input.p[p->pos++];
            if (!work(p, 1)) break;
            switch (esc) {
                case '"': case '\\': case '/': cp = esc; break;
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                case 'n': cp = '\n'; break;
                case 'r': cp = '\r'; break;
                case 't': cp = '\t'; break;
                case 'u': {
                    if (!unicode4(p, &cp)) goto done;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        uint32_t low;
                        if (!take(p, '\\') || !take(p, 'u') || !unicode4(p, &low) || low < 0xdc00 || low > 0xdfff) {
                            problem(p, NX_ERR_PARSE, "unpaired high surrogate"); goto done;
                        }
                        cp = 0x10000 + (cp - 0xd800) * 1024 + low - 0xdc00;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) { problem(p, NX_ERR_PARSE, "unpaired low surrogate"); goto done; }
                    break;
                }
                default: problem(p, NX_ERR_PARSE, "unknown string escape"); goto done;
            }
        } else {
            nx_status st = nx_utf8_decode(p->input, &p->pos, &cp);
            if (st != NX_OK || cp < 0x20) { problem(p, NX_ERR_PARSE, "invalid UTF-8 or unescaped control in string"); break; }
            if (!work(p, p->pos - start)) break;
        }
        uint8_t encoded[4]; size_t n;
        (void)nx_utf8_encode(cp, encoded, &n);
        if (!string_bytes(p, n)) break;
        nx_buf_put(&bytes, encoded, n);
        if (bytes.oom) problem(p, NX_ERR_NOMEM, "JSON string allocation failed");
    }
    if (!closed && p->status == NX_OK) problem(p, NX_ERR_PARSE, "unterminated string");
    if (p->status == NX_OK) *out = copy(p, nx_buf_slice(&bytes));
done:
    nx_buf_free(&bytes); return p->status == NX_OK;
}
static void number_exact(nx_json_number *n) {
    bool negative = n->raw.n && n->raw.p[0] == '-';
    uint64_t magnitude = 0, limit = negative ? UINT64_C(9223372036854775808) : INT64_MAX;
    n->exact_i64 = false;
    for (size_t i = negative ? 1u : 0u; i < n->raw.n; i++) {
        uint8_t c = n->raw.p[i];
        if (!digit(c) || magnitude > (limit - (uint32_t)(c - '0')) / 10) return;
        magnitude = magnitude * 10 + (uint32_t)(c - '0');
    }
    n->exact_i64 = true;
    n->i64 = negative ? (magnitude == UINT64_C(9223372036854775808) ? INT64_MIN : -(int64_t)magnitude) : (int64_t)magnitude;
}
static bool parse_number(json_context *p, nx_json_number *number) {
    size_t start = p->pos;
    if (p->pos < p->input.n && p->input.p[p->pos] == '-') p->pos++;
    if (p->pos == p->input.n || !digit(p->input.p[p->pos])) return problem(p, NX_ERR_PARSE, "expected number digits");
    if (p->input.p[p->pos] == '0') p->pos++;
    else while (p->pos < p->input.n && digit(p->input.p[p->pos])) p->pos++;
    if (p->pos < p->input.n && p->input.p[p->pos] == '.') {
        size_t at = ++p->pos;
        while (p->pos < p->input.n && digit(p->input.p[p->pos])) p->pos++;
        if (p->pos == at) return problem(p, NX_ERR_PARSE, "expected fraction digits");
    }
    if (p->pos < p->input.n && (p->input.p[p->pos] == 'e' || p->input.p[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->input.n && (p->input.p[p->pos] == '+' || p->input.p[p->pos] == '-')) p->pos++;
        size_t at = p->pos;
        while (p->pos < p->input.n && digit(p->input.p[p->pos])) p->pos++;
        if (p->pos == at) return problem(p, NX_ERR_PARSE, "expected exponent digits");
    }
    if (!work(p, p->pos - start)) return false;
    number->raw = copy(p, nx_slice_sub(p->input, start, p->pos - start));
    if (p->status == NX_OK) number_exact(number);
    return p->status == NX_OK;
}
static nx_json *parse_value(json_context *p, size_t depth) {
    whitespace(p);
    if (p->status != NX_OK) return NULL;
    if (depth > p->limits.max_depth || depth > 256 || p->nodes == p->limits.max_nodes) {
        problem(p, NX_ERR_LIMIT, "JSON depth or node limit exceeded"); return NULL;
    }
    if (p->pos == p->input.n) { problem(p, NX_ERR_PARSE, "expected JSON value"); return NULL; }
    if (!work(p, 1)) return NULL;
    p->nodes++;
    nx_json *v = (nx_json *)allocate(p, sizeof *v, _Alignof(nx_json));
    if (!v) return NULL;
    v->offset = p->pos; uint8_t c = p->input.p[p->pos];
    if (c == '[' || c == '{') {
        bool object = c == '{'; uint8_t end = object ? '}' : ']';
        v->kind = object ? NX_JSON_OBJECT : NX_JSON_ARRAY;
        p->pos++; whitespace(p);
        nx_json **link = &v->child;
        if (!take(p, end)) {
            do {
                nx_slice key = {0}; size_t ko = 0, kn = 0;
                whitespace(p);
                if (object) {
                    ko = p->pos;
                    if (!parse_string(p, &key)) break;
                    kn = p->pos - ko; whitespace(p);
                    if (!take(p, ':')) { problem(p, NX_ERR_PARSE, "expected ':' after key"); break; }
                    for (nx_json *prev = v->child; prev; prev = prev->next) {
                        if (!work(p, prev->key.n == key.n ? key.n + 1 : 1)) break;
                        if (nx_slice_eq(prev->key, key)) { problem(p, NX_ERR_PARSE, "duplicate object key"); break; }
                    }
                    if (p->status != NX_OK) break;
                }
                nx_json *child = parse_value(p, depth + 1);
                if (!child) break;
                child->key = key; child->key_offset = ko; child->key_length = kn;
                *link = child; link = &child->next; v->count++;
                whitespace(p);
                if (take(p, end)) break;
                if (!take(p, ',')) { problem(p, NX_ERR_PARSE, "expected comma or closing delimiter"); break; }
            } while (p->status == NX_OK);
        }
    } else if (c == '"') {
        v->kind = NX_JSON_STRING; (void)parse_string(p, &v->as.string);
    } else if (c == '-' || digit(c)) {
        v->kind = NX_JSON_NUMBER; (void)parse_number(p, &v->as.number);
    } else {
        const char *word = c == 'n' ? "null" : c == 't' ? "true" : c == 'f' ? "false" : NULL;
        size_t n = word ? strlen(word) : 0;
        if (!word || n > p->input.n - p->pos || memcmp(p->input.p + p->pos, word, n))
            problem(p, NX_ERR_PARSE, "unknown JSON value");
        else if (work(p, n)) {
            p->pos += n;
            v->kind = c == 'n' ? NX_JSON_NULL : NX_JSON_BOOL; v->as.boolean = c == 't';
        }
    }
    v->length = p->pos - v->offset;
    return p->status == NX_OK ? v : NULL;
}
nx_status nx_json_parse(nx_arena *arena, nx_slice input, const nx_json_limits *limits, nx_json **out, nx_error *error) {
    if (out) *out = NULL;
    nx_error_clear(error);
    if (!arena || !out || (!input.p && input.n)) return NX_ERR_INVALID;
    json_context p = {0}; p.arena = arena; p.input = input; p.error = error;
    p.limits = limits ? *limits : nx_json_default_limits();
    if (input.n > p.limits.max_input_bytes || input.n > INT64_MAX) {
        problem(&p, NX_ERR_LIMIT, "JSON input limit exceeded"); return p.status;
    }
    nx_arena_mark mark = nx_arena_save(arena);
    nx_json *root = parse_value(&p, 1); whitespace(&p);
    if (p.status == NX_OK && p.pos != input.n) problem(&p, NX_ERR_PARSE, "trailing data after JSON value");
    if (p.status == NX_OK) *out = root; else nx_arena_restore(arena, mark);
    return p.status;
}
const nx_json *nx_json_get(const nx_json *object, nx_slice key) {
    if (!object || object->kind != NX_JSON_OBJECT || (!key.p && key.n)) return NULL;
    for (const nx_json *v = object->child; v; v = v->next) if (nx_slice_eq(v->key, key)) return v;
    return NULL;
}
const nx_json *nx_json_get_cstr(const nx_json *object, const char *key) { return key ? nx_json_get(object, nx_slice_cstr(key)) : NULL; }
const nx_json *nx_json_at(const nx_json *array, size_t index) {
    if (!array || array->kind != NX_JSON_ARRAY || index >= array->count) return NULL;
    const nx_json *v = array->child;
    while (index-- && v) v = v->next;
    return v;
}
nx_status nx_json_get_i64(const nx_json *value, int64_t *out) {
    if (!out) return NX_ERR_INVALID;
    if (!value || value->kind != NX_JSON_NUMBER || !value->as.number.exact_i64) return NX_ERR_TYPE;
    *out = value->as.number.i64; return NX_OK;
}
static bool emit(json_context *p, const void *data, size_t n) {
    if (!work(p, n)) return false;
    if (n > p->limits.max_input_bytes - p->output->len) return problem(p, NX_ERR_LIMIT, "serialized JSON size limit exceeded");
    nx_buf_put(p->output, data, n);
    return !p->output->oom || problem(p, NX_ERR_NOMEM, "JSON output allocation failed");
}
static bool emit_string(json_context *p, nx_slice text) {
    if (!string_bytes(p, text.n) || !work(p, text.n)) return false;
    if (nx_utf8_validate(text, NULL) != NX_OK) return problem(p, NX_ERR_CORRUPT, "invalid string in JSON tree");
    if (!emit(p, "\"", 1)) return false;
    const char hexes[] = "0123456789abcdef";
    for (size_t i = 0; i < text.n; i++) {
        uint8_t c = text.p[i]; const char *escape = NULL;
        if (c == '"') escape = "\\\"";
        else if (c == '\\') escape = "\\\\";
        else if (c == '\b') escape = "\\b";
        else if (c == '\f') escape = "\\f";
        else if (c == '\n') escape = "\\n";
        else if (c == '\r') escape = "\\r";
        else if (c == '\t') escape = "\\t";
        if (escape) { if (!emit(p, escape, 2)) return false; }
        else if (c < 0x20) {
            char u[] = {'\\','u','0','0',hexes[c >> 4],hexes[c & 15]};
            if (!emit(p, u, sizeof u)) return false;
        } else if (!emit(p, &c, 1)) return false;
    }
    return emit(p, "\"", 1);
}
static bool serialize_value(json_context *p, const nx_json *v, size_t depth) {
    if (!v) return problem(p, NX_ERR_CORRUPT, "missing JSON node");
    if (depth > p->limits.max_depth || depth > 256 || p->nodes == p->limits.max_nodes)
        return problem(p, NX_ERR_LIMIT, "JSON serialization depth or node limit exceeded");
    if (!work(p, 1)) return false;
    p->nodes++;
    switch (v->kind) {
        case NX_JSON_NULL: return emit(p, "null", 4);
        case NX_JSON_BOOL: return emit(p, v->as.boolean ? "true" : "false", v->as.boolean ? 4 : 5);
        case NX_JSON_STRING: return emit_string(p, v->as.string);
        case NX_JSON_NUMBER: return emit(p, v->as.number.raw.p, v->as.number.raw.n);
        case NX_JSON_ARRAY: case NX_JSON_OBJECT: {
            bool object = v->kind == NX_JSON_OBJECT; size_t count = 0;
            if (!emit(p, object ? "{" : "[", 1)) return false;
            for (const nx_json *child = v->child; child; child = child->next) {
                if (count++ && !emit(p, ",", 1)) return false;
                if (object && (!emit_string(p, child->key) || !emit(p, ":", 1))) return false;
                if (!serialize_value(p, child, depth + 1)) return false;
            }
            if (count != v->count) return problem(p, NX_ERR_CORRUPT, "JSON child count mismatch");
            return emit(p, object ? "}" : "]", 1);
        }
        default: return problem(p, NX_ERR_CORRUPT, "unknown JSON node kind");
    }
}
nx_status nx_json_serialize(const nx_json *value, const nx_json_limits *limits, nx_buf *output, nx_error *error) {
    nx_error_clear(error);
    if (!value || !output) return NX_ERR_INVALID;
    nx_buf bytes; nx_buf_init(&bytes);
    json_context p = {0}; p.limits = limits ? *limits : nx_json_default_limits(); p.error = error; p.output = &bytes;
    if (serialize_value(&p, value, 1)) {
        if (!nx_buf_reserve(output, bytes.len)) problem(&p, NX_ERR_NOMEM, "JSON output allocation failed");
        else nx_buf_put(output, bytes.data, bytes.len);
    }
    nx_buf_free(&bytes); return p.status;
}
