#!/usr/bin/env python3
"""gen_unicode.py - generate src/text/nx_unicode_data.c for NexusSearch.

    python tools/gen_unicode.py                 # (re)write src/text/nx_unicode_data.c
    python tools/gen_unicode.py --check         # verify the committed file is up to date
    python tools/gen_unicode.py --stats         # print table sizes only
    python tools/gen_unicode.py --golden        # write tests/data/unicode_golden.txt (test oracle)
    python tools/gen_unicode.py --compare-regex # diff the derived Word_Break/script classes against
                                                # the third-party 'regex' module (informational)

Only the Python standard library (unicodedata) is needed for the real output; the data comes from
Python 3.12's unicodedata (Unicode 15.0.0).  The generated file is committed; it records the Unicode
version it was built from.  Word_Break, script and emoji classes are NOT available in unicodedata, so
they are derived here from general category plus explicit range lists (see the "classification" section)
- they are "Word_Break-like" classes sufficient for the simplified UAX#29 segmentation in nx_analyze.c.

Layout (all tables are two-stage: stage1[dom(cp) >> shift] -> block id, stage2[block << shift | low bits]):
  props   record index -> packed uint32 (general category, Word_Break class, script class, flags)
  norm    record index -> packed uint32 (canonical combining class, composition-second rank, decomposition tag)
  fold    record index -> uint32 case-fold record (delta or index into a small pool of full folds)
  decomp  record index -> single-step decomposition (offset/len into a UTF-16 pool + tag)
  comp    sorted (first<<7 | second_rank) keys -> composite
  strip   small sorted table of "stroke letters" folded by NX_FOLD_DIACRITICS (o-slash, l-stroke, ...)
The domain of the two-stage tables is [0, DOM_A) U [0xE0000, 0xE1000); everything else is unassigned
(or private use for planes 15/16) and is handled in C.
"""
import os
import sys
import unicodedata as ud

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_C = os.path.join(ROOT, "src", "text", "nx_unicode_data.c")
OUT_GOLDEN = os.path.join(ROOT, "tests", "data", "unicode_golden.txt")

FORMAT_VERSION = 1
SURR = range(0xD800, 0xE000)

# ---------------------------------------------------------------------------------------------
# enumerations shared with nx_unicode.h (checked by _Static_assert in the generated file)
# ---------------------------------------------------------------------------------------------
GC = ["Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps", "Pe", "Pi",
      "Pf", "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn"]
GCI = {n: i for i, n in enumerate(GC)}

WB = ["OTHER", "ALETTER", "NUMERIC", "MIDLETTER", "MIDNUM", "MIDNUMLET", "KATAKANA", "EXTNUMLET",
      "EXTEND", "FORMAT", "ZWJ", "HAN", "HIRAGANA", "EMOJI", "RI"]
WBI = {n: i for i, n in enumerate(WB)}

SCRIPT = ["NONE", "HAN", "HIRAGANA", "KATAKANA", "HANGUL", "SA"]     # SA = Thai/Lao/Khmer/Myanmar (no spaces)
SCI = {n: i for i, n in enumerate(SCRIPT)}

# props flag bits (above gc:5 | wb:4 | script:3)
F_DIACRITIC = 1 << 12     # mark removed by NX_FOLD_DIACRITICS
F_IGNORABLE = 1 << 13     # Default_Ignorable_Code_Point (removed by NX_FOLD_IGNORABLE)
F_SPACE = 1 << 14         # White_Space
F_EXTPICT = 1 << 15       # pictographic symbol (emoji-like) - also wb == EMOJI
PROPS_SHIFT_WB = 5
PROPS_SHIFT_SCRIPT = 9

# norm record: ccc:8 | second_rank:7 << 8 | dtag:2 << 15
RANK_HANGUL = 127
DTAG = {"none": 0, "canon": 1, "width": 2, "compat": 3}


def in_ranges(cp, ranges):
    for lo, hi in ranges:
        if lo <= cp <= hi:
            return True
    return False


# ---------------------------------------------------------------------------------------------
# classification (derived; see module docstring)
# ---------------------------------------------------------------------------------------------
HAN_RANGES = [(0x2E80, 0x2E99), (0x2E9B, 0x2EF3), (0x2F00, 0x2FD5), (0x3005, 0x3005), (0x3007, 0x3007),
              (0x3021, 0x3029), (0x3038, 0x303B), (0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFA6D),
              (0xFA70, 0xFAD9), (0x16FE2, 0x16FE3), (0x16FF0, 0x16FF1), (0x20000, 0x2A6DF), (0x2A700, 0x2B739),
              (0x2B740, 0x2B81D), (0x2B820, 0x2CEA1), (0x2CEB0, 0x2EBE0), (0x2F800, 0x2FA1D), (0x30000, 0x3134A),
              (0x31350, 0x323AF)]
# Han script characters that are ideographs (letters/numbers); radicals (So) are symbols, not word characters.
HAN_WORD_CATS = {"Lo", "Nl", "Lm"}
HIRAGANA_RANGES = [(0x3041, 0x3096), (0x309D, 0x309F), (0x1B001, 0x1B11F), (0x1B132, 0x1B132), (0x1B150, 0x1B152)]
KATAKANA_RANGES = [(0x3031, 0x3035), (0x309B, 0x309C), (0x30A0, 0x30FA), (0x30FC, 0x30FF), (0x31F0, 0x31FF),
                   (0x32D0, 0x32FE), (0x3300, 0x3357), (0xFF66, 0xFF9D), (0x1AFF0, 0x1AFF3), (0x1AFF5, 0x1AFFB),
                   (0x1AFFD, 0x1AFFE), (0x1B000, 0x1B000), (0x1B120, 0x1B122), (0x1B155, 0x1B155),
                   (0x1B164, 0x1B167)]
HANGUL_RANGES = [(0x1100, 0x11FF), (0x3131, 0x318E), (0xA960, 0xA97C), (0xAC00, 0xD7A3), (0xD7B0, 0xD7FB),
                 (0xFFA0, 0xFFDC)]
SA_RANGES = [(0x0E01, 0x0E3A), (0x0E40, 0x0E4E), (0x0E81, 0x0EDF), (0x1000, 0x102A), (0x103F, 0x1049),
             (0x1050, 0x1055), (0x105A, 0x105D), (0x1061, 0x1061), (0x1065, 0x1066), (0x106E, 0x1070),
             (0x1075, 0x1081), (0x108E, 0x108E), (0x1780, 0x17D3), (0x17D7, 0x17D7), (0x17DC, 0x17DD),
             (0x19E0, 0x19FF), (0xA9E0, 0xA9E4), (0xA9E7, 0xA9FE), (0xAA60, 0xAA7F), (0x1031, 0x103E),
             (0x1056, 0x1059), (0x105E, 0x1060), (0x1062, 0x1064), (0x1067, 0x106D), (0x1071, 0x1074),
             (0x1082, 0x108D), (0x108F, 0x108F), (0x109A, 0x109D)]

MIDLETTER = {0x00B7, 0x0387, 0x055F, 0x05F4, 0x2027, 0xFE13, 0xFE55, 0xFF1A}        # NB: ':' deliberately omitted
MIDNUM = {0x002C, 0x003B, 0x037E, 0x0589, 0x060C, 0x060D, 0x066C, 0x07F8, 0x2044, 0xFE10, 0xFE14, 0xFE50,
          0xFE54, 0xFF0C, 0xFF1B}
MIDNUMLET = {0x0027, 0x002E, 0x2018, 0x2019, 0x2024, 0xFE52, 0xFF07, 0xFF0E}
EXTNUMLET_EXTRA = {0x202F}                                                          # + all of Pc
ALPHABETIC_SO = [(0x24B6, 0x24E9), (0x1F130, 0x1F149), (0x1F150, 0x1F169), (0x1F170, 0x1F189)]

# pictographic symbols (emoji-like), restricted to gc == So at generation time
EMOJI_RANGES = [(0x231A, 0x231B), (0x2328, 0x2328), (0x23CF, 0x23CF), (0x23E9, 0x23F3), (0x23F8, 0x23FA),
                (0x24C2, 0x24C2), (0x25AA, 0x25AB), (0x25B6, 0x25B6), (0x25C0, 0x25C0), (0x25FB, 0x25FE),
                (0x2600, 0x27BF), (0x2934, 0x2935), (0x2B05, 0x2B07), (0x2B1B, 0x2B1C), (0x2B50, 0x2B50),
                (0x2B55, 0x2B55), (0x3030, 0x3030), (0x303D, 0x303D), (0x3297, 0x3297), (0x3299, 0x3299),
                (0x1F004, 0x1F004), (0x1F0CF, 0x1F0CF), (0x1F170, 0x1F171), (0x1F17E, 0x1F17F),
                (0x1F18E, 0x1F18E), (0x1F191, 0x1F19A), (0x1F201, 0x1F202), (0x1F21A, 0x1F21A),
                (0x1F22F, 0x1F22F), (0x1F232, 0x1F23A), (0x1F250, 0x1F251), (0x1F300, 0x1FAFF)]
# misc symbols/dingbats that are plain typography (arrows in dingbats, ornaments, bullets): not emoji
NOT_EMOJI = [(0x2700, 0x2700), (0x2701, 0x2701)]
EMOJI_MODIFIER = (0x1F3FB, 0x1F3FF)
RI_RANGE = (0x1F1E6, 0x1F1FF)

# marks removed by NX_FOLD_DIACRITICS: diacritics of alphabetic scripts that are removed in search
# (Latin/Greek/Cyrillic/Hebrew/Arabic).  Indic, Southeast-Asian and kana marks are kept: they are letters.
DIACRITIC_RANGES = [(0x0300, 0x036F), (0x0483, 0x0489), (0x0591, 0x05BD), (0x05BF, 0x05BF), (0x05C1, 0x05C2),
                    (0x05C4, 0x05C5), (0x05C7, 0x05C7), (0x0610, 0x061A), (0x064B, 0x065F), (0x0670, 0x0670),
                    (0x06D6, 0x06DC), (0x06DF, 0x06E4), (0x06E7, 0x06E8), (0x06EA, 0x06ED), (0x1AB0, 0x1AFF),
                    (0x1DC0, 0x1DFF), (0x20D0, 0x20F0), (0xFE20, 0xFE2F)]

DEFAULT_IGNORABLE = [(0x00AD, 0x00AD), (0x034F, 0x034F), (0x061C, 0x061C), (0x115F, 0x1160), (0x17B4, 0x17B5),
                     (0x180B, 0x180F), (0x200B, 0x200F), (0x202A, 0x202E), (0x2060, 0x206F), (0x3164, 0x3164),
                     (0xFE00, 0xFE0F), (0xFEFF, 0xFEFF), (0xFFA0, 0xFFA0), (0xFFF0, 0xFFF8), (0x1BCA0, 0x1BCA3),
                     (0x1D173, 0x1D17A), (0xE0000, 0xE0FFF)]

# "stroke" letters that have no canonical decomposition but are folded by NX_FOLD_DIACRITICS
# (o-slash -> o etc.); applied after case folding, so both cases are listed for the no-CASE mode.
STROKE_FOLD = {
    0x00C6: "AE", 0x00E6: "ae", 0x00D0: "D", 0x00F0: "d", 0x00D8: "O", 0x00F8: "o", 0x00DE: "TH", 0x00FE: "th",
    0x0110: "D", 0x0111: "d", 0x0126: "H", 0x0127: "h", 0x0131: "i", 0x0141: "L", 0x0142: "l", 0x0152: "OE",
    0x0153: "oe", 0x0166: "T", 0x0167: "t", 0x0180: "b", 0x0197: "I", 0x0268: "i", 0x01B5: "Z", 0x01B6: "z",
    0x01E4: "G", 0x01E5: "g", 0x1D6C: "b", 0x1D6D: "d", 0x1D6E: "f", 0x1D7D: "p", 0x1E9E: "SS",
}


def gc_of(cp):
    if cp in SURR:
        return "Cs"
    return ud.category(chr(cp))


def script_of(cp, gc):
    if in_ranges(cp, HAN_RANGES) and (gc in HAN_WORD_CATS):
        return "HAN"
    if in_ranges(cp, HIRAGANA_RANGES):
        return "HIRAGANA"
    if in_ranges(cp, KATAKANA_RANGES):
        return "KATAKANA"
    if in_ranges(cp, HANGUL_RANGES) and gc in ("Lo", "Lm", "Mn"):
        return "HANGUL"
    if in_ranges(cp, SA_RANGES) and gc in ("Lo", "Mn", "Mc", "Lm"):
        return "SA"
    return "NONE"


def is_emoji(cp, gc):
    if gc != "So":
        return False
    if RI_RANGE[0] <= cp <= RI_RANGE[1]:
        return False
    return in_ranges(cp, EMOJI_RANGES) and not in_ranges(cp, NOT_EMOJI)


def wb_of(cp, gc, scr):
    if cp == 0x200D:
        return "ZWJ"
    if cp == 0x200C or EMOJI_MODIFIER[0] <= cp <= EMOJI_MODIFIER[1] or 0xE0020 <= cp <= 0xE007F:
        return "EXTEND"
    if RI_RANGE[0] <= cp <= RI_RANGE[1]:
        return "RI"
    if is_emoji(cp, gc):
        return "EMOJI"
    if scr == "KATAKANA":
        return "KATAKANA" if gc != "Mn" else "EXTEND"          # (U+3099/309A style marks are Mn, not listed)
    if cp in (0xFF9E, 0xFF9F):
        return "EXTEND"
    if scr == "HIRAGANA":
        return "HIRAGANA"
    if scr == "HAN":
        return "HAN"
    if gc in ("Mn", "Mc", "Me"):
        return "EXTEND"
    if gc == "Cf":
        return "FORMAT"
    if cp in MIDLETTER:
        return "MIDLETTER"
    if cp in MIDNUM:
        return "MIDNUM"
    if cp in MIDNUMLET:
        return "MIDNUMLET"
    if gc == "Pc" or cp in EXTNUMLET_EXTRA:
        return "EXTNUMLET"
    if gc == "Nd" or cp in (0x066B,):
        return "NUMERIC"
    if gc in ("Lu", "Ll", "Lt", "Lm", "Lo", "Nl"):
        return "ALETTER"
    if gc == "So" and in_ranges(cp, ALPHABETIC_SO):
        return "ALETTER"
    return "OTHER"


def props_of(cp):
    gc = gc_of(cp)
    scr = script_of(cp, gc)
    wb = wb_of(cp, gc, scr)
    v = GCI[gc] | (WBI[wb] << PROPS_SHIFT_WB) | (SCI[scr] << PROPS_SHIFT_SCRIPT)
    if gc in ("Mn", "Me") and in_ranges(cp, DIACRITIC_RANGES):
        v |= F_DIACRITIC
    if in_ranges(cp, DEFAULT_IGNORABLE):
        v |= F_IGNORABLE
    if gc in ("Zs", "Zl", "Zp") or cp in (0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x85):
        v |= F_SPACE
    if wb == "EMOJI":
        v |= F_EXTPICT
    return v


# ---------------------------------------------------------------------------------------------
# normalization data
# ---------------------------------------------------------------------------------------------
def single_step(cp):
    """(tag, [cps]) of the UnicodeData.txt decomposition mapping, or None.  Hangul syllables are algorithmic."""
    if 0xAC00 <= cp <= 0xD7A3:
        return None
    d = ud.decomposition(chr(cp))
    if not d:
        return None
    parts = d.split()
    if parts[0].startswith("<"):
        t = parts[0]
        tag = "width" if t in ("<wide>", "<narrow>") else "compat"
        parts = parts[1:]
    else:
        tag = "canon"
    return tag, [int(x, 16) for x in parts]


def composition_pairs():
    pairs = {}
    for cp in range(0x110000):
        if cp in SURR:
            continue
        ss = single_step(cp)
        if ss and ss[0] == "canon" and len(ss[1]) == 2:
            if ud.normalize("NFC", chr(cp)) == chr(cp):         # excludes Composition_Exclusion and non-starter forms
                pairs[(ss[1][0], ss[1][1])] = cp
    return pairs


def casefold_of(cp):
    if cp in SURR:
        return None
    s = chr(cp).casefold()
    if s == chr(cp):
        return None
    return [ord(c) for c in s]


# ---------------------------------------------------------------------------------------------
# two-stage table construction
# ---------------------------------------------------------------------------------------------
def dom_a():
    top = 0
    for cp in range(0xE0000):
        if cp in SURR:
            continue
        if ud.category(chr(cp)) != "Cn":
            top = cp
    return (top + 1 + 255) & ~255


def domain_cps(da):
    return list(range(da)) + list(range(0xE0000, 0xE1000))


def build_two_stage(vals, s2_bytes):
    """vals: values over the domain.  Returns (shift, stage1, stage2, total_bytes) for the best shift."""
    best = None
    for shift in range(3, 10):
        bsz = 1 << shift
        n = len(vals)
        pad = (-n) % bsz
        v = list(vals) + [vals[-1]] * pad          # pad with last value (tail is unassigned anyway)
        blocks = {}
        stage1 = []
        stage2 = []
        for b in range(0, len(v), bsz):
            key = tuple(v[b:b + bsz])
            if key not in blocks:
                blocks[key] = len(blocks)
                stage2.extend(key)
            stage1.append(blocks[key])
        s1_bytes = 1 if len(blocks) <= 256 else 2
        total = len(stage1) * s1_bytes + len(stage2) * s2_bytes
        if best is None or total < best[3]:
            best = (shift, stage1, stage2, total)
    return best


def intern(values_list):
    """Map arbitrary hashable values to dense ids; id 0 is values_list[0]'s default (first interned)."""
    table = {}
    order = []
    out = []
    for v in values_list:
        if v not in table:
            table[v] = len(order)
            order.append(v)
        out.append(table[v])
    return order, out


class Emitter:
    def __init__(self):
        self.lines = []

    def w(self, s=""):
        self.lines.append(s)

    def array(self, ctype, name, data, per_line=None, hexfmt=False, comment=None):
        if comment:
            self.w("/* " + comment + " */")
        self.w("const %s %s[%d] = {" % (ctype, name, len(data)))
        if hexfmt:
            toks = ["0x%X" % x for x in data]
        else:
            toks = [str(x) for x in data]
        line = " "
        for t in toks:
            if len(line) + len(t) + 1 > 108:
                self.w(line.rstrip())
                line = " "
            line += " " + t + ","
        if line.strip():
            self.w(line.rstrip())
        self.w("};")
        self.w()


def ctype_for(maxv):
    if maxv < 256:
        return "uint8_t", 1
    if maxv < 65536:
        return "uint16_t", 2
    return "uint32_t", 4


def utf16(cps):
    out = []
    for c in cps:
        if c >= 0x10000:
            c -= 0x10000
            out.append(0xD800 | (c >> 10))
            out.append(0xDC00 | (c & 0x3FF))
        else:
            out.append(c)
    return out


# ---------------------------------------------------------------------------------------------
# main generation
# ---------------------------------------------------------------------------------------------
def generate(stats_only=False):
    da = dom_a()
    cps = domain_cps(da)
    # ---- props: interned records ------------------------------------------------------------
    prop_vals = [props_of(cp) for cp in cps]
    # force record 0 = Cn default, record 1 = Co default
    cn = GCI["Cn"] | (WBI["OTHER"] << PROPS_SHIFT_WB)
    co = GCI["Co"] | (WBI["OTHER"] << PROPS_SHIFT_WB)
    assert props_of(0x323B0) == cn
    assert props_of(0xF0000) == co
    order = [cn, co]
    idx = {cn: 0, co: 1}
    pidx = []
    for v in prop_vals:
        if v not in idx:
            idx[v] = len(order)
            order.append(v)
        pidx.append(idx[v])
    props_recs = order
    p_t, p_s2b = ctype_for(len(props_recs) - 1)
    props_ts = build_two_stage(pidx, p_s2b)

    # ---- norm -------------------------------------------------------------------------------
    pairs = composition_pairs()
    seconds = sorted({b for (_, b) in pairs})
    rank_of = {b: i + 1 for i, b in enumerate(seconds)}
    assert len(seconds) <= 126
    comp_keys = sorted((a << 7) | rank_of[b] for (a, b) in pairs)
    comp_val = {(a << 7) | rank_of[b]: c for (a, b), c in pairs.items()}
    comp_res = [comp_val[k] for k in comp_keys]

    def norm_of(cp):
        ccc = ud.combining(chr(cp)) if cp not in SURR else 0
        rank = rank_of.get(cp, 0)
        if 0x1161 <= cp <= 0x1175 or 0x11A8 <= cp <= 0x11C2:
            assert rank == 0
            rank = RANK_HANGUL
        ss = single_step(cp)
        dt = DTAG[ss[0]] if ss else 0
        return ccc | (rank << 8) | (dt << 15)
    norm_vals = [norm_of(cp) for cp in cps]
    n_order, nidx = intern([0] + norm_vals)     # id 0 = "nothing"
    nidx = nidx[1:]
    n_t, n_s2b = ctype_for(len(n_order) - 1)
    norm_ts = build_two_stage(nidx, n_s2b)

    # ---- fold ---------------------------------------------------------------------------------
    pool = []            # full-fold code points (uint32)
    fold_recs = [0]      # record 0 = none
    fold_idx_of = {}
    fidx = []
    for cp in cps:
        f = casefold_of(cp)
        if f is None:
            fidx.append(0)
            continue
        if len(f) == 1:
            delta = f[0] - cp
            assert -(1 << 23) <= delta < (1 << 23)
            rec = (1 << 30) | (delta & 0xFFFFFF)
        else:
            assert len(f) <= 3
            key = tuple(f)
            off = None
            for i in range(len(pool) - len(f) + 1):
                if tuple(pool[i:i + len(f)]) == key:
                    off = i
                    break
            if off is None:
                off = len(pool)
                pool.extend(f)
            assert off < 65536
            rec = (2 << 30) | ((len(f) - 1) << 16) | off
        if rec not in fold_idx_of:
            fold_idx_of[rec] = len(fold_recs)
            fold_recs.append(rec)
        fidx.append(fold_idx_of[rec])
    f_t, f_s2b = ctype_for(len(fold_recs) - 1)
    fold_ts = build_two_stage(fidx, f_s2b)

    # ---- decomposition (single step) ---------------------------------------------------------
    dpool = []           # UTF-16 units
    d_recs = [(0, 0)]    # (offset, len_in_cps)  -- record 0 = none; tag comes from the norm table
    d_idx_of = {}
    didx = []
    for cp in cps:
        ss = single_step(cp)
        if not ss:
            didx.append(0)
            continue
        units = utf16(ss[1])
        key = tuple(units)
        off = None
        for i in range(len(dpool) - len(units) + 1):
            if tuple(dpool[i:i + len(units)]) == key:
                off = i
                break
        if off is None:
            off = len(dpool)
            dpool.extend(units)
        assert off < 65536 and len(units) < 64
        rec = (off, len(units))
        if rec not in d_idx_of:
            d_idx_of[rec] = len(d_recs)
            d_recs.append(rec)
        didx.append(d_idx_of[rec])
    d_t, d_s2b = ctype_for(len(d_recs) - 1)
    decomp_ts = build_two_stage(didx, d_s2b)

    # ---- strip table ---------------------------------------------------------------------------
    strip_cps = sorted(STROKE_FOLD)
    strip_pool = []
    strip_recs = []
    for cp in strip_cps:
        s = [ord(c) for c in STROKE_FOLD[cp]]
        off = len(strip_pool)
        strip_pool.extend(s)
        strip_recs.append((off << 8) | len(s))

    sizes = {
        "props": props_ts[3] + 4 * len(props_recs),
        "norm": norm_ts[3] + 4 * len(n_order),
        "fold": fold_ts[3] + 4 * len(fold_recs) + 4 * len(pool),
        "decomp": decomp_ts[3] + 4 * len(d_recs) + 2 * len(dpool),
        "comp": 8 * len(comp_keys) + 4 * len(seconds),
    }
    if stats_only:
        print("DOM_A = 0x%X  domain size %d" % (da, len(cps)))
        for name, ts, recs in (("props", props_ts, props_recs), ("norm", norm_ts, n_order),
                               ("fold", fold_ts, fold_recs), ("decomp", decomp_ts, d_recs)):
            print("%-7s shift=%d stage1=%d stage2=%d records=%d  bytes(stage)=%d" %
                  (name, ts[0], len(ts[1]), len(ts[2]), len(recs), ts[3]))
        print("fold pool", len(pool), "decomp pool", len(dpool), "pairs", len(comp_keys), "seconds", len(seconds))
        print("approx object bytes:", sizes, "total", sum(sizes.values()))
        return None

    e = Emitter()
    e.w("/* nx_unicode_data.c - GENERATED by tools/gen_unicode.py from Python %d.%d unicodedata (Unicode %s)." %
        (sys.version_info[0], sys.version_info[1], ud.unidata_version))
    e.w(" * DO NOT EDIT.  Re-run `python tools/gen_unicode.py` to regenerate.  See nx_unicode.h for the layout. */")
    e.w('#include "text/nx_unicode.h"')
    e.w()
    e.w("/* The generator's enumerations must match nx_unicode.h. */")
    for i, n in enumerate(GC):
        e.w("NX_STATIC_ASSERT(NX_GC_%s == %d, \"nx_gc layout\");" % (n, i))
    for i, n in enumerate(WB):
        e.w("NX_STATIC_ASSERT(NX_WB_%s == %d, \"nx_wb layout\");" % (n, i))
    for i, n in enumerate(SCRIPT):
        e.w("NX_STATIC_ASSERT(NX_SCRIPT_%s == %d, \"nx_script layout\");" % (n, i))
    e.w("NX_STATIC_ASSERT(NX_UCD_F_DIACRITIC == 0x%X && NX_UCD_F_IGNORABLE == 0x%X && NX_UCD_F_SPACE == 0x%X &&"
        % (F_DIACRITIC, F_IGNORABLE, F_SPACE))
    e.w("                 NX_UCD_F_EXTPICT == 0x%X, \"props flag layout\");" % F_EXTPICT)
    e.w("NX_STATIC_ASSERT(NX_UCD_RANK_HANGUL == %d, \"rank layout\");" % RANK_HANGUL)
    e.w()
    e.w("const uint32_t nx_ucd_format = %d;" % FORMAT_VERSION)
    maj, mnr, upd = (int(x) for x in ud.unidata_version.split("."))
    e.w("const uint32_t nx_ucd_version[3] = { %d, %d, %d };" % (maj, mnr, upd))
    e.w("const uint32_t nx_ucd_dom_a = 0x%X;     /* two-stage domain: [0, dom_a) and [0xE0000, 0xE1000) */" % da)
    e.w()

    def two_stage(prefix, ts, s2type, recs_name=None):
        shift, s1, s2, _ = ts
        s1t, _ = ctype_for(max(s1))
        e.w("const uint32_t %s_shift = %d;" % (prefix, shift))
        e.array(s1t, prefix + "_s1", s1)
        e.array(s2type, prefix + "_s2", s2)

    e.w("/* ---- props: gc | wb<<5 | script<<9 | flags -------------------------------------- */")
    two_stage("nx_ucd_props", props_ts, p_t)
    e.array("uint16_t", "nx_ucd_props_rec", props_recs, hexfmt=True)
    e.w("/* ---- norm: ccc | second_rank<<8 | dtag<<15 ------------------------------------------ */")
    two_stage("nx_ucd_norm", norm_ts, n_t)
    e.array("uint32_t", "nx_ucd_norm_rec", n_order, hexfmt=True)
    e.w("/* ---- case folding ------------------------------------------------------------------ */")
    two_stage("nx_ucd_fold", fold_ts, f_t)
    e.array("uint32_t", "nx_ucd_fold_rec", fold_recs, hexfmt=True)
    e.array("uint16_t", "nx_ucd_fold_pool", pool, hexfmt=True)
    e.w("/* ---- single-step decompositions (UTF-16 pool) ------------------------------------- */")
    two_stage("nx_ucd_decomp", decomp_ts, d_t)
    e.array("uint16_t", "nx_ucd_decomp_off", [r[0] for r in d_recs])
    e.array("uint8_t", "nx_ucd_decomp_len", [r[1] for r in d_recs])
    e.array("uint16_t", "nx_ucd_decomp_pool", dpool, hexfmt=True)
    e.w("/* ---- primary composites: key = first<<7 | second_rank ------------------------------ */")
    e.array("uint32_t", "nx_ucd_comp_key", comp_keys, hexfmt=True)
    e.array("uint32_t", "nx_ucd_comp_val", comp_res, hexfmt=True)
    e.w("const uint32_t nx_ucd_comp_count = %d;" % len(comp_keys))
    e.w()
    e.w("/* ---- stroke letters folded by NX_FOLD_DIACRITICS (sorted by code point) ------------- */")
    e.array("uint32_t", "nx_ucd_strip_cp", strip_cps, hexfmt=True)
    e.array("uint16_t", "nx_ucd_strip_rec", strip_recs, hexfmt=True, comment="(pool offset << 8) | length")
    e.array("uint8_t", "nx_ucd_strip_pool", strip_pool, hexfmt=True)
    e.w("const uint32_t nx_ucd_strip_count = %d;" % len(strip_cps))
    return "\n".join(e.lines) + "\n", sizes, (props_ts, norm_ts, fold_ts, decomp_ts, props_recs, n_order, fold_recs,
                                              d_recs, pool, dpool, comp_keys, comp_res, strip_cps)


# ---------------------------------------------------------------------------------------------
if __name__ == "__main__":
    args = sys.argv[1:]
    if "--stats" in args:
        generate(stats_only=True)
        sys.exit(0)
    text, sizes, _ = generate()
    if "--check" in args:
        cur = open(OUT_C, encoding="utf-8").read() if os.path.exists(OUT_C) else ""
        if cur != text:
            print("nx_unicode_data.c is out of date")
            sys.exit(1)
        print("nx_unicode_data.c is up to date")
        sys.exit(0)
    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d bytes source); approx object %d bytes" % (OUT_C, len(text), sum(sizes.values())))
