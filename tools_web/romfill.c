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

static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }

int main(int argc, char **argv)
{
    long minrun = 16;
    int a = 1;
    if (a + 1 < argc && !strcmp(argv[a], "-m")) { minrun = atol(argv[a + 1]); a += 2; }
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

    long imn = (long)(img_hi - img_lo);
    uint8_t *tmpl = malloc(imn);
    memcpy(tmpl, img, imn);
    FILE *rep = fopen(argv[a + 5], "w");
    if (!rep) { fprintf(stderr, "romfill: cannot write %s\n", argv[a + 5]); return 2; }
    fprintf(rep, "symbol\taddr\tsize\tnonzero\tfilled\n");
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
        fprintf(rep, "%s\t%#llx\t%ld\t%ld\t%ld\n", s->name, (unsigned long long)s->addr, n, nz, filled);
    }
    fclose(rep);

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
