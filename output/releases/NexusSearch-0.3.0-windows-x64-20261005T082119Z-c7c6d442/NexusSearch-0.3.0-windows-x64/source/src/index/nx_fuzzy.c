#include "index/nx_fuzzy.h"
#include "core/nx_utf8.h"
#include "core/nx_mem.h"

static nx_status scalars(nx_slice s, uint32_t **out, size_t *n) {
    *out = NULL; *n = 0;
    nx_status st = nx_utf8_validate(s,NULL);
    if (st != NX_OK || !s.n) return st;
    uint32_t *v = NX_NEW_ARRAY(uint32_t,s.n);
    if (!v) return NX_ERR_NOMEM;
    size_t pos = 0;
    while (pos < s.n) { (void)nx_utf8_decode(s,&pos,&v[*n]); (*n)++; }
    *out = v; return NX_OK;
}
nx_status nx_fuzzy_distance(nx_slice a, nx_slice b, uint32_t maximum, uint32_t *out) {
    if (!out) return NX_ERR_INVALID;
    *out = 0;
    if ((!a.p && a.n) || (!b.p && b.n)) return NX_ERR_INVALID;
    if (maximum > 64 || a.n > 16384 || b.n > 16384) return NX_ERR_LIMIT;
    uint32_t *x = NULL, *y = NULL, *rows = NULL; size_t n = 0, m = 0;
    nx_status st = scalars(a,&x,&n);
    if (st != NX_OK) goto done;
    st = scalars(b,&y,&m);
    if (st != NX_OK) goto done;
    if ((n > m ? n-m : m-n) > maximum) { *out = maximum + 1; goto done; }
    rows = NX_NEW_ARRAY(uint32_t,2*(m+1));
    if (!rows) { st = NX_ERR_NOMEM; goto done; }
    uint32_t *prev = rows, *curr = rows + m + 1, inf = maximum + 1;
    for (size_t j = 0; j <= m; j++) prev[j] = j <= maximum ? (uint32_t)j : inf;
    for (size_t i = 1; i <= n; i++) {
        size_t first = i > maximum ? i-maximum : 1, last = i+maximum < m ? i+maximum : m;
        curr[0] = i <= maximum ? (uint32_t)i : inf;
        if (first > 1) curr[first-1] = inf;
        uint32_t best = curr[0];
        for (size_t j = first; j <= last; j++) {
            uint32_t v = prev[j-1] + (x[i-1] != y[j-1]);
            if (prev[j]+1 < v) v = prev[j]+1;
            if (curr[j-1]+1 < v) v = curr[j-1]+1;
            curr[j] = v > inf ? inf : v;
            if (curr[j] < best) best = curr[j];
        }
        if (last < m) curr[last+1] = inf;
        uint32_t *swap = prev; prev = curr; curr = swap;
        if (best > maximum) { *out = inf; goto done; }
    }
    *out = prev[m] > maximum ? inf : prev[m];
done:
    nx_free(x); nx_free(y); nx_free(rows); return st;
}
