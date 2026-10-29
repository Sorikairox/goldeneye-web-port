/*
 * romscan — find ROM-derived byte runs in files that are about to be shipped (deploy gate,
 * docs/web/ROMDATA.md). Driver: scripts/web/romscan.sh.
 *
 *   romscan [-m MINRUN] [-v] [-t hits.tsv] ROM FILE...
 *
 * Corpus: the ROM as is, every 0x1172 ("RZ", raw deflate) stream found anywhere in it,
 * decompressed (models, setups, bg rooms, the compressed game data segment, ...), and each of
 * those again with every 32-bit word byte-swapped and with every 16-bit half byte-swapped (the
 * PC build compiles data tables as little-endian values). Any run of >= MINRUN (default 64)
 * bytes of a scanned file that also occurs in the corpus is a hit, unless the run is "boring"
 * (fewer than 8 distinct byte values: zero fill, 0xFF fill, short repeating patterns).
 *
 * Method: every 32-byte block of the corpus at a 32-aligned offset goes into a hash table; the
 * scanned file is hashed at every offset with a rolling hash. A shared run of >= 63 bytes always
 * contains one aligned corpus block, so no run >= 64 is missed. Hits are extended both ways.
 * Exit status 1 when there is any hit.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define BLK 32

typedef struct {
    char name[96];
    uint8_t *p;
    long n;
} Seg;

static Seg *segs;
static int nsegs, capsegs;

static void add_seg(const char *name, uint8_t *p, long n)
{
    if (nsegs == capsegs) {
        capsegs = capsegs ? capsegs * 2 : 256;
        segs = realloc(segs, capsegs * sizeof(Seg));
    }
    snprintf(segs[nsegs].name, sizeof segs[nsegs].name, "%s", name);
    segs[nsegs].p = p;
    segs[nsegs].n = n;
    nsegs++;
}

static uint8_t *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "romscan: cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n ? n : 1);
    if (n && fread(b, 1, n, f) != (size_t)n) { fprintf(stderr, "romscan: read %s\n", path); exit(2); }
    fclose(f);
    *len = n;
    return b;
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
    if (r != Z_STREAM_END || len < 64) { free(b); return -1; }
    *out = b;
    return len;
}

/* polynomial hash of BLK bytes; rolling: h = h*B + in - out*B^BLK */
static const uint64_t HB = 0x100000001b3ULL;
static uint64_t HBK;   /* HB^BLK */

static uint64_t hblock(const uint8_t *p)
{
    uint64_t h = 0;
    for (int i = 0; i < BLK; i++) h = h * HB + p[i] + 1;
    return h;
}

typedef struct { uint64_t h; int seg; uint32_t off; } Ent;
static Ent *tab;
static uint64_t tabmask;

static int distinct(const uint8_t *p, long n)
{
    uint8_t seen[256] = {0};
    int d = 0;
    for (long i = 0; i < n; i++) if (!seen[p[i]]++) d++;
    return d;
}

static void index_seg(int si)
{
    Seg *s = &segs[si];
    for (long o = 0; o + BLK <= s->n; o += BLK) {
        if (distinct(s->p + o, BLK) < 4) continue;
        uint64_t h = hblock(s->p + o);
        uint64_t i = (h * 0x9E3779B97F4A7C15ULL) & tabmask;
        while (tab[i].seg >= 0) {
            if (tab[i].h == h && !memcmp(segs[tab[i].seg].p + tab[i].off, s->p + o, BLK)) goto dup;
            i = (i + 1) & tabmask;
        }
        tab[i].h = h; tab[i].seg = si; tab[i].off = (uint32_t)o;
    dup:;
    }
}

static uint8_t *swapped(const uint8_t *p, long n, int w)
{
    uint8_t *q = malloc(n);
    memcpy(q, p, n);
    for (long i = 0; i + w <= n; i += w)
        for (int k = 0; k < w; k++) q[i + k] = p[i + w - 1 - k];
    return q;
}

int main(int argc, char **argv)
{
    long minrun = 64;
    int verbose = 0;
    const char *tsv = NULL;
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-m") && a + 1 < argc) minrun = atol(argv[++a]);
        else if (!strcmp(argv[a], "-v")) verbose = 1;
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) tsv = argv[++a];
    }
    if (argc - a < 2) { fprintf(stderr, "usage: romscan [-m MINRUN] [-v] [-t hits.tsv] ROM FILE...\n"); return 2; }
    if (minrun < 2 * BLK - 1) minrun = 2 * BLK - 1;
    long romn;
    uint8_t *rom = slurp(argv[a], &romn);
    add_seg("rom", rom, romn);
    long nrz = 0, rzbytes = 0;
    for (long o = 0; o + 2 < romn; o++) {
        if (rom[o] != 0x11 || rom[o + 1] != 0x72) continue;
        uint8_t *d;
        long n = try_inflate(rom + o + 2, romn - o - 2, &d);
        if (n < 0) continue;
        char nm[96];
        snprintf(nm, sizeof nm, "rz@%06lx", o);
        add_seg(nm, d, n);
        nrz++;
        rzbytes += n;
    }
    int nbase = nsegs;
    for (int i = 0; i < nbase; i++) {
        char nm[96];
        snprintf(nm, sizeof nm, "%s.sw32", segs[i].name);
        add_seg(nm, swapped(segs[i].p, segs[i].n, 4), segs[i].n);
        snprintf(nm, sizeof nm, "%s.sw16", segs[i].name);
        add_seg(nm, swapped(segs[i].p, segs[i].n, 2), segs[i].n);
    }
    long total = 0;
    for (int i = 0; i < nsegs; i++) total += segs[i].n;
    uint64_t cap = 1;
    while (cap < (uint64_t)(total / BLK) * 2 + 16) cap <<= 1;
    tab = malloc(cap * sizeof(Ent));
    for (uint64_t i = 0; i < cap; i++) tab[i].seg = -1;
    tabmask = cap - 1;
    for (int i = 0; i < nsegs; i++) index_seg(i);
    HBK = 1;
    for (int i = 0; i < BLK; i++) HBK *= HB;
    fprintf(stderr, "romscan: corpus = ROM %ld bytes + %ld RZ streams (%ld bytes), x3 byte orders\n",
            romn, nrz, rzbytes);

    FILE *tf = tsv ? fopen(tsv, "w") : NULL;
    if (tf) fprintf(tf, "file\toffset\tlength\tsource\tsource_offset\n");
    long allhits = 0, allbytes = 0;
    for (int fi = a + 1; fi < argc; fi++) {
        long n;
        uint8_t *p = slurp(argv[fi], &n);
        long hits = 0, bytes = 0;
        long o = 0;
        uint64_t h = 0;
        if (n >= BLK) h = hblock(p);
        long last_end = 0;
        while (o + BLK <= n) {
            uint64_t i = (h * 0x9E3779B97F4A7C15ULL) & tabmask;
            int found = 0;
            long best_s = 0, best_len = 0, best_seg = 0, best_so = 0;
            for (; tab[i].seg >= 0; i = (i + 1) & tabmask) {
                if (tab[i].h != h) continue;
                Seg *s = &segs[tab[i].seg];
                long so = tab[i].off;
                if (memcmp(s->p + so, p + o, BLK)) continue;
                long b = 0;
                while (o - b - 1 >= last_end && so - b - 1 >= 0 && p[o - b - 1] == s->p[so - b - 1]) b++;
                long e = BLK;
                while (o + e < n && so + e < s->n && p[o + e] == s->p[so + e]) e++;
                if (b + e > best_len) {
                    best_len = b + e; best_s = o - b; best_seg = tab[i].seg; best_so = so - b; found = 1;
                }
            }
            if (found && best_len >= minrun && distinct(p + best_s, best_len) >= 8) {
                hits++;
                bytes += best_len;
                if (tf) fprintf(tf, "%s\t%ld\t%ld\t%s\t%ld\n", argv[fi], best_s, best_len, segs[best_seg].name, best_so);
                if (verbose)
                    printf("  %s +0x%lx len %ld  <- %s +0x%lx\n", argv[fi], best_s, best_len, segs[best_seg].name, best_so);
                long next = best_s + best_len;
                last_end = next;
                if (next + BLK > n) break;
                o = next;
                h = hblock(p + o);
                continue;
            }
            if (o + BLK >= n) break;
            h = h * HB + (uint64_t)(p[o + BLK] + 1) - (uint64_t)(p[o] + 1) * HBK;
            o++;
        }
        printf("%s: %ld runs >= %ld bytes, %ld bytes total\n", argv[fi], hits, minrun, bytes);
        allhits += hits;
        allbytes += bytes;
        free(p);
    }
    if (tf) fclose(tf);
    printf("TOTAL: %ld runs, %ld bytes\n", allhits, allbytes);
    return allhits ? 1 : 0;
}
