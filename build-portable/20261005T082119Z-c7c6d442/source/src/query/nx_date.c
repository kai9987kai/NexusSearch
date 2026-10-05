#include "query/nx_date.h"
#include <string.h>
#include <limits.h>

static bool leap(int64_t y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
static unsigned month_days(int64_t y, unsigned m) {
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return m >= 1 && m <= 12 ? days[m - 1] + (unsigned)(m == 2 && leap(y)) : 0;
}
/* Gregorian 400-year eras, shifted to the Unix epoch. */
static int64_t civil_days(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yy = (unsigned)(y - era * 400);
    unsigned mm = m > 2 ? m - 3 : m + 9;
    unsigned day = (153 * mm + 2) / 5 + d - 1;
    return era * 146097 + (int64_t)(yy * 365 + yy / 4 - yy / 100 + day) - 719468;
}
static void from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yy = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = (int64_t)yy + era * 400;
    unsigned doy = doe - (365 * yy + yy / 4 - yy / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y += *m <= 2;
}
static int compare(nx_instant a, nx_instant b) {
    if (a.seconds != b.seconds) return a.seconds > b.seconds ? 1 : -1;
    return (a.nanos > b.nanos) - (a.nanos < b.nanos);
}
static nx_instant next_nano(nx_instant t) {
    if (++t.nanos == 1000000000u) { t.nanos = 0; ++t.seconds; }
    return t;
}
nx_status nx_date_resolve(const nx_datetime *v, int64_t now, nx_date_range *out) {
    if (!v || !out) return NX_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    int64_t y = v->year; unsigned m = v->month ? v->month : 1, d = v->day ? v->day : 1;
    if (v->base_rel != NX_REL_NONE) {
        if (now < -62135596800LL || now > 253402300799LL) return NX_ERR_LIMIT;
        if (v->base_rel != NX_REL_NOW && v->base_rel != NX_REL_TODAY && v->base_rel != NX_REL_YESTERDAY) return NX_ERR_TYPE;
        int64_t days = now / 86400;
        if (now % 86400 < 0) --days;
        int64_t value = v->base_rel == NX_REL_NOW ? now : days * 86400;
        if (v->base_rel == NX_REL_YESTERDAY) value -= 86400;
        int64_t amount = v->rel_amount;
        if (amount > 1000000000LL || amount < -1000000000LL) return NX_ERR_LIMIT;
        if (v->rel_unit == NX_UNIT_MONTH || v->rel_unit == NX_UNIT_YEAR) {
            int64_t base_day = value / 86400;
            if (value % 86400 < 0) --base_day;
            int64_t remainder = value - base_day * 86400;
            from_days(base_day, &y, &m, &d);
            int64_t total = y * 12 + (int64_t)m - 1 + amount * (v->rel_unit == NX_UNIT_YEAR ? 12 : 1);
            if (total < 12 || total > 9999 * 12 + 11) return NX_ERR_LIMIT;
            y = total / 12; m = (unsigned)(total % 12) + 1;
            if (d > month_days(y,m)) d = month_days(y,m);
            value = civil_days(y,m,d) * 86400 + remainder;
        } else {
            int64_t unit;
            switch (v->rel_unit) {
            case NX_UNIT_NONE: unit = 0; break;
            case NX_UNIT_SECOND: unit = 1; break;
            case NX_UNIT_MINUTE: unit = 60; break;
            case NX_UNIT_HOUR: unit = 3600; break;
            case NX_UNIT_DAY: unit = 86400; break;
            case NX_UNIT_WEEK: unit = 604800; break;
            default: return NX_ERR_TYPE;
            }
            value += amount * unit;
        }
        if (value < -62135596800LL || value > 253402300799LL) return NX_ERR_LIMIT;
        out->lo.seconds = value;
        out->end = out->lo;
        out->end.seconds += v->base_rel == NX_REL_NOW ? 1 : 86400;
        return NX_OK;
    }
    if (y < 1 || y > 9999 || m > 12 || !d || d > month_days(y,m) ||
        v->hour > 23 || v->minute > 59 || v->second > 59 || v->nanos >= 1000000000u ||
        v->tz_minutes < -1439 || v->tz_minutes > 1439) return NX_ERR_TYPE;
    out->lo.seconds = civil_days(y,m,d) * 86400 + v->hour * 3600 + v->minute * 60 + v->second;
    if (v->has_tz) out->lo.seconds -= v->tz_minutes * 60;
    out->lo.nanos = v->nanos;
    out->end = out->lo;
    switch (v->prec) {
    case NX_DT_PREC_YEAR: out->end.seconds += (leap(y) ? 366 : 365) * 86400; break;
    case NX_DT_PREC_MONTH: out->end.seconds += (int64_t)month_days(y,m) * 86400; break;
    case NX_DT_PREC_DAY: out->end.seconds += 86400; break;
    case NX_DT_PREC_MINUTE: out->end.seconds += 60; break;
    case NX_DT_PREC_SECOND: out->end.seconds += 1; break;
    case NX_DT_PREC_FRACTION: out->end = next_nano(out->lo); break;
    default: return NX_ERR_TYPE;
    }
    return NX_OK;
}
static bool digits(nx_slice s, size_t *at, unsigned count, unsigned *out) {
    *out = 0;
    if (*at > s.n || count > s.n - *at) return false;
    for (unsigned i=0;i<count;i++) { unsigned c=s.p[(*at)++]; if(c<'0'||c>'9') return false; *out=*out*10+c-'0'; }
    return true;
}
static bool take(nx_slice s,size_t *at,uint8_t ch) { if(*at<s.n && s.p[*at]==ch) {++*at;return true;}return false; }
nx_status nx_date_read(nx_slice s, nx_instant *out) {
    if (!out || (!s.p && s.n)) return NX_ERR_INVALID;
    memset(out,0,sizeof(*out));
    if (s.n < 10 || s.n > 40) return NX_ERR_TYPE;
    nx_datetime v = {0}; v.prec = NX_DT_PREC_DAY;
    size_t at=0; unsigned y,m,d,h=0,min=0,sec=0;
    if(!digits(s,&at,4,&y)||!take(s,&at,'-')||!digits(s,&at,2,&m)||!take(s,&at,'-')||!digits(s,&at,2,&d)) return NX_ERR_TYPE;
    v.year=(int32_t)y;v.month=(uint8_t)m;v.day=(uint8_t)d;
    if(at<s.n && (s.p[at]=='T'||s.p[at]=='t')) {
        ++at;
        if(!digits(s,&at,2,&h)||!take(s,&at,':')||!digits(s,&at,2,&min)) return NX_ERR_TYPE;
        v.prec=NX_DT_PREC_MINUTE;
        if(take(s,&at,':')) {
            if(!digits(s,&at,2,&sec)) return NX_ERR_TYPE;
            v.prec=NX_DT_PREC_SECOND;
            if(take(s,&at,'.')) {
                unsigned n=0;
                while(at<s.n && s.p[at]>='0' && s.p[at]<='9') {
                    if(n++==9) return NX_ERR_TYPE;
                    v.nanos=v.nanos*10+s.p[at++]-'0';
                }
                if(!n) return NX_ERR_TYPE;
                while(n++<9) v.nanos*=10;
                v.prec=NX_DT_PREC_FRACTION;
            }
        }
        if(take(s,&at,'Z')||take(s,&at,'z')) v.has_tz=true;
        else if(at<s.n && (s.p[at]=='+'||s.p[at]=='-')) {
            int sign=s.p[at++]=='+'?1:-1; unsigned zh,zm;
            if(!digits(s,&at,2,&zh)||!take(s,&at,':')||!digits(s,&at,2,&zm)||zh>23||zm>59) return NX_ERR_TYPE;
            v.has_tz=true;v.tz_minutes=(int16_t)(sign*(int)(zh*60+zm));
        }
    }
    if(at!=s.n||m<1||m>12||d<1||d>31||h>23||min>59||sec>59) return NX_ERR_TYPE;
    v.hour=(uint8_t)h;v.minute=(uint8_t)min;v.second=(uint8_t)sec;
    nx_date_range range;
    nx_status st=nx_date_resolve(&v,0,&range);
    if(st==NX_OK) *out=range.lo;
    return st;
}
bool nx_date_matches(nx_instant value,const nx_date_range *r,nx_op op) {
    if(!r) return false;
    int lo=compare(value,r->lo),hi=compare(value,r->end);
    switch(op) {
    case NX_OP_EQ:case NX_OP_MATCH:return lo>=0&&hi<0;
    case NX_OP_NE:return lo<0||hi>=0;
    case NX_OP_LT:return lo<0;
    case NX_OP_LE:return hi<0;
    case NX_OP_GT:return hi>=0;
    case NX_OP_GE:return lo>=0;
    default:return false;
    }
}
