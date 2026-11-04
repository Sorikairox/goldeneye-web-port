/*
 * romfill — build step for "template + fill" (docs/web/ROMDATA.md, PLAN.md 3.2 / W3).
 *
 *   romfill [-m MINRUN] ROM IN.wasm LINK.map OUT.wasm OUT.romfill REPORT.tsv
 *
 * Takes the linked web game (IN.wasm, with the real data compiled in) and its wasm-ld map, and
 * for every data symbol that comes from the decompiled game, its assets or the port's C layer
 * (objects under CMakeFiles/ge007.dir/{src,assets,srcfix,port/src}/, not port/src/web/, plus the
 * linker-merged string pool) finds the byte
 * runs that also exist in the ROM: in the ROM itself or in any of its 0x1172 (RZ, raw deflate)
 * streams, read big-endian, with every 32-bit word byte-swapped, or with every 16-bit half
 * byte-swapped (the PC build stores the decomp's tables as little-endian values). Each run of
 * >= MINRUN (default 16) bytes with a non-zero byte becomes a fill entry; OUT.wasm is IN.wasm with
 * those bytes zeroed in its data section (the "template": everything else, pointers included,
 * stays exactly as linked), and OUT.romfill is the offsets-only table the page applies at boot
 * (port/src/web/romfill.c) from the player's ROM:
 *
 *   header  u32 magic 'GERF', u32 version 1, u32 count, u32 crc32 of the filled bytes (in entry
 *           order, after the fill), u32 crc32 of all selected symbols' bytes, u32 nsyms
 *   nsyms x { u32 addr, u32 size }                        the selected symbols (whole extents)
 *   count x { u32 dst (memory address), u32 len, u32 rom_off (ROM offset of the raw ROM or of an
 *             RZ stream), u32 src_off (offset inside it), u8 kind (0 raw, 1 RZ), u8 order
 *             (0 as is, 1 32-bit swapped, 2 16-bit swapped), u16 pad }
 *
 * Before writing, the tool applies the table to the zeroed image itself and requires it to be
 * byte-identical to IN.wasm's data (exit 1 otherwise). The table holds no ROM bytes (offsets,
 * lengths and two CRCs only). REPORT.tsv lists per symbol: size, non-zero bytes, bytes filled.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define BLK 8

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "romfill: cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n + 1);
    if (n && fread(b, 1, n, f) != (size_t)n) { fprintf(stderr, "romfill: read %s\n", path); exit(2); }
    fclose(f);
    b[n] = 0;
    *len = n;
    return b;
}

/* ------------------------------------------------------------------ corpus */
typedef struct {
    uint32_t rom_off;   /* raw: 0; RZ: offset of the 0x11 0x72 header */
    int kind, order;
    uint8_t *p;
    long n;
} Seg;
static Seg *segs;
static int nsegs, capsegs;

static void add_seg(uint32_t rom_off, int kind, int order, uint8_t *p, long n)
{
    if (nsegs == capsegs) { capsegs = capsegs ? capsegs * 2 : 256; segs = realloc(segs, capsegs * sizeof(Seg)); }
    segs[nsegs++] = (Seg){ rom_off, kind, order, p, n };
}

static long try_inflate(const uint8_t *in, long inlen, uint8_t **out)
{
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, -15) != Z_OK) return -1;
    long cap = 1 << 16, len = 0;
    uint8_t *b = malloc(cap);
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)inlen;
    int r;
    for (;;) {
        if (len == cap) { cap *= 2; b = realloc(b, cap); }
        zs.next_out = b + len;
        zs.avail_out = (uInt)(cap - len);
        r = inflate(&zs, Z_NO_FLUSH);
        len = cap - zs.avail_out;
        if (r == Z_STREAM_END) break;
        if (r != Z_OK && !(r == Z_BUF_ERROR && zs.avail_out == 0)) break;
        if (len > (64L << 20)) break;
    }
    inflateEnd(&zs);
    if (r != Z_STREAM_END || len < 16) { free(b); return -1; }
    *out = b;
    return len;
}

static uint8_t *swapped(const uint8_t *p, long n, int w)
{
    uint8_t *q = malloc(n);
    memcpy(q, p, n);
    for (long i = 0; i + w <= n; i += w)
        for (int k = 0; k < w; k++) q[i + k] = p[i + w - 1 - k];
    return q;
}

static const uint64_t HB = 0x100000001b3ULL;
static uint64_t HBK;
static uint64_t hblock(const uint8_t *p)
{
    uint64_t h = 0;
    for (int i = 0; i < BLK; i++) h = h * HB + p[i] + 1;
    return h;
}

typedef struct { uint64_t h; int32_t seg; uint32_t off; } Ent;
static Ent *tab;
static uint64_t tabmask;

static void index_seg(int si)
{
    Seg *s = &segs[si];
    for (long o = 0; o + BLK <= s->n; o += BLK) {
        const uint8_t *b = s->p + o;
        int same = 1;
        for (int k = 1; k < BLK; k++) if (b[k] != b[0]) { same = 0; break; }
        if (same) continue;
        uint64_t h = hblock(b);
        uint64_t i = (h * 0x9E3779B97F4A7C15ULL) & tabmask;
        while (tab[i].seg >= 0) {
            if (tab[i].h == h && !memcmp(segs[tab[i].seg].p + tab[i].off, b, BLK)) goto dup;
            i = (i + 1) & tabmask;
        }
        tab[i].h = h; tab[i].seg = si; tab[i].off = (uint32_t)o;
    dup:;
    }
}

/* ------------------------------------------------------------------- wasm */
typedef struct { uint64_t addr; long foff, n; } DSeg;
static DSeg dsegs[4096];
static int ndsegs;

static uint64_t uleb(const uint8_t *p, long *o)
{
    uint64_t v = 0; int sh = 0;
    for (;;) { uint8_t b = p[(*o)++]; v |= (uint64_t)(b & 0x7f) << sh; sh += 7; if (!(b & 0x80)) break; }
    return v;
}
static int64_t sleb(const uint8_t *p, long *o)
{
    int64_t v = 0; int sh = 0; uint8_t b;
    do { b = p[(*o)++]; v |= (int64_t)(b & 0x7f) << sh; sh += 7; } while (b & 0x80);
    if (sh < 64 && (b & 0x40)) v |= -((int64_t)1 << sh);
    return v;
}

static void parse_wasm(const uint8_t *w, long n)
{
    if (n < 8 || memcmp(w, "\0asm", 4)) { fprintf(stderr, "romfill: not a wasm file\n"); exit(2); }
    long o = 8;
    while (o < n) {
        int id = w[o++];
        uint64_t sz = uleb(w, &o);
        long end = o + (long)sz;
        if (id == 11) {
            long q = o;
            uint64_t cnt = uleb(w, &q);
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t flags = uleb(w, &q);
                uint64_t addr = 0;
                int active = flags == 0 || flags == 2;
                if (flags == 2) uleb(w, &q);
                if (active) {
                    int op = w[q++];
                    if (op == 0x41 || op == 0x42) addr = (uint64_t)sleb(w, &q);
                    else { fprintf(stderr, "romfill: data segment offset opcode %#x\n", op); exit(2); }
                    if (w[q++] != 0x0b) { fprintf(stderr, "romfill: bad const expr\n"); exit(2); }
                }
                uint64_t len = uleb(w, &q);
                if (active && ndsegs < 4096) dsegs[ndsegs++] = (DSeg){ addr, q, (long)len };
                q += (long)len;
            }
        }
        o = end;
    }
}

/* The data image: [img_lo, img_hi) in memory, with each byte's file offset (-1 where the
 * linker / wasm-opt left a zero gap between segments). */
static uint64_t img_lo, img_hi;
static uint8_t *img;          /* original bytes */
static long *img_foff;

static void build_image(const uint8_t *w)
{
    img_lo = UINT64_MAX; img_hi = 0;
    for (int i = 0; i < ndsegs; i++) {
        if (dsegs[i].addr < 0x20000000ull || dsegs[i].addr >= 0x80000000ull) continue;   /* cart window / DRAM */
        if (dsegs[i].addr < img_lo) img_lo = dsegs[i].addr;
        if (dsegs[i].addr + dsegs[i].n > img_hi) img_hi = dsegs[i].addr + dsegs[i].n;
    }
    long n = (long)(img_hi - img_lo);
    img = calloc(n, 1);
    img_foff = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) img_foff[i] = -1;
    for (int i = 0; i < ndsegs; i++) {
        if (dsegs[i].addr < img_lo || dsegs[i].addr + dsegs[i].n > img_hi) continue;
        long b = (long)(dsegs[i].addr - img_lo);
        memcpy(img + b, w + dsegs[i].foff, dsegs[i].n);
        for (long k = 0; k < dsegs[i].n; k++) img_foff[b + k] = dsegs[i].foff + k;
    }
}

/* memory range inside the image? returns its image index or -1 */
static long mem2img(uint64_t a, long len)
{
    if (a < img_lo || a + len > img_hi) return -1;
    return (long)(a - img_lo);
}

/* -------------------------------------------------------------------- map */
typedef struct { uint64_t addr; long size; char name[160]; } Sym;
static Sym *syms;
static int nsyms, capsyms;

static int selected(const char *in)
{
    if (!strncmp(in, "<internal>:(.rodata)", 20)) return 1;
    if (strncmp(in, "CMakeFiles/ge007.dir/", 21)) return 0;
    const char *p = in + 21;
    /* the decomp (src/, srcfix/ build copies, assets/) and the port's C layer, which carries a few
     * copies of ROM tables (mixer.c's resample table); not port/src/web/ (page-facing code, no ROM
     * data) and not the C++ renderer (static constructors run before
     * the fill). */
    if (strncmp(p, "src/", 4) && strncmp(p, "assets/", 7) && strncmp(p, "srcfix/", 7) &&
        (strncmp(p, "port/src/", 9) || !strncmp(p, "port/src/web/", 13))) return 0;
    if (strstr(in, ".cpp.o:(")) return 0;
    const char *s = strstr(in, ":(");
    /* port C: read-only tables only (its .data may be changed by constructors before the fill) */
    if (s && !strncmp(p, "port/src/", 9) && strncmp(s + 2, ".rodata", 7)) return 0;
    return s && (!strncmp(s + 2, ".data", 5) || !strncmp(s + 2, ".rodata", 7));
}

/* pass 2 (short anchored pieces) applies to the decomp's and the assets' data only */
static int pass2_selected(const char *in)
{
    if (strncmp(in, "CMakeFiles/ge007.dir/", 21)) return 0;
    const char *p = in + 21;
    return !strncmp(p, "src/", 4) || !strncmp(p, "assets/", 7) || !strncmp(p, "srcfix/", 7);
}

static void parse_map(char *m)
{
    for (char *line = strtok(m, "\n"); line; line = strtok(NULL, "\n")) {
        char a[32], off[32], size[32], in[1024];
        if (sscanf(line, "%31s %31s %31s %1023s", a, off, size, in) != 4) continue;
        if (!strcmp(a, "-")) continue;
        if (!selected(in)) continue;
        if (nsyms == capsyms) { capsyms = capsyms ? capsyms * 2 : 4096; syms = realloc(syms, capsyms * sizeof(Sym)); }
        Sym *s = &syms[nsyms++];
        s->addr = strtoull(a, NULL, 16);
        s->size = strtol(size, NULL, 16);
        snprintf(s->name, sizeof s->name, "%s", in);
    }
}

/* ----------------------------------------------------------------- output */
typedef struct { uint32_t dst, len, rom_off, src_off; uint8_t kind, order; } Fill;
static Fill *fills;
static long nfills, capfills;

/* ---------------------------------------------------------------- pass 2 ---
 * (decomp + assets symbols only, pass2_selected: the string pool and the port's C tables keep their
 * pass-1 runs; short pieces there were coincidences that broke constructor-time option keys.)
 * Short pieces (< MINRUN) between pointers and padding: vertices (12 bytes 16-bit swapped + 4 bytes
 * as is), display-list words (the PC Gfx keeps each N64 word in the low half of a u64), mixed
 * u8/s16/f32 struct fields. Walking each symbol in address order, the last fill (pass 1 or 2) of
 * the same object gives an anchor (stream, offset delta); the next unfilled non-zero, non-pointer
 * byte is looked up near anchor + delta in the anchor's stream, in all three byte orders, and the
 * longest match wins (ties: nearest). The anchor then moves to that match, so pieces follow the
 * N64 layout locally even where the PC layout is wider. Acceptance: >= 4 bytes within the window,
 * 2-3 bytes within 16 of the prediction, 1 byte only exactly at it; once per symbol, when nothing
 * is found locally, a 64 KiB window re-anchors on a piece of 6+ bytes. Mixed-width structs (s16
 * pairs next to an f32 next to u8[4]) are matched as 4-byte units at 4-aligned stream offsets, each
 * unit in its own byte order, or split into two 16-bit halves each as is or swapped (an s16 next
 * to two s8) (>= 8 bytes, >= 2 non-zero units). Every accepted piece is exact
 * by construction (the bytes are equal); the self-check below proves the whole table. */
static const char *g_debug;   /* -d SUBSTRING: trace pass 2 for matching symbols */
static uint8_t *ptrmask;
static uint8_t *fillmask;

static void mark_pointers(void)
{
    long imn = (long)(img_hi - img_lo);
    ptrmask = calloc(imn, 1);
    for (long i = (long)((8 - (img_lo & 7)) & 7); i + 8 <= imn; i += 8) {
        uint64_t v = 0;
        for (int k = 7; k >= 0; k--) v = v << 8 | img[i + k];
        if ((v >= 0x10000000ull && v < 0x30000000ull) || (v >= 0x80000000ull && v < 0x80800000ull))
            memset(ptrmask + i, 1, 8);
    }
}

static int sib(int base, int order, int nbase)
{
    return order == 0 ? base : nbase + 2 * base + (order - 1);
}

static int g_nbase;
static void add_fill(uint8_t *tmpl, long imgoff, long len, int seg, long src)
{
    if (nfills == capfills) { capfills = capfills ? capfills * 2 : 4096; fills = realloc(fills, capfills * sizeof(Fill)); }
    Seg *g = &segs[seg];
    fills[nfills++] = (Fill){ (uint32_t)(img_lo + imgoff), (uint32_t)len, g->rom_off, (uint32_t)src,
                              (uint8_t)g->kind, (uint8_t)g->order };
    memset(tmpl + imgoff, 0, len);
    memset(fillmask + imgoff, 1, len);
}

static int base_of(int seg) { return seg < g_nbase ? seg : (seg - g_nbase) / 2; }

/* Display lists: the PC Gfx is { u64 w0; u64 w1 } holding the N64 words in the low halves, so a
 * DL never forms a contiguous run. For every symbol that looks like a Gfx array (16-byte slots whose
 * high halves are all zero) the N64 image is rebuilt (w0, w1 per 8 bytes) and matched like pass 1;
 * each matched word is filled from the 32-bit-swapped stream. */
static long pass_gfx(uint8_t *tmpl, long minrun)
{
    long total = 0;
    for (int si = 0; si < nsyms; si++) {
        Sym *sy = &syms[si];
        long fo = mem2img(sy->addr, sy->size);
        if (fo < 0 || sy->size < 32 || sy->size % 16 || ((img_lo + fo) & 7) || !pass2_selected(sy->name)) continue;
        const uint8_t *q = img + fo;
        int gfx = 1;
        for (long k = 0; k < sy->size && gfx; k += 16)
            for (int b = 4; b < 8; b++) if (q[k + b] || q[k + 8 + b]) { gfx = 0; break; }
        if (!gfx) continue;
        long n = sy->size / 2;
        uint8_t *c = malloc(n);
        for (long k = 0; k < sy->size / 16; k++) { memcpy(c + 8 * k, q + 16 * k, 4); memcpy(c + 8 * k + 4, q + 16 * k + 8, 4); }
        long o = 0;
        uint64_t h = n >= BLK ? hblock(c) : 0;
        while (o + BLK <= n) {
            uint64_t i = (h * 0x9E3779B97F4A7C15ULL) & tabmask;
            long best_len = 0, best_s = 0, best_so = 0;
            int best_seg = -1, tries = 0;
            for (; tab[i].seg >= 0 && tries < 256; i = (i + 1) & tabmask) {
                if (tab[i].h != h) continue;
                tries++;
                Seg *g = &segs[tab[i].seg];
                long so = tab[i].off;
                if (g->order != 1 || memcmp(g->p + so, c + o, BLK)) continue;
                long e = BLK;
                while (o + e < n && so + e < g->n && c[o + e] == g->p[so + e]) e++;
                /* whole 8-byte commands only, so the word split below stays aligned */
                long so0 = so - (o & 7), o0 = o - (o & 7);
                if (so0 < 0) continue;
                long b = o & 7;
                for (long k = 0; k < b; k++) if (c[o0 + k] != g->p[so0 + k]) { b = -1; break; }
                if (b < 0) continue;
                long len = (o + e - o0) & ~7L;
                if (len > best_len) { best_len = len; best_s = o0; best_so = so0; best_seg = tab[i].seg; }
            }
            if (best_seg >= 0 && best_len >= minrun) {
                for (long w = 0; w < best_len; w += 4) {
                    long cw = best_s + w;
                    long pc = fo + (cw / 8) * 16 + ((cw & 4) ? 8 : 0);
                    int nz = 0, free_ = 1;
                    for (int k = 0; k < 4; k++) { nz |= img[pc + k]; free_ &= !fillmask[pc + k] && !ptrmask[pc + k]; }
                    if (!nz || !free_) continue;
                    add_fill(tmpl, pc, 4, best_seg, best_so + w);
                    total += 4;
                }
                o = best_s + best_len;
                if (o + BLK > n) break;
                h = hblock(c + o);
                continue;
            }
            if (o + BLK >= n) break;
            h = h * HB + (uint64_t)(c[o + BLK] + 1) - (uint64_t)(c[o] + 1) * HBK;
            o++;
        }
        free(c);
    }
    return total;
}

/* one candidate search: longest single-order match, or mixed-order 4-byte units, near pred */
typedef struct { long len, src, dist, u0; int seg, mixed; int mord[64]; } Cand;

static void search_single(int base, long pred, long win, long p, long end, Cand *best)
{
    for (int o = 0; o < 3; o++) {
        int sg = sib(base, o, g_nbase);
        Seg *g = &segs[sg];
        long lo = pred - win < 0 ? 0 : pred - win, hi = pred + win > g->n ? g->n : pred + win;
        for (long sp = lo; sp < hi; sp++) {
            if (g->p[sp] != img[p]) continue;
            long L = 0;
            while (p + L < end && sp + L < g->n && !fillmask[p + L] && !ptrmask[p + L] && g->p[sp + L] == img[p + L]) L++;
            while (L > 0 && !img[p + L - 1]) L--;   /* end on a non-zero byte */
            long d = sp > pred ? sp - pred : pred - sp;
            if (L > best->len || (L == best->len && d < best->dist)) {
                best->len = L; best->src = sp; best->dist = d; best->seg = sg; best->mixed = 0; best->u0 = p;
            }
        }
    }
}

static void search_mixed(int base, long pred, long win, long p, long fo, long end, Cand *best)
{
    long u0 = p - (long)((img_lo + p) & 3);
    if (u0 < fo) return;
    long upred = pred - (p - u0);
    long lo = upred - win < 0 ? 0 : (upred - win) & ~3L, hi = upred + win;
    for (long sp = lo; sp < hi; sp += 4) {
        int ord[64];
        long L = 0, nzu = 0;
        while (L < 256 && u0 + L + 4 <= end) {
            const uint8_t *x = img + u0 + L;
            int clean = 1, hit = -1;
            for (int k = 0; k < 4; k++) clean &= !fillmask[u0 + L + k] && !ptrmask[u0 + L + k];
            if (!clean) break;
            for (int o = 0; o < 3 && hit < 0; o++) {
                Seg *g = &segs[sib(base, o, g_nbase)];
                if (sp + L + 4 <= g->n && !memcmp(g->p + sp + L, x, 4)) hit = o;
            }
            if (hit < 0) {   /* halves in different orders (s16 next to two s8): 16-bit units */
                Seg *g0 = &segs[sib(base, 0, g_nbase)], *g2 = &segs[sib(base, 2, g_nbase)];
                int h[2] = { -1, -1 };
                for (int k = 0; k < 2 && sp + L + 4 <= g0->n; k++) {
                    if (!memcmp(g0->p + sp + L + 2 * k, x + 2 * k, 2)) h[k] = 0;
                    else if (!memcmp(g2->p + sp + L + 2 * k, x + 2 * k, 2)) h[k] = 2;
                }
                if (h[0] >= 0 && h[1] >= 0) hit = 10 + 3 * h[0] + h[1];
            }
            if (hit < 0) break;
            ord[L / 4] = hit;
            nzu += (x[0] | x[1] | x[2] | x[3]) != 0;
            L += 4;
        }
        while (L > 0 && !(img[u0 + L - 4] | img[u0 + L - 3] | img[u0 + L - 2] | img[u0 + L - 1])) L -= 4;
        if (nzu < 2 || L < 8) continue;
        long d = sp > upred ? sp - upred : upred - sp;
        /* a mixed match counts from p, like a single one */
        long Lp = L - (p - u0);
        if (Lp > best->len || (Lp == best->len && d < best->dist)) {
            best->len = Lp; best->src = sp; best->dist = d; best->seg = base; best->mixed = 1; best->u0 = u0;
            memcpy(best->mord, ord, sizeof(int) * (L / 4));
        }
    }
}

static long pass2(uint8_t *tmpl, long minrun)
{
    const long W = 1024, WW = 65536;
    const char *dbg = g_debug;
    long imn = (long)(img_hi - img_lo), total = 0;
    fillmask = calloc(imn, 1);
    for (long i = 0; i < nfills; i++) memset(fillmask + (fills[i].dst - img_lo), 1, fills[i].len);
    total += pass_gfx(tmpl, minrun);
    /* fills by image offset: the anchor follows them as the walk passes */
    long *anchor_at = malloc(imn * sizeof(long));
    for (long i = 0; i < imn; i++) anchor_at[i] = -1;
    for (long i = 0; i < nfills; i++) for (uint32_t k = 0; k < fills[i].len; k++) anchor_at[fills[i].dst - img_lo + k] = i;
    /* most recently used streams with their last delta (src offset - image offset) */
    enum { NMRU = 4 };
    int mru[NMRU];
    long mdel[NMRU];
    int nmru = 0;
#define USE(base, delta) do { int _b = (base); long _d = (delta), _i; \
        for (_i = 0; _i < nmru && mru[_i] != _b; _i++); \
        if (_i == nmru) { if (nmru < NMRU) nmru++; _i = nmru - 1; } \
        for (; _i > 0; _i--) { mru[_i] = mru[_i - 1]; mdel[_i] = mdel[_i - 1]; } \
        mru[0] = _b; mdel[0] = _d; } while (0)
    for (int si = 0; si < nsyms; si++) {
        Sym *sy = &syms[si];
        long fo = mem2img(sy->addr, sy->size);
        if (fo < 0 || sy->size <= 0) continue;
        /* Pass 2 only for the decomp and its assets. Not for the merged string pool (every TU's strings:
         * port, SDL, libc next to the game's) nor the port's own C tables: there, pieces < MINRUN near an
         * anchor are coincidences (ASCII letters, small ints), not ROM data ("Input.MouseSensitivity" lost
         * 4 bytes, config.c's padded key copies, video.c's preset table). The page fills after the C
         * constructors, which register option keys and copy default bindings from these bytes, so such a
         * piece broke them (100+ "option '...' not registered", bind default 'Do'). Those symbols keep
         * their pass-1 runs (>= MINRUN: the game's names and paths, mixer.c's resample table). */
        if (!pass2_selected(sy->name)) continue;
        long end = fo + sy->size;
        if (!nmru) {   /* nothing yet: anchor on an 8-byte block found anywhere */
            for (long k = fo; k + BLK <= end && !nmru; k++) {
                int nzc = 0, clean = 1;
                for (int b = 0; b < BLK; b++) { nzc += img[k + b] != 0; clean &= !ptrmask[k + b] && !fillmask[k + b]; }
                if (nzc < 4 || !clean) continue;
                uint64_t h = hblock(img + k);
                for (uint64_t i = (h * 0x9E3779B97F4A7C15ULL) & tabmask; tab[i].seg >= 0; i = (i + 1) & tabmask) {
                    if (tab[i].h != h || memcmp(segs[tab[i].seg].p + tab[i].off, img + k, BLK)) continue;
                    USE(base_of(tab[i].seg), (long)tab[i].off - k);
                    break;
                }
            }
        }
        if (dbg && strstr(sy->name, dbg)) {
            fprintf(stderr, "dbg sym %s mru %d:", sy->name, nmru);
            for (long k = fo; k < end; k++) fprintf(stderr, " %02x%s%s", img[k], fillmask[k] ? "f" : "", ptrmask[k] ? "p" : "");
            fprintf(stderr, "\n");
        }
        int wide = 0;
        for (long p = fo; p < end;) {
            if (anchor_at[p] >= 0) {
                Fill *f = &fills[anchor_at[p]];
                for (int q = 0; q < g_nbase; q++)
                    if (segs[q].rom_off == f->rom_off && segs[q].kind == f->kind) { USE(q, (long)f->src_off - (long)(f->dst - img_lo)); break; }
                p = (long)(f->dst - img_lo) + f->len;
                continue;
            }
            if (fillmask[p] || !img[p] || ptrmask[p] || !nmru) { p++; continue; }
            Cand best;
            memset(&best, 0, sizeof best);
            best.seg = -1;
            int bm = 0;
            for (int m = 0; m < nmru; m++) {
                Cand c;
                memset(&c, 0, sizeof c);
                c.seg = -1;
                long win = wide == 1 && m == 0 ? WW : W;
                search_single(mru[m], p + mdel[m], win, p, end, &c);
                search_mixed(mru[m], p + mdel[m], win, p, fo, end, &c);
                if (c.seg >= 0 && (c.len > best.len || (c.len == best.len && c.dist < best.dist))) { best = c; bm = m; }
            }
            int ok;
            if (wide == 1) { ok = best.seg >= 0 && best.len >= 6; wide = 2; }
            else ok = best.seg >= 0 && (best.len >= 4 || (best.len >= 2 && best.dist <= 16) || (best.len == 1 && best.dist == 0));
            if (dbg && strstr(sy->name, dbg))
                fprintf(stderr, "dbg %s +%ld: best %ld@%#lx d%ld mixed %d mru#%d ok %d wide %d\n", sy->name, p - fo, best.len,
                        best.src, best.dist, best.mixed, bm, ok, wide);
            if (!ok && !wide && best.len < 6) { wide = 1; continue; }   /* retry this byte with a wide window */
            if (!ok) { p++; continue; }
            int base = best.mixed ? best.seg : base_of(best.seg);
            if (!best.mixed) {
                add_fill(tmpl, p, best.len, best.seg, best.src);
                total += best.len;
                USE(base, best.src - p);
                p += best.len;
            } else {
                long u0 = best.u0, mlen = best.len + (p - u0);
                for (long u = 0; u < mlen; u += 4) {
                    if (!(img[u0 + u] | img[u0 + u + 1] | img[u0 + u + 2] | img[u0 + u + 3])) continue;
                    if (best.mord[u / 4] >= 10) {   /* split unit: two 16-bit entries */
                        int hh[2] = { (best.mord[u / 4] - 10) / 3, (best.mord[u / 4] - 10) % 3 };
                        for (int k = 0; k < 2; k++)
                            if (img[u0 + u + 2 * k] | img[u0 + u + 2 * k + 1]) {
                                add_fill(tmpl, u0 + u + 2 * k, 2, sib(base, hh[k], g_nbase), best.src + u + 2 * k);
                                total += 2;
                            }
                        continue;
                    }
                    long v = u + 4;
                    while (v < mlen && best.mord[v / 4] == best.mord[u / 4] &&
                           (img[u0 + v] | img[u0 + v + 1] | img[u0 + v + 2] | img[u0 + v + 3])) v += 4;
                    add_fill(tmpl, u0 + u, v - u, sib(base, best.mord[u / 4], g_nbase), best.src + u);
                    total += v - u;
                    u = v - 4;
                }
                USE(base, best.src - u0);
                p = u0 + mlen;
            }
        }
    }
#undef USE
    free(anchor_at);
    return total;
}

static int cmp_fill(const void *a, const void *b)
{
    const Fill *x = a, *y = b;
    if (x->kind != y->kind) return x->kind - y->kind;
    if (x->rom_off != y->rom_off) return x->rom_off < y->rom_off ? -1 : 1;
    return x->dst < y->dst ? -1 : x->dst > y->dst;
}

static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }

int main(int argc, char **argv)
{
    long minrun = 16;
    int a = 1;
    for (;;) {
        if (a + 1 < argc && !strcmp(argv[a], "-m")) { minrun = atol(argv[a + 1]); a += 2; }
        else if (a + 1 < argc && !strcmp(argv[a], "-d")) { g_debug = argv[a + 1]; a += 2; }
        else break;
    }
    if (argc - a != 6) {
        fprintf(stderr, "usage: romfill [-m MINRUN] ROM IN.wasm LINK.map OUT.wasm OUT.romfill REPORT.tsv\n");
        return 2;
    }
    if (minrun < 2 * BLK - 1) minrun = 2 * BLK - 1;
    long romn, wn, mn;
    uint8_t *rom = slurp(argv[a], &romn);
    uint8_t *wasm = slurp(argv[a + 1], &wn);
    char *map = (char *)slurp(argv[a + 2], &mn);
    parse_wasm(wasm, wn);
    build_image(wasm);
    parse_map(map);

    add_seg(0, 0, 0, rom, romn);
    for (long o = 0; o + 2 < romn; o++) {
        if (rom[o] != 0x11 || rom[o + 1] != 0x72) continue;
        uint8_t *d;
        long n = try_inflate(rom + o + 2, romn - o - 2, &d);
        if (n >= 0) add_seg((uint32_t)o, 1, 0, d, n);
    }
    int nbase = nsegs;
    for (int i = 0; i < nbase; i++) {
        add_seg(segs[i].rom_off, segs[i].kind, 1, swapped(segs[i].p, segs[i].n, 4), segs[i].n);
        add_seg(segs[i].rom_off, segs[i].kind, 2, swapped(segs[i].p, segs[i].n, 2), segs[i].n);
    }
    long total = 0;
    for (int i = 0; i < nsegs; i++) total += segs[i].n;
    uint64_t cap = 1;
    while (cap < (uint64_t)(total / BLK) * 2 + 16) cap <<= 1;
    tab = malloc(cap * sizeof(Ent));
    if (!tab) { fprintf(stderr, "romfill: out of memory for the index\n"); return 2; }
    for (uint64_t i = 0; i < cap; i++) tab[i].seg = -1;
    tabmask = cap - 1;
    for (int i = 0; i < nsegs; i++) index_seg(i);
    HBK = 1;
    for (int i = 0; i < BLK; i++) HBK *= HB;

    g_nbase = nbase;
    mark_pointers();
    long imn = (long)(img_hi - img_lo);
    uint8_t *tmpl = malloc(imn);
    memcpy(tmpl, img, imn);
    long tot_nz = 0, tot_fill = 0, nsel = 0;
    for (int si = 0; si < nsyms; si++) {
        Sym *s = &syms[si];
        long fo = mem2img(s->addr, s->size);
        if (fo < 0 || s->size <= 0) continue;   /* bss or outside the data image */
        nsel++;
        const uint8_t *p = img + fo;
        long n = s->size, nz = 0, filled = 0;
        for (long i = 0; i < n; i++) nz += p[i] != 0;
        long o = 0, last_end = 0;
        uint64_t h = n >= BLK ? hblock(p) : 0;
        while (o + BLK <= n) {
            uint64_t i = (h * 0x9E3779B97F4A7C15ULL) & tabmask;
            long best_len = 0, best_s = 0, best_so = 0;
            int best_seg = -1, tries = 0;
            for (; tab[i].seg >= 0 && tries < 256; i = (i + 1) & tabmask) {
                if (tab[i].h != h) continue;
                tries++;
                Seg *g = &segs[tab[i].seg];
                long so = tab[i].off;
                if (memcmp(g->p + so, p + o, BLK)) continue;
                long b = 0;
                while (o - b - 1 >= last_end && so - b - 1 >= 0 && p[o - b - 1] == g->p[so - b - 1]) b++;
                long e = BLK;
                while (o + e < n && so + e < g->n && p[o + e] == g->p[so + e]) e++;
                if (b + e > best_len) { best_len = b + e; best_s = o - b; best_so = so - b; best_seg = tab[i].seg; }
            }
            int nonzero = 0;
            for (long k = 0; best_seg >= 0 && k < best_len; k++) if (p[best_s + k]) { nonzero = 1; break; }
            if (best_seg >= 0 && best_len >= minrun && nonzero) {
                if (nfills == capfills) { capfills = capfills ? capfills * 2 : 4096; fills = realloc(fills, capfills * sizeof(Fill)); }
                Seg *g = &segs[best_seg];
                fills[nfills++] = (Fill){ (uint32_t)(s->addr + best_s), (uint32_t)best_len, g->rom_off,
                                          (uint32_t)best_so, (uint8_t)g->kind, (uint8_t)g->order };
                memset(tmpl + fo + best_s, 0, best_len);
                filled += best_len;
                o = best_s + best_len;
                last_end = o;
                if (o + BLK > n) break;
                h = hblock(p + o);
                continue;
            }
            if (o + BLK >= n) break;
            h = h * HB + (uint64_t)(p[o + BLK] + 1) - (uint64_t)(p[o] + 1) * HBK;
            o++;
        }
        tot_nz += nz;
        tot_fill += filled;
    }
    long npass1 = nfills, fill1 = tot_fill;
    long fill2 = pass2(tmpl, minrun);
    tot_fill += fill2;

    FILE *rep = fopen(argv[a + 5], "w");
    if (!rep) { fprintf(stderr, "romfill: cannot write %s\n", argv[a + 5]); return 2; }
    fprintf(rep, "symbol\taddr\tsize\tnonzero\tfilled\tpointers\tresidual\n");
    long tot_ptr = 0, tot_res = 0;
    for (int si = 0; si < nsyms; si++) {
        Sym *sy = &syms[si];
        long fo = mem2img(sy->addr, sy->size);
        if (fo < 0 || sy->size <= 0) continue;
        long nz = 0, fl = 0, pt = 0, res = 0;
        for (long i = fo; i < fo + sy->size; i++) {
            if (!img[i]) continue;
            nz++;
            if (!tmpl[i]) fl++;
            else if (ptrmask[i]) pt++;
            else res++;
        }
        tot_ptr += pt;
        tot_res += res;
        fprintf(rep, "%s\t%#llx\t%ld\t%ld\t%ld\t%ld\t%ld\n", sy->name, (unsigned long long)sy->addr, sy->size, nz, fl, pt, res);
    }
    fclose(rep);
    printf("romfill: pass 1 (runs >= %ld) %ld entries / %ld bytes, pass 2 (anchored pieces) %ld entries / %ld bytes; "
           "left: %ld pointer-like bytes, %ld other non-zero bytes\n", minrun, npass1, fill1, nfills - npass1, fill2,
           tot_ptr, tot_res);

    /* group the entries by source stream (the page inflates each RZ stream once) */
    qsort(fills, nfills, sizeof(Fill), cmp_fill);
    /* self-check: apply the table to the template, compare with the original */
    uint8_t *sim = malloc(imn);
    memcpy(sim, tmpl, imn);
    uLong crc_fill = crc32(0, NULL, 0);
    for (long i = 0; i < nfills; i++) {
        Fill *f = &fills[i];
        Seg *g = NULL;
        for (int k = 0; k < nsegs; k++)
            if (segs[k].rom_off == f->rom_off && segs[k].kind == f->kind && segs[k].order == f->order) { g = &segs[k]; break; }
        long fo = mem2img(f->dst, f->len);
        if (!g || fo < 0 || f->src_off + f->len > (uint64_t)g->n) { fprintf(stderr, "romfill: bad entry %ld\n", i); return 1; }
        memcpy(sim + fo, g->p + f->src_off, f->len);
        crc_fill = crc32(crc_fill, sim + fo, f->len);
    }
    if (memcmp(sim, img, imn)) { fprintf(stderr, "romfill: SELF-CHECK FAILED (filled image != original)\n"); return 1; }
    uLong crc_syms = crc32(0, NULL, 0);
    int nkept = 0;
    for (int si = 0; si < nsyms; si++) {
        long fo = mem2img(syms[si].addr, syms[si].size);
        if (fo < 0 || syms[si].size <= 0) continue;
        crc_syms = crc32(crc_syms, img + fo, syms[si].size);
        nkept++;
    }

    /* the template wasm: zero the filled bytes where they are present in the file */
    for (long i = 0; i < imn; i++)
        if (tmpl[i] != img[i]) {
            if (img_foff[i] < 0) { fprintf(stderr, "romfill: filled byte outside the file\n"); return 1; }
            wasm[img_foff[i]] = 0;
        }
    FILE *wf = fopen(argv[a + 3], "wb");
    if (!wf || fwrite(wasm, 1, wn, wf) != (size_t)wn) { fprintf(stderr, "romfill: cannot write %s\n", argv[a + 3]); return 2; }
    fclose(wf);
    FILE *tf = fopen(argv[a + 4], "wb");
    if (!tf) { fprintf(stderr, "romfill: cannot write %s\n", argv[a + 4]); return 2; }
    put32(tf, 0x46524547); /* 'GERF' */
    put32(tf, 1);
    put32(tf, (uint32_t)nfills);
    put32(tf, (uint32_t)crc_fill);
    put32(tf, (uint32_t)crc_syms);
    put32(tf, (uint32_t)nkept);
    for (int si = 0; si < nsyms; si++) {
        long fo = mem2img(syms[si].addr, syms[si].size);
        if (fo < 0 || syms[si].size <= 0) continue;
        put32(tf, (uint32_t)syms[si].addr);
        put32(tf, (uint32_t)syms[si].size);
    }
    for (long i = 0; i < nfills; i++) {
        Fill *f = &fills[i];
        put32(tf, f->dst); put32(tf, f->len); put32(tf, f->rom_off); put32(tf, f->src_off);
        uint8_t b[4] = { f->kind, f->order, 0, 0 };
        fwrite(b, 1, 4, tf);
    }
    fclose(tf);
    printf("romfill: %ld symbols (%ld non-zero bytes), %ld fill entries, %ld bytes zeroed + filled; "
           "self-check OK; crc fill %08lx syms %08lx\n", nsel, tot_nz, nfills, tot_fill,
           (unsigned long)crc_fill, (unsigned long)crc_syms);
    return 0;
}
