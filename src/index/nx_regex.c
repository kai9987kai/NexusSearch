#include "nx_regex.h"
#include "core/nx_mem.h"

enum { RE_EMPTY, RE_BYTE, RE_BOL, RE_EOL, RE_CAT, RE_ALT, RE_REPEAT, RE_SPLIT, RE_MATCH };
typedef struct re_node { int op, left, right; uint32_t min, max; uint8_t bits[32]; } re_node;
typedef struct re_state { int op; uint32_t out, alt; uint8_t bits[32]; } re_state;
struct nx_regex { re_state *states; uint32_t count, start, flags; };
typedef struct re_parser {
    nx_slice input;
    size_t pos;
    uint32_t flags, count, depth;
    re_node *nodes;
    nx_status status;
    nx_error *error;
} re_parser;

static int re_error(re_parser *p, nx_status status, const char *message) {
    if (p->status == NX_OK) {
        p->status = status;
        nx_error_set(p->error, status, (int64_t)p->pos, 1, "%s", message);
    }
    return -1;
}
static int node(re_parser *p, int op, int left, int right) {
    if (p->status != NX_OK) return -1;
    if (p->count == NX_RE_MAX_STATES) return re_error(p, NX_ERR_LIMIT, "regex node limit exceeded");
    int id = (int)p->count++;
    p->nodes[id].op = op; p->nodes[id].left = left; p->nodes[id].right = right;
    return id;
}
static void bit_add(uint8_t *bits, uint32_t c, uint32_t flags) {
    bits[c / 8] |= (uint8_t)(1u << (c % 8));
    if ((flags & NX_RE_ICASE) && c >= 'A' && c <= 'Z') bit_add(bits, c + 32, 0);
    if ((flags & NX_RE_ICASE) && c >= 'a' && c <= 'z') bit_add(bits, c - 32, 0);
}
static int hex(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Returns a literal byte or -2 for a character-set escape. */
static int class_item(re_parser *p, uint8_t *bits) {
    if (p->pos == p->input.n) return re_error(p, NX_ERR_PARSE, "missing character");
    uint32_t c = p->input.p[p->pos++];
    if (c == '\\') {
        if (p->pos == p->input.n) return re_error(p, NX_ERR_PARSE, "trailing escape");
        c = p->input.p[p->pos++];
        if (c == 'd' || c == 'D' || c == 's' || c == 'S' || c == 'w' || c == 'W') {
            bool inverse = c < 'a'; uint32_t kind = inverse ? c + 32 : c;
            for (uint32_t i = 0; i < 256; i++) {
                bool digit = i >= '0' && i <= '9';
                bool word = digit || (i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || i == '_';
                bool space = i == ' ' || (i >= '\t' && i <= '\r');
                bool yes = kind == 'd' ? digit : kind == 'w' ? word : space;
                if (yes != inverse) bit_add(bits, i, p->flags);
            }
            return -2;
        }
        if (c == 'n') c = '\n';
        else if (c == 'r') c = '\r';
        else if (c == 't') c = '\t';
        else if (c == 'x') {
            if (p->input.n - p->pos < 2 || hex(p->input.p[p->pos]) < 0 || hex(p->input.p[p->pos + 1]) < 0)
                return re_error(p, NX_ERR_PARSE, "expected two hex digits after \\x");
            c = (uint32_t)(hex(p->input.p[p->pos]) * 16 + hex(p->input.p[p->pos + 1])); p->pos += 2;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            return re_error(p, NX_ERR_UNSUPPORTED, "unsupported regex escape or backreference");
        }
    }
    bit_add(bits, c, p->flags);
    return (int)c;
}
static int expression(re_parser *p);
static int atom(re_parser *p) {
    if (++p->depth > 128) { p->depth--; return re_error(p, NX_ERR_LIMIT, "regex nesting limit exceeded"); }
    int id = -1;
    uint8_t c = p->input.p[p->pos];
    if (c == '(') {
        p->pos++;
        if (p->pos < p->input.n && p->input.p[p->pos] == '?') {
            if (p->input.n - p->pos >= 2 && p->input.p[p->pos + 1] == ':') p->pos += 2;
            else { re_error(p, NX_ERR_UNSUPPORTED, "lookaround and inline flags are unsupported"); goto done; }
        }
        id = expression(p);
        if (p->pos == p->input.n || p->input.p[p->pos] != ')') re_error(p, NX_ERR_PARSE, "missing closing parenthesis");
        else p->pos++;
    } else if (c == '^' || c == '$') {
        p->pos++; id = node(p, c == '^' ? RE_BOL : RE_EOL, -1, -1);
    } else if (c == '*' || c == '+' || c == '?' || c == '{' || c == '}' || c == ']') {
        re_error(p, NX_ERR_PARSE, "unexpected metacharacter");
    } else {
        id = node(p, RE_BYTE, -1, -1);
        if (id < 0) goto done;
        uint8_t *bits = p->nodes[id].bits;
        if (c == '.') {
            memset(bits, 255, 32); p->pos++;
            if (!(p->flags & NX_RE_DOTALL)) bits['\n' / 8] &= (uint8_t)~(1u << ('\n' % 8));
        } else if (c == '[') {
            p->pos++;
            bool invert = p->pos < p->input.n && p->input.p[p->pos] == '^';
            if (invert) p->pos++;
            bool any = false;
            while (p->pos < p->input.n && p->input.p[p->pos] != ']' && p->status == NX_OK) {
                int start = class_item(p, bits); any = true;
                if (p->input.n - p->pos >= 2 && p->input.p[p->pos] == '-' && p->input.p[p->pos + 1] != ']') {
                    p->pos++;
                    uint8_t end_bits[32] = {0}; int end = class_item(p, end_bits);
                    if (start < 0 || end < start) { re_error(p, NX_ERR_PARSE, "invalid character range"); break; }
                    for (uint32_t i = (uint32_t)start; i <= (uint32_t)end; i++) bit_add(bits, i, p->flags);
                }
            }
            if (!any || p->pos == p->input.n) re_error(p, NX_ERR_PARSE, "empty or unterminated character class");
            else p->pos++;
            if (invert) for (size_t i = 0; i < 32; i++) bits[i] ^= 255;
        } else (void)class_item(p, bits);
    }
done:
    p->depth--;
    return id;
}
static uint32_t repeat_number(re_parser *p) {
    size_t start = p->pos; uint32_t n = 0;
    while (p->pos < p->input.n && p->input.p[p->pos] >= '0' && p->input.p[p->pos] <= '9') {
        n = n * 10 + (uint32_t)(p->input.p[p->pos++] - '0');
        if (n > 1024) { re_error(p, NX_ERR_LIMIT, "regex repetition limit exceeded"); return 0; }
    }
    if (p->pos == start) re_error(p, NX_ERR_PARSE, "expected repeat count");
    return n;
}
static int repetition(re_parser *p) {
    int child = atom(p);
    if (p->status != NX_OK || p->pos == p->input.n) return child;
    uint8_t c = p->input.p[p->pos];
    if (c != '*' && c != '+' && c != '?' && c != '{') return child;
    p->pos++;
    uint32_t min = c == '+' ? 1u : 0u, max = c == '?' ? 1u : UINT32_MAX;
    if (c == '{') {
        min = repeat_number(p); max = min;
        if (p->pos < p->input.n && p->input.p[p->pos] == ',') {
            p->pos++;
            max = p->pos < p->input.n && p->input.p[p->pos] == '}' ? UINT32_MAX : repeat_number(p);
        }
        if (p->pos == p->input.n || p->input.p[p->pos] != '}' || min > max)
            return re_error(p, NX_ERR_PARSE, "invalid repetition");
        p->pos++;
    }
    int id = node(p, RE_REPEAT, child, -1);
    if (id >= 0) { p->nodes[id].min = min; p->nodes[id].max = max; }
    if (p->pos < p->input.n && p->input.p[p->pos] != 0 && strchr("*+?{", p->input.p[p->pos]))
        return re_error(p, NX_ERR_PARSE, "stacked or lazy quantifiers unsupported");
    return id;
}
static int concatenation(re_parser *p) {
    int result = -1;
    while (p->pos < p->input.n && p->input.p[p->pos] != ')' && p->input.p[p->pos] != '|' && p->status == NX_OK) {
        int next = repetition(p);
        result = result < 0 ? next : node(p, RE_CAT, result, next);
    }
    return result < 0 ? node(p, RE_EMPTY, -1, -1) : result;
}
static int expression(re_parser *p) {
    int left = concatenation(p);
    while (p->status == NX_OK && p->pos < p->input.n && p->input.p[p->pos] == '|') {
        p->pos++;
        int right = concatenation(p);
        left = node(p, RE_ALT, left, right);
    }
    return left;
}
static uint32_t state_new(re_parser *p, nx_regex *re, int op, uint32_t out, uint32_t alt, const uint8_t *bits) {
    if (p->status != NX_OK) return 0;
    if (re->count == NX_RE_MAX_STATES) { re_error(p, NX_ERR_LIMIT, "regex state limit exceeded"); return 0; }
    uint32_t id = re->count++;
    re_state *s = &re->states[id]; s->op = op; s->out = out; s->alt = alt;
    if (bits) memcpy(s->bits, bits, 32);
    return id;
}
static uint32_t compile_node(re_parser *p, nx_regex *re, int id, uint32_t next, uint32_t depth) {
    if (p->status != NX_OK || id < 0) return 0;
    if (depth > 256) { re_error(p, NX_ERR_LIMIT, "regex expression tree depth exceeded"); return 0; }
    re_node *n = &p->nodes[id];
    switch (n->op) {
        case RE_EMPTY: return next;
        case RE_BYTE: case RE_BOL: case RE_EOL: return state_new(p, re, n->op, next, 0, n->bits);
        case RE_CAT: {
            uint32_t right = compile_node(p, re, n->right, next, depth + 1);
            return compile_node(p, re, n->left, right, depth + 1);
        }
        case RE_ALT: {
            uint32_t left = compile_node(p, re, n->left, next, depth + 1);
            uint32_t right = compile_node(p, re, n->right, next, depth + 1);
            return state_new(p, re, RE_SPLIT, left, right, NULL);
        }
        case RE_REPEAT: {
            if (n->max == UINT32_MAX) {
                uint32_t split = state_new(p, re, RE_SPLIT, 0, next, NULL);
                uint32_t child = compile_node(p, re, n->left, split, depth + 1);
                re->states[split].out = child;
                next = split;
            } else {
                for (uint32_t i = n->min; i < n->max && p->status == NX_OK; i++) {
                    uint32_t child = compile_node(p, re, n->left, next, depth + 1);
                    next = state_new(p, re, RE_SPLIT, child, next, NULL);
                }
            }
            for (uint32_t i = 0; i < n->min && p->status == NX_OK; i++) next = compile_node(p, re, n->left, next, depth + 1);
            return next;
        }
        default: re_error(p, NX_ERR_INTERNAL, "unknown regex node"); return 0;
    }
}

nx_status nx_regex_compile(nx_slice input, uint32_t flags, nx_regex **out, nx_error *error) {
    if (!out) return NX_ERR_INVALID;
    *out = NULL; nx_error_clear(error);
    if ((!input.p && input.n) || (flags & ~7u)) return NX_ERR_INVALID;
    if (input.n > NX_RE_MAX_PATTERN) return NX_ERR_LIMIT;
    re_parser p = {0}; p.input = input; p.flags = flags; p.error = error;
    p.nodes = NX_NEW_ARRAY(re_node, NX_RE_MAX_STATES);
    nx_regex *re = NX_NEW(nx_regex);
    if (re) re->states = NX_NEW_ARRAY(re_state, NX_RE_MAX_STATES);
    if (!p.nodes || !re || !re->states) { nx_free(p.nodes); nx_regex_free(re); return NX_ERR_NOMEM; }
    int root = expression(&p);
    if (p.status == NX_OK && p.pos != input.n) re_error(&p, NX_ERR_PARSE, "unexpected closing parenthesis");
    re->flags = flags;
    (void)state_new(&p, re, RE_MATCH, 0, 0, NULL);
    re->start = compile_node(&p, re, root, 0, 0);
    nx_free(p.nodes);
    if (p.status != NX_OK) nx_regex_free(re); else *out = re;
    return p.status;
}
void nx_regex_free(nx_regex *re) { if (re) { nx_free(re->states); nx_free(re); } }

typedef struct re_work {
    const nx_regex *re;
    nx_slice text;
    uint32_t *list, *seen, *stack, count, generation;
    uint64_t remaining;
    bool limited;
} re_work;
static void push(re_work *w, uint32_t id, uint32_t *n) {
    if (w->seen[id] == w->generation || w->limited) return;
    if (!w->remaining) { w->limited = true; return; }
    w->remaining--; w->seen[id] = w->generation;
    w->stack[(*n)++] = id;
}
static void closure(re_work *w, uint32_t start, size_t pos) {
    uint32_t n = 0; push(w, start, &n);
    while (n && !w->limited) {
        uint32_t id = w->stack[--n];
        const re_state *s = &w->re->states[id];
        if (s->op == RE_SPLIT) { push(w, s->out, &n); push(w, s->alt, &n); }
        else if (s->op == RE_BOL) {
            if (pos == 0 || ((w->re->flags & NX_RE_MULTILINE) && w->text.p[pos - 1] == '\n')) push(w, s->out, &n);
        } else if (s->op == RE_EOL) {
            if (pos == w->text.n || ((w->re->flags & NX_RE_MULTILINE) && w->text.p[pos] == '\n')) push(w, s->out, &n);
        } else w->list[w->count++] = id;
    }
}
nx_status nx_regex_match(const nx_regex *re, nx_slice text, uint64_t max_work, bool *matched) {
    if (!matched) return NX_ERR_INVALID;
    *matched = false;
    if (!re || (!text.p && text.n)) return NX_ERR_INVALID;
    uint32_t *memory = NX_NEW_ARRAY(uint32_t, (size_t)re->count * 4);
    if (!memory) return NX_ERR_NOMEM;
    uint32_t *current = memory, *next = memory + re->count;
    re_work w = {re, text, current, memory + 2 * re->count, memory + 3 * re->count,
                 0, 1, max_work ? max_work : NX_RE_DEFAULT_WORK, false};
    closure(&w, re->start, 0);
    for (size_t pos = 0; !w.limited; pos++) {
        closure(&w, re->start, pos);
        uint32_t count = w.count;
        for (uint32_t i = 0; i < count; i++) if (re->states[current[i]].op == RE_MATCH) { *matched = true; goto done; }
        if (pos == text.n) break;
        uint8_t c = text.p[pos];
        if (++w.generation == 0) { memset(w.seen, 0, (size_t)re->count * sizeof(uint32_t)); w.generation = 1; }
        w.list = next; w.count = 0;
        for (uint32_t i = 0; i < count && !w.limited; i++) {
            if (!w.remaining) { w.limited = true; break; }
            w.remaining--;
            const re_state *s = &re->states[current[i]];
            if (s->bits[c / 8] & (1u << (c % 8))) closure(&w, s->out, pos + 1);
        }
        uint32_t *swap = current; current = next; next = swap;
    }
done:
    nx_free(memory);
    return w.limited ? NX_ERR_LIMIT : NX_OK;
}
