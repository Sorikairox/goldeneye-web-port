/*
 * geconv.c — C port of the PC sidecar converters (see geconv.h).
 *
 *   d43  tools_pc/d43_emit.py      model files (C*Z / G*Z / P*Z)  -> pcmodels.bin + manifest.csv
 *   d69  tools_pc/d69_emit.py      bg/<n>.seg + Tbg_<n>_stanZ       -> pccg.bin + manifest.csv
 *   d88  tools_pc/d88_emit.py      Usetup*Z / Ump_setup*Z         -> appended to pccg (--regen)
 *        tools_pc/d88_propdefs.py  (the propDefs record stream inside the setup files)
 *
 * The port follows the Python line by line, including its orderings (dict insertion order,
 * stable sorts, last-write-wins maps), because the gate is byte-identical output. Function and
 * variable names match the Python ones so the two can be read side by side. The Python's
 * diagnostics-only parts are not ported: d43's round-trip validate() and its buffer-fit
 * cross-checks (P_est / chain checks) never change the output; reads outside a file, which make
 * the Python raise, are errors here too.
 */
#ifdef GECONV_HOST
#include <zlib.h>
#else
#include "realzlib.h"
#endif
#include "geconv.h"

/* No libc headers: the decomp's include/ shadows stdlib.h/string.h for port TUs (D38). LP64. */
extern void *malloc(unsigned long);
extern void *calloc(unsigned long, unsigned long);
extern void *realloc(void *, unsigned long);
extern void free(void *);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);
extern int memcmp(const void *, const void *, unsigned long);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, unsigned long);
extern unsigned long strlen(const char *);
extern int snprintf(char *, unsigned long, const char *, ...);

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;
typedef long long s64;

#define MASK 0xFFFFFF

/* ------------------------------------------------------------------ errors ---- */
static char g_err[512];
static int g_failed;

static void fail(const char *name, const char *what, s64 a)
{
    if (!g_failed)
        snprintf(g_err, sizeof(g_err), "%s: %s (%#llx)", name ? name : "?", what,
                 (unsigned long long)a);
    g_failed = 1;
}

/* ------------------------------------------------------------- growable --- */
static void *xrealloc(void *p, unsigned long n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fail("geconv", "out of memory", (s64)n);
        free(p);
    }
    return q;
}

static int blob_reserve(GeconvBlob *b, unsigned long n)
{
    if (n <= b->cap)
        return 0;
    unsigned long nc = b->cap ? b->cap : 4096;
    while (nc < n)
        nc *= 2;
    u8 *d = (u8 *)realloc(b->data, nc);
    if (!d) {
        fail("geconv", "out of memory", (s64)nc);
        return -1;
    }
    b->data = d;
    b->cap = nc;
    return 0;
}

static int blob_append(GeconvBlob *b, const void *p, unsigned long n)
{
    if (blob_reserve(b, b->len + n))
        return -1;
    if (p)
        memcpy(b->data + b->len, p, n);
    else
        memset(b->data + b->len, 0, n);
    b->len += n;
    return 0;
}

static void blob_free(GeconvBlob *b)
{
    free(b->data);
    b->data = 0;
    b->len = b->cap = 0;
}

/* int64 vector */
typedef struct {
    s64 *v;
    long n, cap;
} Vec;

static void vpush(Vec *a, s64 x)
{
    if (a->n == a->cap) {
        long nc = a->cap ? a->cap * 2 : 64;
        s64 *nv = (s64 *)xrealloc(a->v, (unsigned long)nc * sizeof(s64));
        if (!nv) {
            a->v = 0;
            a->n = a->cap = 0;
            return;
        }
        a->v = nv;
        a->cap = nc;
    }
    a->v[a->n++] = x;
}
static void vfree(Vec *a) { free(a->v); a->v = 0; a->n = a->cap = 0; }

/* u32 key -> s64 value hash map (open addressing). */
typedef struct {
    u32 *keys;
    s64 *vals;
    u8 *used;
    long cap, n;
} Map;

static void mfree(Map *m) { free(m->keys); free(m->vals); free(m->used); memset(m, 0, sizeof(*m)); }

static long mslot(const Map *m, u32 k)
{
    u32 h = k * 2654435761u;
    long i = (long)(h & (u32)(m->cap - 1));
    while (m->used[i] && m->keys[i] != k)
        i = (i + 1) & (m->cap - 1);
    return i;
}

static void mput(Map *m, u32 k, s64 v);
static void mgrow(Map *m)
{
    Map o = *m;
    long nc = o.cap ? o.cap * 2 : 256;
    m->keys = (u32 *)calloc((unsigned long)nc, sizeof(u32));
    m->vals = (s64 *)calloc((unsigned long)nc, sizeof(s64));
    m->used = (u8 *)calloc((unsigned long)nc, 1);
    m->cap = nc;
    m->n = 0;
    if (!m->keys || !m->vals || !m->used) {
        fail("geconv", "out of memory", nc);
        return;
    }
    for (long i = 0; i < o.cap; i++)
        if (o.used[i])
            mput(m, o.keys[i], o.vals[i]);
    mfree(&o);
}
static void mput(Map *m, u32 k, s64 v)
{
    if ((m->n + 1) * 2 > m->cap)
        mgrow(m);
    if (g_failed && !m->cap)
        return;
    long i = mslot(m, k);
    if (!m->used[i]) {
        m->used[i] = 1;
        m->keys[i] = k;
        m->n++;
    }
    m->vals[i] = v;
}
static int mget(const Map *m, u32 k, s64 *v)
{
    if (!m->cap)
        return 0;
    long i = mslot(m, k);
    if (!m->used[i])
        return 0;
    if (v)
        *v = m->vals[i];
    return 1;
}

static int cmp_s64(const void *a, const void *b)
{
    s64 x = *(const s64 *)a, y = *(const s64 *)b;
    return x < y ? -1 : x > y;
}

/* Stable merge sort (Python's sort is stable; some orders here depend on it). */
static void msort(void *base, long n, unsigned long sz, int (*cmp)(const void *, const void *))
{
    if (n < 2)
        return;
    u8 *a = (u8 *)base;
    u8 *tmp = (u8 *)malloc((unsigned long)n * sz);
    if (!tmp) {
        fail("geconv", "out of memory", n);
        return;
    }
    for (long w = 1; w < n; w *= 2) {
        for (long lo = 0; lo < n; lo += 2 * w) {
            long mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            long i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (cmp(a + j * sz, a + i * sz) < 0)
                    memcpy(tmp + (k++) * sz, a + (j++) * sz, sz);
                else
                    memcpy(tmp + (k++) * sz, a + (i++) * sz, sz);
            }
            while (i < mid) memcpy(tmp + (k++) * sz, a + (i++) * sz, sz);
            while (j < hi) memcpy(tmp + (k++) * sz, a + (j++) * sz, sz);
        }
        memcpy(a, tmp, (unsigned long)n * sz);
    }
    free(tmp);
}

/* sorted unique copy of a vector */
static void vsortuniq(Vec *a)
{
    msort(a->v, a->n, sizeof(s64), cmp_s64);
    long j = 0;
    for (long i = 0; i < a->n; i++)
        if (j == 0 || a->v[j - 1] != a->v[i])
            a->v[j++] = a->v[i];
    a->n = j;
}

/* ------------------------------------------------------------ byte access -- */
typedef struct {
    const u8 *p;
    long n;
    const char *name;
} Src;

static u32 rd8(const Src *s, s64 o)
{
    if (o < 0 || o + 1 > s->n) { fail(s->name, "read past the end of the file", o); return 0; }
    return s->p[o];
}
static u32 rdu16(const Src *s, s64 o)
{
    if (o < 0 || o + 2 > s->n) { fail(s->name, "read past the end of the file", o); return 0; }
    return (u32)s->p[o] << 8 | s->p[o + 1];
}
static s32 rds16(const Src *s, s64 o) { return (s32)(short)rdu16(s, o); }
static u32 rd32(const Src *s, s64 o)
{
    if (o < 0 || o + 4 > s->n) { fail(s->name, "read past the end of the file", o); return 0; }
    return (u32)s->p[o] << 24 | (u32)s->p[o + 1] << 16 | (u32)s->p[o + 2] << 8 | s->p[o + 3];
}
static u32 rd32o(const Src *s, s64 o) { return rd32(s, o) & MASK; }
static s32 rds32(const Src *s, s64 o) { return (s32)rd32(s, o); }

typedef struct {
    u8 *p;
    long n;
    const char *name;
} Dst;

static u8 *wp(Dst *d, s64 o, long len)
{
    static u8 sink[16];
    if (o < 0 || o + len > d->n) {
        fail(d->name, "write past the end of the output", o);
        return sink;
    }
    return d->p + o;
}
static void wle16(Dst *d, s64 o, u32 v) { u8 *q = wp(d, o, 2); q[0] = (u8)v; q[1] = (u8)(v >> 8); }
static void wle32(Dst *d, s64 o, u32 v)
{
    u8 *q = wp(d, o, 4);
    q[0] = (u8)v; q[1] = (u8)(v >> 8); q[2] = (u8)(v >> 16); q[3] = (u8)(v >> 24);
}
static void wzero(Dst *d, s64 o, long len) { memset(wp(d, o, len), 0, (unsigned long)len); }
static void wcopy(Dst *d, s64 o, const Src *s, s64 so, long len)
{
    if (len <= 0)
        return;
    if (so < 0 || so + len > s->n) { fail(s->name, "copy past the end of the file", so); return; }
    memcpy(wp(d, o, len), s->p + so, (unsigned long)len);
}
/* 4 source bytes, reversed (BE word -> LE) */
static void wswap32(Dst *d, s64 o, const Src *s, s64 so) { wle32(d, o, rd32(s, so)); }

/* ------------------------------------------------------------------- zlib --- */
static int inflate_raw(const u8 *in, long inlen, u8 **out, long *outlen, const char *name)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK) { fail(name, "inflateInit2 failed", 0); return -1; }
    long cap = inlen * 4 + 4096, len = 0;
    u8 *buf = (u8 *)malloc((unsigned long)cap);
    if (!buf) { inflateEnd(&zs); fail(name, "out of memory", cap); return -1; }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)inlen;
    for (;;) {
        if (len == cap) {
            cap *= 2;
            u8 *nb = (u8 *)realloc(buf, (unsigned long)cap);
            if (!nb) { free(buf); inflateEnd(&zs); fail(name, "out of memory", cap); return -1; }
            buf = nb;
        }
        zs.next_out = buf + len;
        zs.avail_out = (uInt)(cap - len);
        int r = inflate(&zs, Z_NO_FLUSH);
        len = cap - zs.avail_out;
        if (r == Z_STREAM_END)
            break;
        if (r != Z_OK && !(r == Z_BUF_ERROR && zs.avail_out == 0)) {
            free(buf);
            inflateEnd(&zs);
            fail(name, "decompress failed", r);
            return -1;
        }
    }
    inflateEnd(&zs);
    *out = buf;
    *outlen = len;
    return 0;
}

/* b"\x11\x72" + raw deflate level 6 (Python zlib.compressobj(6, DEFLATED, -15)). */
static int rz_compress(const u8 *in, long inlen, GeconvBlob *out, const char *name)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        fail(name, "deflateInit2 failed", 0);
        return -1;
    }
    unsigned long bound = deflateBound(&zs, (uLong)inlen) + 2;
    out->len = 0;
    if (blob_reserve(out, bound)) { deflateEnd(&zs); return -1; }
    out->data[0] = 0x11;
    out->data[1] = 0x72;
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)inlen;
    zs.next_out = out->data + 2;
    zs.avail_out = (uInt)(bound - 2);
    int r = deflate(&zs, Z_FINISH);
    if (r != Z_STREAM_END) { deflateEnd(&zs); fail(name, "deflate failed", r); return -1; }
    out->len = 2 + zs.total_out;
    deflateEnd(&zs);
    return 0;
}

/* --------------------------------------------------------- input tables ---- */
typedef struct {
    char base[64];
    u32 off, size;
} FlRow;

typedef struct {
    const GeconvInput *in;
    FlRow *fl;
    long nfl;
    char (*names)[64];
    long nnames;
} Tables;

static Tables T;

/* csv.reader rows: base = r[2].rsplit("/",1)[-1] minus ".bin" -> (int(r[0]), int(r[1])) */
static int parse_filelist(void)
{
    const char *s = T.in->filelist, *e = s + T.in->filelistlen;
    long cap = 0;
    while (s < e) {
        const char *le = s;
        while (le < e && *le != '\n')
            le++;
        const char *f[5] = {0};
        long fl[5] = {0};
        int nf = 0;
        const char *p = s, *lend = le;
        if (lend > s && lend[-1] == '\r')
            lend--;
        while (nf < 5) {
            const char *q = p;
            while (q < lend && *q != ',')
                q++;
            f[nf] = p;
            fl[nf] = q - p;
            nf++;
            if (q >= lend)
                break;
            p = q + 1;
        }
        s = le + 1;
        if (nf < 3 || fl[2] == 0)
            continue;
        const char *b = f[2], *be = f[2] + fl[2];
        for (const char *q = f[2]; q < be; q++)
            if (*q == '/')
                b = q + 1;
        long bl = be - b;
        if (bl >= 4 && !strncmp(be - 4, ".bin", 4))
            bl -= 4;
        if (bl <= 0 || bl >= 64)
            continue;
        if (T.nfl == cap) {
            cap = cap ? cap * 2 : 1024;
            FlRow *nr = (FlRow *)xrealloc(T.fl, (unsigned long)cap * sizeof(FlRow));
            if (!nr) { T.fl = 0; return -1; }
            T.fl = nr;
        }
        FlRow *r = &T.fl[T.nfl++];
        memcpy(r->base, b, (unsigned long)bl);
        r->base[bl] = 0;
        u32 v0 = 0, v1 = 0;
        for (long i = 0; i < fl[0]; i++) v0 = v0 * 10 + (u32)(f[0][i] - '0');
        for (long i = 0; i < fl[1]; i++) v1 = v1 * 10 + (u32)(f[1][i] - '0');
        r->off = v0;
        r->size = v1;
    }
    return 0;
}

/* dict semantics: the last row with a given base wins */
static const FlRow *fl_get(const char *base)
{
    for (long i = T.nfl - 1; i >= 0; i--)
        if (!strcmp(T.fl[i].base, base))
            return &T.fl[i];
    return 0;
}

static int is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

/* re.search(r'\{\s*\w+\s*,\s*"([^"]+)"', line) per line */
static int parse_restable(void)
{
    const char *s = T.in->restable, *e = s + T.in->restablelen;
    long cap = 0;
    while (s < e) {
        const char *le = s;
        while (le < e && *le != '\n')
            le++;
        for (const char *p = s; p < le; p++) {
            if (*p != '{')
                continue;
            const char *q = p + 1;
            while (q < le && is_space(*q)) q++;
            const char *w = q;
            while (q < le && is_word(*q)) q++;
            if (q == w) continue;
            while (q < le && is_space(*q)) q++;
            if (q >= le || *q != ',') continue;
            q++;
            while (q < le && is_space(*q)) q++;
            if (q >= le || *q != '"') continue;
            q++;
            const char *n0 = q;
            while (q < le && *q != '"') q++;
            if (q >= le || q == n0) continue;
            long nl = q - n0;
            if (nl >= 64) { fail("restable", "name too long", nl); return -1; }
            if (T.nnames == cap) {
                cap = cap ? cap * 2 : 1024;
                void *nn = xrealloc(T.names, (unsigned long)cap * 64);
                if (!nn) { T.names = 0; return -1; }
                T.names = (char (*)[64])nn;
            }
            memcpy(T.names[T.nnames], n0, (unsigned long)nl);
            T.names[T.nnames][nl] = 0;
            T.nnames++;
            break;
        }
        s = le + 1;
    }
    return 0;
}

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static int ends(const char *s, const char *p)
{
    unsigned long a = strlen(s), b = strlen(p);
    return a >= b && !strcmp(s + a - b, p);
}
static int contains(const char *s, const char *p)
{
    unsigned long b = strlen(p);
    for (; *s; s++)
        if (!strncmp(s, p, b))
            return 1;
    return 0;
}

/* (name, name[1:], name[:-1], name[1:-1]) */
static void cand(const char *name, int k, char *out)
{
    unsigned long n = strlen(name);
    unsigned long a = (k == 1 || k == 3) ? 1 : 0;
    unsigned long b = (k == 2 || k == 3) ? (n ? n - 1 : 0) : n;
    if (b < a) b = a;
    memcpy(out, name + a, b - a);
    out[b - a] = 0;
}

/* ------------------------------------------------------------- emit stream -- */
typedef struct {
    GeconvBlob *bin, *man;
    long cur;
} Emit;

static int emit_file(Emit *em, const char *name, const u8 *data, long len)
{
    long start = (em->cur + 15) & ~15L;
    if (start > em->cur) {
        if (blob_append(em->bin, 0, (unsigned long)(start - em->cur)))
            return -1;
        em->cur = start;
    }
    char line[160];
    int n = snprintf(line, sizeof(line), "%s,%ld,%ld\r\n", name, em->cur, len);
    if (blob_append(em->man, line, (unsigned long)n) || blob_append(em->bin, data, (unsigned long)len))
        return -1;
    em->cur += len;
    return 0;
}

/* ============================================================== d43 (models) */
static const int N64_REC[25] = { -1, 0x1C, 0x1C, 0x1C, 0x14, -1, -1, -1, 0x10, 0x24, 0x1C, -1, 0x28,
                                 0x20, -1, 0x1C, -1, 0x20, 0x08, -1, -1, 0x14, 0x10, 0x02, 0x20 };
static const int PC_REC[25] = { -1, 24, 40, 40, 40, -1, -1, -1, 24, 48, 28, -1, 48,
                                48, -1, 28, -1, -1, 16, -1, -1, 20, 32, 2, 64 };

typedef struct { s64 old, size, nw; } Region;

static int cmp_region(const void *a, const void *b)
{
    const Region *x = (const Region *)a, *y = (const Region *)b;
    if (x->old != y->old) return x->old < y->old ? -1 : 1;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    if (x->nw != y->nw) return x->nw < y->nw ? -1 : 1;
    return 0;
}

typedef struct {
    Region *v;
    long n, cap;
} RVec;

static void rpush(RVec *r, s64 old, s64 size, s64 nw)
{
    if (r->n == r->cap) {
        long nc = r->cap ? r->cap * 2 : 256;
        Region *nv = (Region *)xrealloc(r->v, (unsigned long)nc * sizeof(Region));
        if (!nv) { r->v = 0; r->n = r->cap = 0; return; }
        r->v = nv;
        r->cap = nc;
    }
    r->v[r->n].old = old;
    r->v[r->n].size = size;
    r->v[r->n].nw = nw;
    r->n++;
}

typedef struct {
    int op;
    u32 data, parent, next, prev, child;
} Node;

typedef struct {
    Src src;
    long D;
    Node *nodes;
    long nnodes, capnodes;
    Map nodeidx;          /* n64 offset -> index into nodes */
    RVec regions;
} D43;

static Node *d43_node(D43 *m, u32 off)
{
    s64 i;
    if (!mget(&m->nodeidx, off, &i)) {
        fail(m->src.name, "node not in the node map", off);
        return 0;
    }
    return &m->nodes[i];
}

static void d43_build_nodes(D43 *m, u32 R0)
{
    Vec stack = {0};
    vpush(&stack, R0);
    while (stack.n && !g_failed) {
        u32 o = (u32)stack.v[--stack.n];
        if (mget(&m->nodeidx, o, 0) || (long)o >= m->D)
            continue;
        if (m->nnodes == m->capnodes) {
            long nc = m->capnodes ? m->capnodes * 2 : 256;
            Node *nn = (Node *)xrealloc(m->nodes, (unsigned long)nc * sizeof(Node));
            if (!nn) { m->nodes = 0; break; }
            m->nodes = nn;
            m->capnodes = nc;
        }
        Node *n = &m->nodes[m->nnodes];
        n->op = (int)(rdu16(&m->src, o) & 0xff);
        n->data = rd32o(&m->src, o + 4);
        n->parent = rd32o(&m->src, o + 8);
        n->next = rd32o(&m->src, o + 0xC);
        n->prev = rd32o(&m->src, o + 0x10);
        n->child = rd32o(&m->src, o + 0x14);
        mput(&m->nodeidx, o, m->nnodes);
        m->nnodes++;
        if (n->op == 8) {
            u32 q = rd32o(&m->src, n->data + 8);
            if (q) vpush(&stack, q);
        } else if (n->op == 18) {
            u32 q = rd32o(&m->src, n->data + 0);
            if (q) vpush(&stack, q);
        } else if (n->child) {
            vpush(&stack, n->child);
        }
        if (n->next)
            vpush(&stack, n->next);
    }
    vfree(&stack);
}

static void d43_gdl_of(D43 *m, const Node *n, u32 *p, u32 *s)
{
    *p = *s = 0;
    if (n->op == 4 || n->op == 24) {
        *p = rd32o(&m->src, n->data + 0);
        *s = rd32o(&m->src, n->data + 4);
    } else if (n->op == 22) {
        *p = rd32o(&m->src, n->data + 8);
    }
}

/* EXACT modelIterateDisplayLists GDL visit order (mutable walk) */
static void d43_visit_seq(D43 *m, u32 R0, Vec *seq)
{
    s64 node = R0, prev_node = -1;
    u32 prev_gdl = 0;
    long guard = 0;
    while (node && guard < 200000 && !g_failed) {
        guard++;
        Node *n = d43_node(m, (u32)node);
        if (!n) return;
        u32 gdl = 0;
        if (n->op == 4 || n->op == 22 || n->op == 24) {
            u32 p, s;
            d43_gdl_of(m, n, &p, &s);
            if (node != prev_node)
                gdl = p;
            else if (s && s != prev_gdl)
                gdl = s;
        } else if (n->op == 8) {
            n->child = rd32o(&m->src, n->data + 8);
        } else if (n->op == 18) {
            n->child = rd32o(&m->src, n->data + 0);
        } else if (n->op == 9) {
            u32 node1 = rd32o(&m->src, n->data + 0x18), node2 = rd32o(&m->src, n->data + 0x1C);
            if (node1) {
                n->child = node1;
                Node *a = d43_node(m, node1);
                if (!a) return;
                a->prev = 0;
                u32 loop = node1;
                for (;;) {
                    Node *l = d43_node(m, loop);
                    if (!l) return;
                    if (!(l->next && l->next != node2)) break;
                    loop = l->next;
                }
                d43_node(m, loop)->next = node2;
                if (node2) {
                    d43_node(m, node2)->prev = loop;
                    loop = node2;
                    for (;;) {
                        Node *l = d43_node(m, loop);
                        if (!l) return;
                        if (!(l->next && l->next != node1)) break;
                        loop = l->next;
                    }
                    d43_node(m, loop)->next = 0;
                }
            } else {
                n->child = node2;
                if (node2) {
                    Node *b = d43_node(m, node2);
                    if (!b) return;
                    b->prev = 0;
                }
            }
        }
        if (gdl) {
            vpush(seq, gdl);
            prev_node = node;
            prev_gdl = gdl;
            continue;
        }
        prev_node = -1;
        if (n->child) {
            node = n->child;
        } else {
            while (node) {
                Node *x = d43_node(m, (u32)node);
                if (!x) return;
                u32 nn = x->next;
                if (nn) { node = nn; break; }
                node = x->parent;
            }
        }
    }
}

/* d43_convert walk(): next-first DFS stack -> placement order (n64 offsets) */
static void d43_placement_order(D43 *m, u32 R0, Vec *out)
{
    Map seen = {0};
    Vec stack = {0};
    vpush(&stack, R0);
    while (stack.n && !g_failed) {
        u32 o = (u32)stack.v[--stack.n];
        if (mget(&seen, o, 0) || (long)o >= m->D)
            continue;
        mput(&seen, o, 1);
        Node *n = d43_node(m, o);
        if (!n) break;
        vpush(out, o);
        if (n->op == 8) {
            u32 a = rd32o(&m->src, n->data + 8);
            if (a) vpush(&stack, a);
        } else if (n->op == 18) {
            u32 c = rd32o(&m->src, n->data + 0);
            if (c) vpush(&stack, c);
        } else if (n->child) {
            vpush(&stack, n->child);
        }
        if (n->next)
            vpush(&stack, n->next);
    }
    mfree(&seen);
    vfree(&stack);
}

static s64 d43_gdl_end(D43 *m, s64 g)
{
    s64 o = g;
    while (o + 8 <= m->D) {
        if ((rd32(&m->src, o) >> 24) == 0xB8)
            return o + 8;
        o += 8;
    }
    return m->D;
}

/* remap(off): regions sorted by (old,size,new); last region with old <= off must contain it */
static int d43_remap(D43 *m, s64 off, s64 *out)
{
    if (off == 0) { *out = 0; return 1; }
    long lo = 0, hi = m->regions.n;   /* bisect_right on old */
    while (lo < hi) {
        long mid = (lo + hi) / 2;
        if (off < m->regions.v[mid].old) hi = mid; else lo = mid + 1;
    }
    long i = lo - 1;
    if (i < 0) return 0;
    Region *r = &m->regions.v[i];
    if (r->old <= off && off < r->old + r->size) {
        *out = r->nw + (off - r->old);
        return 1;
    }
    return 0;
}

static void d43_put_ptr(D43 *m, Dst *b, s64 o, u32 old_off)
{
    if (!old_off)
        return;
    s64 n;
    if (!d43_remap(m, old_off, &n)) {
        fail(m->src.name, "ptr not in region map", old_off);
        n = 0;
    }
    wle32(b, o, 0x05000000u | ((u32)n & MASK));
    wzero(b, o + 4, 4);
}

static void d43_put16(D43 *m, Dst *b, s64 o, s64 so) { wle16(b, o, rdu16(&m->src, so)); }
static void d43_putf(D43 *m, Dst *b, s64 o, s64 so) { wswap32(b, o, &m->src, so); }

/* ordered unique list helper */
static void push_unique(Vec *v, Map *set, s64 x)
{
    if (mget(set, (u32)x, 0))
        return;
    mput(set, (u32)x, 1);
    vpush(v, x);
}

static int d43_process(const char *name, GeconvBlob *comp)
{
    char key[64] = "", c[64];
    const FlRow *row = 0;
    for (int k = 0; k < 4 && !row; k++) {
        cand(name, k, c);
        row = fl_get(c);
        if (row) memcpy(key, c, sizeof(c));
    }
    if (!row) { fail(name, "no filelist row", 0); return -1; }
    if ((unsigned long)row->off + row->size > T.in->romlen || row->size < 2) {
        fail(name, "filelist row outside the ROM", row->off);
        return -1;
    }
    u8 *srcbuf = 0;
    long D = 0;
    if (inflate_raw(T.in->rom + row->off + 2, (long)row->size - 2, &srcbuf, &D, name))
        return -1;
    int NS = -1, NT = -1, found = 0;
    for (int k = 0; k < 4 && !found; k++) {
        cand(key, k, c);
        found = T.in->header(T.in->headerctx, c, &NS, &NT);
    }
    if (!found) { free(srcbuf); fail(name, "no header data", 0); return -1; }

    D43 M;
    memset(&M, 0, sizeof(M));
    D43 *m = &M;
    m->src.p = srcbuf; m->src.n = D; m->src.name = name;
    m->D = D;
    u32 R0 = (u32)(4 * NS + 12 * NT);

    Vec seq = {0}, placed = {0}, vtx_vo = {0}, vtx_nv = {0}, zero_vtx = {0}, ref_gdls = {0},
        unvisited = {0}, all_gdls = {0}, obj_offs = {0}, blob_targets = {0}, pu_off = {0}, pu_nv = {0},
        blobs_old = {0}, blobs_size = {0}, blobs_new = {0};
    Map rec_newoff = {0}, node_newoff = {0}, is_collision = {0}, pu_index = {0}, refset = {0},
        visited = {0}, gdl_newoff = {0};
    u8 *buf = 0;
    int rc = -1;

    d43_build_nodes(m, R0);
    if (g_failed) goto out;
    if (!m->nnodes) { fail(name, "empty tree", 0); goto out; }
    d43_visit_seq(m, R0, &seq);
    d43_placement_order(m, R0, &placed);
    if (g_failed) goto out;

    /* ---- layout pass ---- */
    s64 dstpos = 0;
#define ADD_REGION(old, osz) (rpush(&m->regions, (old), (osz), dstpos), dstpos += (osz))
    for (int i = 0; i < NS; i++) ADD_REGION(4 * i, 8);
    for (int i = 0; i < NT; i++) ADD_REGION(4 * NS + 12 * i, 12);

    for (long pi = 0; pi < placed.n && !g_failed; pi++) {
        u32 no = (u32)placed.v[pi];
        int op = d43_node(m, no)->op;
        u32 data = rd32o(&m->src, no + 4);
        ADD_REGION(no, 48);
        mput(&node_newoff, no, dstpos - 48);
        if (op < 0 || op > 24 || N64_REC[op] < 0) { fail(name, "unknown opcode", op); goto out; }
        if (PC_REC[op] < 0) { fail(name, "opcode without a PC record size", op); goto out; }
        int psz = PC_REC[op];
        mput(&rec_newoff, data, dstpos);
        ADD_REGION(data, psz);
        if (op == 4) {
            u32 nv = rdu16(&m->src, data + 0x10), vo = rd32o(&m->src, data + 0xC);
            if (vo) {
                if (!nv) vpush(&zero_vtx, vo);
                else { vpush(&vtx_vo, vo); vpush(&vtx_nv, nv); ADD_REGION(vo, 16 * (s64)nv); }
            }
        } else if (op == 24) {
            s32 nv = rds16(&m->src, data + 0xC), ncv = rds16(&m->src, data + 0xE);
            u32 vo = rd32o(&m->src, data + 8), cvo = rd32o(&m->src, data + 0x10);
            u32 puo = rd32o(&m->src, data + 0x14);
            if (nv && vo) { vpush(&vtx_vo, vo); vpush(&vtx_nv, nv); ADD_REGION(vo, 16 * (s64)nv); }
            if (ncv && cvo) {
                mput(&is_collision, cvo, 1);
                vpush(&vtx_vo, cvo); vpush(&vtx_nv, ncv); ADD_REGION(cvo, 16 * (s64)ncv);
            }
            if (nv && puo) {
                ADD_REGION(puo, 2 * (s64)nv);
                s64 idx;
                if (mget(&pu_index, puo, &idx)) pu_nv.v[idx] = nv;
                else { mput(&pu_index, puo, pu_off.n); vpush(&pu_off, puo); vpush(&pu_nv, nv); }
            }
        } else if (op == 22) {
            s32 nv = rds32(&m->src, data);
            u32 vo = rd32o(&m->src, data + 4);
            if (nv && vo) { vpush(&vtx_vo, vo); vpush(&vtx_nv, 4 * (s64)nv); ADD_REGION(vo, 16 * 4 * (s64)nv); }
        }
    }
    if (g_failed) goto out;

    for (long pi = 0; pi < placed.n; pi++) {
        u32 no = (u32)placed.v[pi];
        int op = d43_node(m, no)->op;
        u32 data = rd32o(&m->src, no + 4);
        if (op == 4 || op == 24) {
            for (int off = 0; off <= 4; off += 4) {
                u32 q = rd32o(&m->src, data + off);
                if (q) push_unique(&ref_gdls, &refset, q);
            }
        } else if (op == 22) {
            u32 q = rd32o(&m->src, data + 8);
            if (q) push_unique(&ref_gdls, &refset, q);
        }
    }
    for (long i = 0; i < seq.n; i++) mput(&visited, (u32)seq.v[i], 1);
    for (long i = 0; i < ref_gdls.n; i++)
        if (!mget(&visited, (u32)ref_gdls.v[i], 0)) vpush(&unvisited, ref_gdls.v[i]);
    msort(unvisited.v, unvisited.n, sizeof(s64), cmp_s64);
    for (long i = 0; i < unvisited.n; i++) vpush(&all_gdls, unvisited.v[i]);
    for (long i = 0; i < seq.n; i++) vpush(&all_gdls, seq.v[i]);

    /* obj_offs_now (only "min over offsets > vo" is needed, order irrelevant) */
    for (long i = 0; i < placed.n; i++) vpush(&obj_offs, placed.v[i]);
    for (long i = 0; i < rec_newoff.cap; i++) if (rec_newoff.used[i]) vpush(&obj_offs, rec_newoff.keys[i]);
    for (long i = 0; i < vtx_vo.n; i++) vpush(&obj_offs, vtx_vo.v[i]);
    for (long i = 0; i < all_gdls.n; i++) vpush(&obj_offs, all_gdls.v[i]);
    for (long zi = 0; zi < zero_vtx.n; zi++) {
        s64 vo = zero_vtx.v[zi], nxt = D;
        for (long i = 0; i < obj_offs.n; i++)
            if (obj_offs.v[i] > vo && obj_offs.v[i] < nxt) nxt = obj_offs.v[i];
        s64 sz = nxt - vo;
        if (sz <= 0 || sz % 16) { fail(name, "zero-vtx array bad size", vo); goto out; }
        vpush(&vtx_vo, vo); vpush(&vtx_nv, sz / 16);
        ADD_REGION(vo, sz);
    }

    /* embedded image blobs (G_SETTIMG seg-5 targets) */
    for (long gi = 0; gi < all_gdls.n; gi++) {
        s64 o = all_gdls.v[gi];
        while (o + 8 <= D) {
            u32 w0 = rd32(&m->src, o), cc = w0 >> 24;
            if (cc == 0xB8) break;
            if (cc == 0xFD && (rd32(&m->src, o + 4) >> 24) == 5) vpush(&blob_targets, rd32(&m->src, o + 4) & MASK);
            o += 8;
        }
    }
    vsortuniq(&blob_targets);
    {
        long nreg = m->regions.n;
        s64 mo = -1, me = -1;
        for (long ti = 0; ti < blob_targets.n; ti++) {
            s64 t = blob_targets.v[ti], end = D;
            for (long i = 0; i < nreg; i++)
                if (m->regions.v[i].old > t && m->regions.v[i].old < end) end = m->regions.v[i].old;
            if (mo >= 0 && t <= me) {
                if (end > me) me = end;
            } else {
                if (mo >= 0) { vpush(&blobs_old, mo); vpush(&blobs_size, me - mo); }
                mo = t; me = end;
            }
        }
        if (mo >= 0) { vpush(&blobs_old, mo); vpush(&blobs_size, me - mo); }
        for (long i = 0; i < blobs_old.n; i++) {
            vpush(&blobs_new, dstpos);
            ADD_REGION(blobs_old.v[i], blobs_size.v[i]);
        }
    }
#undef ADD_REGION

    /* GDLs last, 16B slots: unvisited first, then visit order */
    for (long gi = 0; gi < all_gdls.n; gi++) {
        s64 g = all_gdls.v[gi];
        s64 end = d43_gdl_end(m, g);
        s64 nslots = (end - g) / 8;
        if ((end - g) % 8) { fail(name, "GDL not 8B aligned end", g); goto out; }
        mput(&gdl_newoff, (u32)g, dstpos);
        rpush(&m->regions, g, 16 * nslots, dstpos);
        dstpos += 16 * nslots;
    }
    if (g_failed) goto out;
    s64 D_PC = dstpos;

    msort(m->regions.v, m->regions.n, sizeof(Region), cmp_region);

    /* ---- emit pass ---- */
    buf = (u8 *)calloc((unsigned long)(D_PC ? D_PC : 1), 1);
    if (!buf) { fail(name, "out of memory", D_PC); goto out; }
    Dst B = { buf, (long)D_PC, name };
    Dst *b = &B;
    for (int i = 0; i < NS; i++) d43_put_ptr(m, b, 8 * i, rd32o(&m->src, 4 * i));
    for (int i = 0; i < NT; i++) {
        s64 doff = 4 * NS + 12 * i, npos = 8 * NS + 12 * i;
        u32 tid = rd32(&m->src, doff);
        wle32(b, npos, tid);
        wcopy(b, npos + 4, &m->src, doff + 4, 8);
        s64 t;
        if ((tid >> 24) == 5 && !d43_remap(m, tid & MASK, &t)) fail(name, "texconfig seg-5 target not in a blob region", tid);
    }

    for (long pi = 0; pi < placed.n && !g_failed; pi++) {
        u32 no = (u32)placed.v[pi];
        int op = d43_node(m, no)->op;
        s64 npos, r;
        mget(&node_newoff, no, &npos);
        d43_put16(m, b, npos, no);
        d43_put_ptr(m, b, npos + 8, rd32o(&m->src, no + 4));
        d43_put_ptr(m, b, npos + 16, rd32o(&m->src, no + 8));
        d43_put_ptr(m, b, npos + 24, rd32o(&m->src, no + 0xC));
        d43_put_ptr(m, b, npos + 32, rd32o(&m->src, no + 0x10));
        d43_put_ptr(m, b, npos + 40, rd32o(&m->src, no + 0x14));
        u32 d = rd32o(&m->src, no + 4);
        mget(&rec_newoff, d, &r);
        s64 gp;
        switch (op) {
        case 1:
            d43_put16(m, b, r, d); d43_put16(m, b, r + 2, d + 2);
            d43_put_ptr(m, b, r + 8, rd32o(&m->src, d + 4));
            d43_put16(m, b, r + 16, d + 8); d43_put16(m, b, r + 18, d + 0xA);
            d43_put16(m, b, r + 20, d + 0xC);
            break;
        case 2: case 3:
            for (int i = 0; i < 3; i++) d43_putf(m, b, r + 4 * i, d + 4 * i);
            d43_put16(m, b, r + 12, d + 0xC);
            for (int i = 0; i < 3; i++) d43_put16(m, b, r + 14 + 2 * i, d + 0xE + 2 * i);
            d43_put_ptr(m, b, r + 24, rd32o(&m->src, d + 0x14));
            d43_putf(m, b, r + 32, d + 0x18);
            break;
        case 4: {
            u32 p = rd32o(&m->src, d), s = rd32o(&m->src, d + 4);
            if (p) { mget(&gdl_newoff, p, &gp); wle32(b, r, 0x05000000u | ((u32)gp & MASK)); }
            if (s) { mget(&gdl_newoff, s, &gp); wle32(b, r + 8, 0x05000000u | ((u32)gp & MASK)); }
            d43_put_ptr(m, b, r + 24, rd32o(&m->src, d + 0xC));
            d43_put16(m, b, r + 32, d + 0x10);
            *wp(b, r + 34, 1) = (u8)rd8(&m->src, d + 0x12);
            break;
        }
        case 8:
            d43_putf(m, b, r, d); d43_putf(m, b, r + 4, d + 4);
            d43_put_ptr(m, b, r + 8, rd32o(&m->src, d + 8));
            d43_put16(m, b, r + 16, d + 0xC);
            break;
        case 9:
            for (int i = 0; i < 3; i++) d43_putf(m, b, r + 4 * i, d + 4 * i);
            for (int i = 0; i < 3; i++) d43_putf(m, b, r + 12 + 4 * i, d + 0xC + 4 * i);
            d43_put_ptr(m, b, r + 24, rd32o(&m->src, d + 0x18));
            d43_put_ptr(m, b, r + 32, rd32o(&m->src, d + 0x1C));
            d43_put16(m, b, r + 42, d + 0x22);
            break;
        case 10:
            wle32(b, r, rd32(&m->src, d));
            for (int i = 0; i < 6; i++) d43_putf(m, b, r + 4 + 4 * i, d + 4 + 4 * i);
            break;
        case 12:
            for (int i = 0; i < 3; i++) d43_putf(m, b, r + 4 * i, d + 4 * i);
            for (int i = 0; i < 3; i++) d43_putf(m, b, r + 12 + 4 * i, d + 0xC + 4 * i);
            d43_put_ptr(m, b, r + 24, rd32o(&m->src, d + 0x18));
            d43_putf(m, b, r + 32, d + 0x1C);
            d43_put16(m, b, r + 36, d + 0x20);
            break;
        case 13:
            for (int i = 0; i < 2; i++) d43_putf(m, b, r + 4 * i, d + 4 * i);
            for (int i = 0; i < 2; i++) d43_putf(m, b, r + 8 + 4 * i, d + 8 + 4 * i);
            d43_put_ptr(m, b, r + 16, rd32o(&m->src, d + 0x10));
            d43_put_ptr(m, b, r + 24, rd32o(&m->src, d + 0x14));
            d43_putf(m, b, r + 32, d + 0x18);
            break;
        case 15:
            for (int i = 0; i < 3; i++) d43_putf(m, b, r + 4 * i, d + 4 * i);
            for (int i = 0; i < 3; i++) d43_putf(m, b, r + 12 + 4 * i, d + 0xC + 4 * i);
            d43_putf(m, b, r + 24, d + 0x18);
            break;
        case 18:
            d43_put_ptr(m, b, r, rd32o(&m->src, d));
            d43_put16(m, b, r + 8, d + 4);
            break;
        case 21:
            for (int i = 0; i < 3; i++) d43_putf(m, b, r + 4 * i, d + 4 * i);
            d43_put16(m, b, r + 12, d + 0xC); d43_put16(m, b, r + 14, d + 0xE);
            d43_putf(m, b, r + 16, d + 0x10);
            break;
        case 22: {
            wle32(b, r, rd32(&m->src, d));
            d43_put_ptr(m, b, r + 8, rd32o(&m->src, d + 4));
            u32 p = rd32o(&m->src, d + 8);
            if (p) { mget(&gdl_newoff, p, &gp); wle32(b, r + 16, 0x05000000u | ((u32)gp & MASK)); }
            break;
        }
        case 23:
            d43_put16(m, b, r, d);
            break;
        case 24: {
            u32 p = rd32o(&m->src, d), s = rd32o(&m->src, d + 4);
            if (p) { mget(&gdl_newoff, p, &gp); wle32(b, r, 0x05000000u | ((u32)gp & MASK)); }
            if (s) { mget(&gdl_newoff, s, &gp); wle32(b, r + 8, 0x05000000u | ((u32)gp & MASK)); }
            d43_put_ptr(m, b, r + 16, rd32o(&m->src, d + 8));
            d43_put16(m, b, r + 24, d + 0xC); d43_put16(m, b, r + 26, d + 0xE);
            d43_put_ptr(m, b, r + 32, rd32o(&m->src, d + 0x10));
            d43_put_ptr(m, b, r + 40, rd32o(&m->src, d + 0x14));
            d43_put16(m, b, r + 48, d + 0x18); d43_put16(m, b, r + 50, d + 0x1A);
            break;
        }
        }
    }

    /* vertex arrays */
    for (long vi = 0; vi < vtx_vo.n && !g_failed; vi++) {
        s64 vo = vtx_vo.v[vi], nv = vtx_nv.v[vi], npos;
        if (!d43_remap(m, vo, &npos)) { fail(name, "vtx array not in region map", vo); break; }
        if (mget(&is_collision, (u32)vo, 0)) {
            for (s64 i = 0; i < nv; i++) {
                s64 o = vo + 16 * i, q = npos + 16 * i;
                d43_put16(m, b, q, o); d43_put16(m, b, q + 2, o + 2);
                d43_put16(m, b, q + 4, o + 4); d43_put16(m, b, q + 6, o + 6);
                u32 v = rd32(&m->src, o + 8);
                if (v == 0) {
                } else if ((v >> 24) == 5) {
                    s64 n;
                    if (!d43_remap(m, v & MASK, &n)) { fail(name, "LinkedTo not in map", v); n = 0; }
                    wle32(b, q + 8, 0x05000000u | ((u32)n & MASK));
                } else {
                    fail(name, "LinkedTo neither null nor seg-5", v);
                }
                d43_put16(m, b, q + 12, o + 0xC); d43_put16(m, b, q + 14, o + 0xE);
            }
        } else {
            for (s64 i = 0; i < nv; i++) {
                s64 o = vo + 16 * i, q = npos + 16 * i;
                for (int k = 0; k < 12; k += 2) d43_put16(m, b, q + k, o + k);
                wcopy(b, q + 12, &m->src, o + 0xC, 4);
            }
        }
    }

    /* D120: PointUsage[] s16 chains */
    for (long i = 0; i < pu_off.n; i++) {
        s64 pnpos;
        if (!d43_remap(m, pu_off.v[i], &pnpos)) { fail(name, "PointUsage not in region map", pu_off.v[i]); continue; }
        for (s64 k = 0; k < pu_nv.v[i]; k++) d43_put16(m, b, pnpos + 2 * k, pu_off.v[i] + 2 * k);
    }

    /* GDLs, 16B LE slots */
    for (long gi = 0; gi < all_gdls.n && !g_failed; gi++) {
        s64 g = all_gdls.v[gi], end = d43_gdl_end(m, g), npos;
        mget(&gdl_newoff, (u32)g, &npos);
        long j = 0;
        s64 o = g;
        while (o + 8 <= end) {
            u32 w0 = rd32(&m->src, o), w1 = rd32(&m->src, o + 4);
            s64 q = npos + 16 * j;
            u32 cc = w0 >> 24;
            if ((cc == 0x04 || cc == 0xFD) && (w1 >> 24) == 5) {
                s64 n;
                if (!d43_remap(m, w1 & MASK, &n)) { fail(name, "GDL w1 not in map", w1); n = 0; }
                w1 = 0x05000000u | ((u32)n & MASK);
            }
            wle32(b, q, w0); wle32(b, q + 4, 0);
            wle32(b, q + 8, w1); wle32(b, q + 12, 0);
            if (cc == 0xB8) break;
            j++;
            o += 8;
        }
    }

    /* embedded image blobs: raw pixel copy */
    for (long i = 0; i < blobs_old.n; i++) wcopy(b, blobs_new.v[i], &m->src, blobs_old.v[i], (long)blobs_size.v[i]);

    if (g_failed) goto out;
    rc = rz_compress(buf, (long)D_PC, comp, name);

out:
    free(buf);
    free(srcbuf);
    free(m->nodes);
    free(m->regions.v);
    mfree(&m->nodeidx);
    Vec *vs[] = { &seq, &placed, &vtx_vo, &vtx_nv, &zero_vtx, &ref_gdls, &unvisited, &all_gdls, &obj_offs,
                  &blob_targets, &pu_off, &pu_nv, &blobs_old, &blobs_size, &blobs_new };
    for (unsigned i = 0; i < sizeof(vs) / sizeof(vs[0]); i++) vfree(vs[i]);
    Map *ms[] = { &rec_newoff, &node_newoff, &is_collision, &pu_index, &refset, &visited, &gdl_newoff };
    for (unsigned i = 0; i < sizeof(ms) / sizeof(ms[0]); i++) mfree(ms[i]);
    return g_failed ? -1 : rc;
}

/* ============================================================ d69 (bg, stan) */
typedef struct { s64 start, size; int is_record; } BlobRegion;

static u8 *d69_convert_seg(const char *name, const Src *s, long *outlen)
{
    long D = s->n;
    if (D < 0x40) { fail(name, "too small", D); return 0; }
    u32 h0 = rd32(s, 0);
    u32 h1 = rd32(s, 4) & MASK, h2 = rd32(s, 8) & MASK, h3 = rd32(s, 12) & MASK, h4 = rd32(s, 16) & MASK;
    if (h0 != 0) { fail(name, "header word0 != 0", h0); return 0; }
    if (h4 != 0) { fail(name, "header word4 != 0 (f32 array path not implemented)", h4); return 0; }
    if (h2 == 0 || h2 <= h1) { fail(name, "portal offset <= room offset", h2); return 0; }
    u32 room_end = (h3 && h3 > h1 && h3 < h2) ? h3 : h2;
    if ((room_end - h1) % 24 != 0) { fail(name, "room table extent not a multiple of 24", room_end - h1); return 0; }

    s64 o = h2;
    long n_portals = 0;
    Vec targets = {0};
    int term = 0;
    while (o + 8 <= D) {
        u32 v = rd32(s, o) & MASK;
        if (v == 0) { term = 1; break; }
        vpush(&targets, v);
        n_portals++;
        o += 8;
    }
    if (!term) { vfree(&targets); fail(name, "portal table terminator not found before EOF", o); return 0; }
    s64 portal_table_end_n64 = o + 8;
    s64 portal_delta = 8 * (n_portals + 1);

    const int ENVIRONMENTDATA_ALT = 100;
    if (h3 && room_end == h3) {
        s64 o2 = h3;
        while (o2 < h2) {
            u32 etype = rd8(s, o2);
            s32 data = rds32(s, o2 + 4);
            if ((int)etype == ENVIRONMENTDATA_ALT && data) vpush(&targets, (u32)data & MASK);
            o2 += 8;
            if (etype == 0) break;
        }
    }
    vsortuniq(&targets);
    BlobRegion *br = (BlobRegion *)malloc(sizeof(BlobRegion) * (unsigned long)(2 * targets.n + 2));
    long nbr = 0;
    s64 cursor = portal_table_end_n64;
    for (long i = 0; br && i < targets.n; i++) {
        s64 t = targets.v[i];
        if (t < cursor) { fail(name, "portal target overlaps previous region", t); goto bad; }
        if (t > cursor) { br[nbr].start = cursor; br[nbr].size = t - cursor; br[nbr].is_record = 0; nbr++; }
        u32 numPoints = rd8(s, t);
        if (g_failed) goto bad;
        if (numPoints == 0 || numPoints > 20) { fail(name, "portal target numPoints implausible", t); goto bad; }
        s64 recsize = 4 + 12 * numPoints;
        if (t + recsize > D) { fail(name, "portal target overruns EOF", t); goto bad; }
        br[nbr].start = t; br[nbr].size = recsize; br[nbr].is_record = 1; nbr++;
        cursor = t + recsize;
    }
    if (!br) { fail(name, "out of memory", 0); goto bad; }
    if (cursor < D) { br[nbr].start = cursor; br[nbr].size = D - cursor; br[nbr].is_record = 0; nbr++; }

    long outn = (long)(D + portal_delta);
    u8 *outp = (u8 *)calloc((unsigned long)outn, 1);
    if (!outp) { fail(name, "out of memory", outn); goto bad; }
    Dst O = { outp, outn, name };
    Dst *out = &O;
    wle32(out, 0, h0);
    wle32(out, 4, 0x0F000000u | h1);
    wle32(out, 8, 0x0F000000u | h2);
    wle32(out, 12, h3 ? (0x0F000000u | h3) : 0);
    wle32(out, 16, 0);
    wcopy(out, 20, s, 20, 0x40 - 20);

    for (s64 o2 = h1; o2 < room_end; o2 += 24) {
        for (int w = 0; w < 3; w++) {
            u32 v = rd32(s, o2 + 4 * w) & MASK;
            u32 nv = v ? (0x0F000000u | ((u32)(v + portal_delta) & MASK)) : 0;
            wle32(out, o2 + 4 * w, nv);
        }
        for (int w = 0; w < 3; w++) wswap32(out, o2 + 12 + 4 * w, s, o2 + 12 + 4 * w);
    }

    if (h3 && room_end == h3) {
        s64 o2 = h3;
        while (o2 < h2) {
            u32 etype = rd8(s, o2);
            *wp(out, o2, 1) = (u8)etype;
            wcopy(out, o2 + 1, s, o2 + 1, 3);
            s32 data = rds32(s, o2 + 4);
            if ((int)etype == ENVIRONMENTDATA_ALT) data = data ? (s32)(data + portal_delta) : data;
            wle32(out, o2 + 4, (u32)data);
            o2 += 8;
            if (etype == 0) break;
        }
    }

    {
        s64 o2 = h2, pc_pos = h2;
        long idx = 0;
        while (idx <= n_portals) {
            u32 off_val = rd32(s, o2) & MASK;
            if (off_val) wle32(out, pc_pos, 0x0F000000u | ((u32)(off_val + portal_delta) & MASK));
            else wle32(out, pc_pos, 0);
            wle32(out, pc_pos + 4, 0);
            wcopy(out, pc_pos + 8, s, o2 + 4, 4);
            wle32(out, pc_pos + 12, 0);
            o2 += 8;
            pc_pos += 16;
            idx++;
        }
    }

    for (long i = 0; i < nbr; i++) {
        s64 dst = br[i].start + portal_delta;
        if (!br[i].is_record) { wcopy(out, dst, s, br[i].start, (long)br[i].size); continue; }
        wcopy(out, dst, s, br[i].start, 4);
        for (s64 k = 0; k < (br[i].size - 4) / 4; k++) wswap32(out, dst + 4 + 4 * k, s, br[i].start + 4 + 4 * k);
    }
    free(br);
    vfree(&targets);
    if (g_failed) { free(outp); return 0; }
    *outlen = outn;
    return outp;
bad:
    free(br);
    vfree(&targets);
    return 0;
}

static const int TILESIZES[12] = { 0x20, 0x20, 0x20, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0 };

static u8 *d69_convert_stan(const char *name, const Src *s, long *outlen)
{
    long D = s->n;
    if (D < 8) { fail(name, "too small", D); return 0; }
    s32 stanfile = rds32(s, 0);
    Vec offs = {0};
    s64 o = 4;
    int term = 0;
    while (o + 4 <= D) {
        u32 v = rd32(s, o);
        vpush(&offs, v);
        o += 4;
        if (v == 0) { term = 1; break; }
    }
    if (!term) { vfree(&offs); fail(name, "room-offset array terminator not found", o); return 0; }
    long N = offs.n - 1;
    s64 old_array_end = o, array_delta = 4 * (N + 2), new_tiledata_start = old_array_end + array_delta;
    long outn = (long)(D + array_delta);
    u8 *outp = (u8 *)calloc((unsigned long)outn, 1);
    if (!outp) { vfree(&offs); fail(name, "out of memory", outn); return 0; }
    Dst O = { outp, outn, name };
    Dst *out = &O;
    wle32(out, 0, (u32)stanfile);
    wle32(out, 4, 0);
    s64 p = 8;
    for (long i = 0; i < offs.n; i++) {
        u32 v = (u32)offs.v[i];
        wle32(out, p, v ? (u32)(v + array_delta) : 0);
        wle32(out, p + 4, 0);
        p += 8;
    }
    vfree(&offs);
    if (p != new_tiledata_start) { fail(name, "stan layout mismatch", p); free(outp); return 0; }

    s64 src_o = old_array_end, dst_o = new_tiledata_start;
    term = 0;
    while (src_o + 4 <= D && !g_failed) {
        if (rd32(s, src_o) == 0) { wle32(out, dst_o, 0); term = 1; break; }
        u8 *q = wp(out, dst_o, 4);
        q[0] = (u8)rd8(s, src_o + 1); q[1] = (u8)rd8(s, src_o); q[2] = (u8)rd8(s, src_o + 2); q[3] = (u8)rd8(s, src_o + 3);
        u32 midhalf = rdu16(s, src_o + 4), tailhalf = rdu16(s, src_o + 6);
        wle16(out, dst_o + 4, midhalf);
        wle16(out, dst_o + 6, tailhalf);
        u32 pointCount = (tailhalf >> 12) & 0xF;
        int sz = pointCount < 12 ? TILESIZES[pointCount] : 0;
        if (sz == 0) { fail(name, "bad pointCount", pointCount); break; }
        if (src_o + sz > D) { fail(name, "tile overruns EOF", src_o); break; }
        for (u32 i = 0; i < pointCount; i++) {
            s64 po = src_o + 8 + 8 * i, qo = dst_o + 8 + 8 * i;
            for (int k = 0; k < 8; k += 2) wle16(out, qo + k, rdu16(s, po + k));
        }
        src_o += sz;
        dst_o += sz;
    }
    if (!term && !g_failed) fail(name, "ran off EOF without an all-zero EOF sentinel", src_o);
    if (g_failed) { free(outp); return 0; }
    s64 tail_src_o = src_o + 4, tail_dst_o = dst_o + 4;
    if (tail_src_o < D) wcopy(out, tail_dst_o, s, tail_src_o, (long)(D - tail_src_o));
    if (g_failed) { free(outp); return 0; }
    *outlen = outn;
    return outp;
}

/* =========================================================== d88 (setups) */
/* ---- d88_propdefs.py ---- */
static int PROPDEF_N64_WORDS(int t)
{
    switch (t) {
    case 1: return 64; case 2: return 2; case 3: return 32; case 4: return 33; case 5: return 32;
    case 6: return 59; case 7: return 33; case 8: return 34; case 9: return 7; case 10: return 64;
    case 11: return 149; case 12: return 32; case 13: return 54; case 14: return 3; case 17: return 32;
    case 18: return 3; case 19: return 4; case 20: return 45; case 21: return 34; case 22: return 4;
    case 23: return 4; case 24: return 1; case 25: case 26: case 27: case 28: case 29: return 2;
    case 30: return 4; case 31: return 1; case 32: return 4; case 33: return 5; case 34: return 3;
    case 35: return 4; case 36: return 32; case 37: return 10; case 38: return 4; case 39: return 44;
    case 40: return 45; case 42: case 43: return 32; case 44: return 5; case 45: return 56;
    case 46: return 7; case 47: return 37; case 48: return 1;
    }
    return -1;
}
static int PROPDEF_PC_BYTES(int t)
{
    switch (t) {
    case 1: return 296; case 2: return 8;
    case 3: case 5: case 12: case 17: case 36: case 42: case 43: return 144;
    case 4: return 152; case 6: return 272; case 7: return 152; case 8: return 160; case 9: return 32;
    case 10: return 288; case 11: return 664; case 13: return 248; case 14: return 32; case 18: return 12;
    case 19: return 32; case 20: return 200; case 21: return 152; case 22: return 24; case 23: return 16;
    case 24: return 4; case 25: case 26: case 27: case 28: case 29: return 8; case 30: return 24;
    case 31: return 4; case 32: return 24; case 33: return 24; case 34: return 12; case 35: return 24;
    case 37: return 48; case 38: return 32; case 39: return 208; case 40: return 208; case 44: return 40;
    case 45: return 248; case 46: return 28; case 47: return 168; case 48: return 4;
    }
    return -1;
}
#define OBJ_N64_WORDS 32
#define OBJ_PC_BYTES 144

/* word transforms on 4 source bytes w[0..3] -> out */
static void w_bswap32(u8 *o, const u8 *w) { o[0] = w[3]; o[1] = w[2]; o[2] = w[1]; o[3] = w[0]; }
static void w_hdr(u8 *o, const u8 *w) { o[0] = w[1]; o[1] = w[0]; o[2] = w[2]; o[3] = w[3]; }
static void w_hh(u8 *o, const u8 *w) { o[0] = w[1]; o[1] = w[0]; o[2] = w[3]; o[3] = w[2]; }

static int pd_in(int i, const int *set, int n)
{
    for (int k = 0; k < n; k++)
        if (set[k] == i)
            return 1;
    return 0;
}

static void pd_object_prefix(u8 *out, const u8 *w0)
{
    static const int OBJ_PTR_WORDS[4] = { 4, 5, 26, 27 };
    int pc = 0;
    for (int i = 0; i < OBJ_N64_WORDS; i++) {
        const u8 *w = w0 + 4 * i;
        if (pd_in(i, OBJ_PTR_WORDS, 4)) { pc = (pc + 7) & ~7; pc += 8; }
        else if (i == 0) { w_hdr(out + pc, w); pc += 4; }
        else if (i == 1) { w_hh(out + pc, w); pc += 4; }
        else if (i == 30 || i == 31) { memcpy(out + pc, w, 4); pc += 4; }
        else { w_bswap32(out + pc, w); pc += 4; }
    }
}

/* out has PROPDEF_PC_BYTES(t) zeroed bytes; w0 = the record's N64 words */
static int pd_convert_record(u8 *out, const u8 *w0, int t, const char *name)
{
    int n64w = PROPDEF_N64_WORDS(t), pcb = PROPDEF_PC_BYTES(t);
#define W(i) (w0 + 4 * (i))
    switch (t) {
    case 3: case 5: case 12: case 17: case 36: case 42: case 43:
        pd_object_prefix(out, w0);
        return 0;
    case 1: {
        static const int DOOR_TAIL_PTR_WORDS[4] = { 50, 51, 61, 62 };
        pd_object_prefix(out, w0);
        int pc = OBJ_PC_BYTES;
        for (int i = OBJ_N64_WORDS; i < n64w; i++) {
            const u8 *w = W(i);
            if (pd_in(i, DOOR_TAIL_PTR_WORDS, 4)) { pc = (pc + 7) & ~7; pc += 8; }
            else if (i == 38) { w_hh(out + pc, w); pc += 4; }
            else if (i == 47) { out[pc] = w[0]; out[pc + 1] = w[1]; out[pc + 2] = w[3]; out[pc + 3] = w[2]; pc += 4; }
            else if (i == 49) { out[pc] = w[1]; out[pc + 1] = w[0]; out[pc + 2] = w[2]; out[pc + 3] = w[3]; pc += 4; }
            else { w_bswap32(out + pc, w); pc += 4; }
        }
        if (pc > pcb) { fail(name, "DOOR record overflow", pc); return -1; }
        return 0;
    }
    case 4: case 21: case 7: {
        pd_object_prefix(out, w0);
        int pc = OBJ_PC_BYTES;
        for (int i = OBJ_N64_WORDS; i < n64w; i++) { w_bswap32(out + pc, W(i)); pc += 4; }
        return 0;
    }
    case 8: {
        pd_object_prefix(out, w0);
        const u8 *w = W(32);
        out[144] = w[0]; out[145] = w[1]; out[146] = w[3]; out[147] = w[2];
        return 0;
    }
    case 10: case 11: {
        pd_object_prefix(out, w0);
        int nmon = t == 10 ? 1 : 4;
        int tail_start = OBJ_N64_WORDS + 29 * nmon, pc = OBJ_PC_BYTES + 128 * nmon;
        for (int i = tail_start; i < n64w; i++) {
            if (t == 11) memcpy(out + pc, W(i), 4); else w_bswap32(out + pc, W(i));
            pc += 4;
        }
        return 0;
    }
    case 6: {
        pd_object_prefix(out, w0);
        w_bswap32(out + 144, W(32));
        int pc = 212;
        for (int i = 49; i < n64w; i++) { w_bswap32(out + pc, W(i)); pc += 4; }
        return 0;
    }
    case 22:
        w_hdr(out, W(0)); w_hh(out + 4, W(1));
        return 0;
    case 37:
        w_hdr(out, W(0));
        for (int i = 1; i < 9; i++) w_bswap32(out + 4 * i, W(i));
        return 0;
    case 14: case 19: case 38: case 44: {
        int nidx = t == 44 ? 3 : 2;
        w_hdr(out, W(0));
        for (int k = 0; k < nidx; k++) w_bswap32(out + 8 + 8 * k, W(k + 1));
        return 0;
    }
    case 18:
        w_hdr(out, W(0)); w_bswap32(out + 4, W(1));
        out[8] = W(2)[1]; out[9] = W(2)[0]; out[10] = W(2)[2]; out[11] = W(2)[3];
        return 0;
    case 9:
        w_hdr(out, W(0));
        for (int i = 1; i < 6; i++) w_hh(out + 4 * i, W(i));
        return 0;
    case 47: case 39: case 40: case 45: case 13: case 20: {
        /* D122 OBJ_TAIL_DESC: (ptr_words, hh_words), D123 OBJ_ID_WORDS */
        int ptrw[3] = { -1, -1, -1 }, nptr = 0, id32 = 0, hh_lo = -1, hh_hi = -1;
        if (t == 39) { ptrw[0] = 41; ptrw[1] = 43; nptr = 2; hh_lo = hh_hi = 33; id32 = 1; }
        if (t == 40) { ptrw[0] = 43; ptrw[1] = 44; nptr = 2; hh_lo = hh_hi = 33; id32 = 1; }
        if (t == 45) { ptrw[0] = 32; nptr = 1; }
        if (t == 13) { ptrw[0] = 49; ptrw[1] = 50; ptrw[2] = 51; nptr = 3; }
        if (t == 20) { hh_lo = 32; hh_hi = 44; }
        pd_object_prefix(out, w0);
        int pc = OBJ_PC_BYTES;
        for (int i = OBJ_N64_WORDS; i < n64w; i++) {
            const u8 *w = W(i);
            if (id32 && i == 32) { pc = (pc + 7) & ~7; w_bswap32(out + pc, w); pc += 8; }
            else if (pd_in(i, ptrw, nptr)) { pc = (pc + 7) & ~7; pc += 8; }
            else if (i >= hh_lo && i <= hh_hi && hh_lo >= 0) { w_hh(out + pc, w); pc += 4; }
            else { w_bswap32(out + pc, w); pc += 4; }
        }
        if (pc > pcb) { fail(name, "tail record overflow", t); return -1; }
        return 0;
    }
    case 30: case 32: case 33: case 35:
        w_hdr(out, W(0));
        for (int i = 1; i < n64w - 1; i++) w_bswap32(out + 4 * i, W(i));
        return 0;
    default:
        w_hdr(out, W(0));
        for (int i = 1; i < n64w; i++) w_bswap32(out + 4 * i, W(i));
        return 0;
    }
#undef W
}

/* convert_stream(src, start, end) -> PC bytes */
static int pd_convert_stream(const Src *s, s64 start, s64 end, GeconvBlob *out)
{
    out->len = 0;
    s64 o = start;
    long n = 0;
    while (o < end) {
        if (o + 4 > end) { fail(s->name, "propDefs: truncated header", o); return -1; }
        int t = (int)rd8(s, o + 3);
        int n64w = PROPDEF_N64_WORDS(t), pcb = PROPDEF_PC_BYTES(t);
        if (n64w < 0 || pcb < 0) { fail(s->name, "propDefs: unknown record type", t); return -1; }
        if (o + 4 * n64w > end || o + 4 * n64w > s->n) { fail(s->name, "propDefs: record overruns region end", o); return -1; }
        unsigned long at = out->len;
        if (blob_append(out, 0, (unsigned long)pcb)) return -1;
        if (pd_convert_record(out->data + at, s->p + o, t, s->name)) return -1;
        o += 4 * n64w;
        n++;
        if (t == 48) break;
    }
    if (o != end) {
        s64 pad = end - o;
        int nz = 0;
        for (s64 i = o; i < end && i < s->n; i++) if (s->p[i]) nz = 1;
        if (pad < 0 || pad >= 16 || nz) { fail(s->name, "propDefs: walk ended before the region end", o); return -1; }
        if (blob_append(out, s->p + o, (unsigned long)pad)) return -1;
    }
    return 0;
}

/* ---- d88_emit.py convert_usetup ---- */
enum { K_HEADER, K_GROW, K_INTRO, K_PROPDEFS, K_LEAF_S32, K_LEAF_CSTR, K_AISTREAM, K_GAP };
typedef struct {
    s64 start, end;   /* end < 0: None (resolved later) */
    int kind;
    int table;        /* K_GROW: GROW_TABLES index */
} SReg;

static int cmp_sreg_start(const void *a, const void *b)
{
    s64 x = ((const SReg *)a)->start, y = ((const SReg *)b)->start;
    return x < y ? -1 : x > y;
}

typedef struct {
    const char *fn;
    int field;          /* FIELD_NAMES index */
    int recsize, term_off, s32neg, oldsz, newsz;
    Vec starts;         /* record starts (excl. terminator) */
    s64 term;           /* -1 = None */
} Grow;

static const int INTRO_SZ[10] = { 12, 16, 16, 32, 8, 8, 40, 12, 8, 4 };

/* FIELD_NAMES order */
enum { F_PATHWAYPOINTS, F_WAYPOINTGROUPS, F_INTRO, F_PROPDEFS, F_PATROLPATHS, F_AILISTS, F_PADS,
       F_BOUNDPADS, F_PADNAMES, F_BOUNDPADNAMES };

typedef struct { s64 start; int kind; s64 end; } Leaf;

static u8 *d88_convert_usetup(const char *name, const Src *s, long *outlen)
{
    long D = s->n;
    if (D < 0x28) { fail(name, "too small", D); return 0; }
    u32 H[10];
    for (int i = 0; i < 10; i++) {
        H[i] = rd32(s, 4 * i);
        if (H[i] != 0 && H[i] >= (u32)D) { fail(name, "header field offset >= filesize", H[i]); return 0; }
    }
    /* GROW_TABLES order: pads, boundpads, waypointgroups, pathwaypoints, patrolpaths, ailists,
     * padnames, boundpadnames */
    Grow G[8] = {
        { "pads", F_PADS, 44, 0x24, 0, 44, 56, {0}, -1 },
        { "boundpads", F_BOUNDPADS, 68, 0x24, 0, 68, 80, {0}, -1 },
        { "waypointgroups", F_WAYPOINTGROUPS, 12, 0, 0, 12, 24, {0}, -1 },
        { "pathwaypoints", F_PATHWAYPOINTS, 16, 0, 1, 16, 24, {0}, -1 },
        { "patrolpaths", F_PATROLPATHS, 8, 0, 0, 8, 16, {0}, -1 },
        { "ailists", F_AILISTS, 8, 0, 0, 8, 16, {0}, -1 },
        { "padnames", F_PADNAMES, 4, 0, 0, 4, 8, {0}, -1 },
        { "boundpadnames", F_BOUNDPADNAMES, 4, 0, 0, 4, 8, {0}, -1 },
    };
    u8 *outp = 0;
    SReg *regs = 0, *tiled = 0;
    Leaf *leaves = 0;
    long nleaves = 0;
    Map leafset = {0}, delta_at = {0};
    Vec intro_o = {0}, intro_t = {0}, ai = {0};
    GeconvBlob propdefs_pc = {0};
    int have_propdefs = 0;

    for (int g = 0; g < 8; g++) {
        u32 off = H[G[g].field];
        if (off == 0) continue;
        s64 o = off;
        long guard = 0;
        while (guard < 100000 && !g_failed) {
            guard++;
            int term = G[g].s32neg ? (rds32(s, o + G[g].term_off) < 0) : (rd32(s, o + G[g].term_off) == 0);
            if (term) { G[g].term = o; break; }
            vpush(&G[g].starts, o);
            o += G[g].recsize;
        }
        if (G[g].term < 0 && !g_failed) fail(name, "runaway walk_fixed", off);
        if (g_failed) goto bad;
    }

    s64 intro_end = H[F_INTRO];
    if (H[F_INTRO]) {
        s64 o = H[F_INTRO];
        long guard = 0;
        int done = 0;
        while (guard < 5000 && !g_failed) {
            guard++;
            s32 t = rds32(s, o);
            if (t < 0 || t > 9) { fail(name, "unknown intro type", t); goto bad; }
            vpush(&intro_o, o); vpush(&intro_t, t);
            o += INTRO_SZ[t];
            if (t == 9) { done = 1; break; }
        }
        if (!done) { fail(name, "intro walk runaway", o); goto bad; }
        intro_end = o;
    }

    /* leaves: insertion-ordered dict start -> (kind, end) */
    long capl = 256;
    leaves = (Leaf *)malloc(sizeof(Leaf) * (unsigned long)capl);
    if (!leaves) { fail(name, "out of memory", 0); goto bad; }
#define ADD_LEAF(st, kd) do { \
        s64 _st = (st); \
        if (_st != 0 && !mget(&leafset, (u32)_st, 0)) { \
            s64 _end = -1; \
            if ((kd) == K_LEAF_S32) { \
                s64 _o = _st; long _g = 0; \
                while (_g < 100000 && !g_failed) { _g++; if (rds32(s, _o) < 0) { _end = _o + 4; break; } _o += 4; _g++; } \
                if (_end < 0 && !g_failed) fail(name, "runaway s32 array", _st); \
            } else { \
                for (s64 _o = _st; _o < D; _o++) if (!s->p[_o]) { _end = _o + 1; break; } \
                if (_end < 0) fail(name, "unterminated string", _st); \
            } \
            if (g_failed) goto bad; \
            if (nleaves == capl) { capl *= 2; Leaf *_nl = (Leaf *)xrealloc(leaves, sizeof(Leaf) * (unsigned long)capl); if (!_nl) { leaves = 0; goto bad; } leaves = _nl; } \
            leaves[nleaves].start = _st; leaves[nleaves].kind = (kd); leaves[nleaves].end = _end; nleaves++; \
            mput(&leafset, (u32)_st, 1); \
        } \
    } while (0)
    /* G indices: 0 pads 1 boundpads 2 waypointgroups 3 pathwaypoints 4 patrolpaths 5 ailists 6 padnames 7 boundpadnames */
    for (long i = 0; i < G[3].starts.n; i++) ADD_LEAF(rd32(s, G[3].starts.v[i] + 4), K_LEAF_S32);
    for (long i = 0; i < G[2].starts.n; i++) {
        ADD_LEAF(rd32(s, G[2].starts.v[i] + 0), K_LEAF_S32);
        ADD_LEAF(rd32(s, G[2].starts.v[i] + 4), K_LEAF_S32);
    }
    for (long i = 0; i < G[4].starts.n; i++) ADD_LEAF(rd32(s, G[4].starts.v[i] + 0), K_LEAF_S32);
    for (long i = 0; i < G[0].starts.n; i++) ADD_LEAF(rd32(s, G[0].starts.v[i] + 0x24), K_LEAF_CSTR);
    for (long i = 0; i < G[1].starts.n; i++) ADD_LEAF(rd32(s, G[1].starts.v[i] + 0x24), K_LEAF_CSTR);
    for (long i = 0; i < G[6].starts.n; i++) ADD_LEAF(rd32(s, G[6].starts.v[i]), K_LEAF_CSTR);
    for (long i = 0; i < G[7].starts.n; i++) ADD_LEAF(rd32(s, G[7].starts.v[i]), K_LEAF_CSTR);
#undef ADD_LEAF
    for (long i = 0; i < G[5].starts.n; i++) {
        u32 st = rd32(s, G[5].starts.v[i]);
        if (st && !mget(&leafset, st, 0)) vpush(&ai, st);
    }
    vsortuniq(&ai);   /* a Python set; only its order among equal region starts could matter */

    long cap = 16 + nleaves + ai.n;
    regs = (SReg *)calloc((unsigned long)cap, sizeof(SReg));
    tiled = (SReg *)calloc((unsigned long)(2 * cap + 2), sizeof(SReg));
    if (!regs || !tiled) { fail(name, "out of memory", cap); goto bad; }
    long nr = 0;
    regs[nr++] = (SReg){ 0, 0x28, K_HEADER, -1 };
    for (int g = 0; g < 8; g++)
        if (G[g].term >= 0) regs[nr++] = (SReg){ H[G[g].field], G[g].term + G[g].oldsz, K_GROW, g };
    if (H[F_INTRO]) regs[nr++] = (SReg){ H[F_INTRO], intro_end, K_INTRO, -1 };
    if (H[F_PROPDEFS]) regs[nr++] = (SReg){ H[F_PROPDEFS], -1, K_PROPDEFS, -1 };
    for (long i = 0; i < nleaves; i++) regs[nr++] = (SReg){ leaves[i].start, leaves[i].end, leaves[i].kind, -1 };
    for (long i = 0; i < ai.n; i++) regs[nr++] = (SReg){ ai.v[i], -1, K_AISTREAM, -1 };
    msort(regs, nr, sizeof(SReg), cmp_sreg_start);
    for (long i = 0; i < nr; i++)
        if (regs[i].end < 0) regs[i].end = i + 1 < nr ? regs[i + 1].start : D;

    long nt = 0;
    s64 cursor = 0;
    for (long i = 0; i < nr; i++) {
        if (regs[i].start < cursor) { fail(name, "region overlap", regs[i].start); goto bad; }
        if (regs[i].start > cursor) tiled[nt++] = (SReg){ cursor, regs[i].start, K_GAP, -1 };
        if (regs[i].end < regs[i].start) { fail(name, "region has negative length", regs[i].start); goto bad; }
        tiled[nt++] = regs[i];
        cursor = regs[i].end;
    }
    if (cursor < D) tiled[nt++] = (SReg){ cursor, D, K_GAP, -1 };
    else if (cursor > D) { fail(name, "regions overrun EOF", cursor); goto bad; }

    if (H[F_PROPDEFS]) {
        s64 pd_end = -1;
        for (long i = 0; i < nt; i++) if (tiled[i].kind == K_PROPDEFS) { pd_end = tiled[i].end; break; }
        if (pd_convert_stream(s, H[F_PROPDEFS], pd_end, &propdefs_pc)) goto bad;
        have_propdefs = 1;
    }

    s64 cum = 0x28;
    for (long i = 0; i < nt; i++) {
        SReg *r = &tiled[i];
        if (r->kind == K_HEADER) { mput(&delta_at, (u32)r->start, 0); continue; }
        mput(&delta_at, (u32)r->start, cum);
        if (r->kind == K_GROW) {
            Grow *g = &G[r->table];
            cum += (g->starts.n + 1) * (s64)(g->newsz - g->oldsz);
        } else if (r->kind == K_PROPDEFS && have_propdefs) {
            cum += (s64)propdefs_pc.len - (r->end - r->start);
        }
    }
#define RELOC(off) ((off) == 0 ? (s64)0 : (s64)(off) + (mget(&delta_at, (u32)(off), &_dv) ? _dv : 0))
    s64 _dv;
    long total_new = (long)(D + cum);
    outp = (u8 *)calloc((unsigned long)total_new, 1);
    if (!outp) { fail(name, "out of memory", total_new); goto bad; }
    Dst O = { outp, total_new, name };
    Dst *out = &O;
    for (int i = 0; i < 10; i++) {
        wle32(out, 8 * i, (u32)RELOC(H[i]));
        wle32(out, 8 * i + 4, 0);
    }

    for (int gi = 0; gi < 8; gi++) {
        Grow *g = &G[gi];
        if (g->term < 0) continue;
        s64 table_base = RELOC(H[g->field]);
        for (long idx = 0; idx <= g->starts.n; idx++) {
            s64 so = idx < g->starts.n ? g->starts.v[idx] : g->term;
            s64 dd = table_base + idx * g->newsz;
            if (gi == 0 || gi == 1) {
                for (int k = 0; k < 9; k++) wswap32(out, dd + 4 * k, s, so + 4 * k);
                u32 plink = rd32(s, so + 0x24);
                wle32(out, dd + 0x28, (u32)RELOC(plink));
                wle32(out, dd + 0x2c, 0);
                wzero(out, dd + 0x30, 8);
                if (gi == 1)
                    for (int k = 0; k < 6; k++) wswap32(out, dd + 0x38 + 4 * k, s, so + 0x2c + 4 * k);
            } else if (gi == 2) {
                u32 nb = rd32(s, so), wpv = rd32(s, so + 4), dist = rd32(s, so + 8);
                wle32(out, dd, nb ? (u32)RELOC(nb) : 0); wle32(out, dd + 4, 0);
                wle32(out, dd + 8, wpv ? (u32)RELOC(wpv) : 0); wle32(out, dd + 12, 0);
                wle32(out, dd + 16, dist); wle32(out, dd + 20, 0);
            } else if (gi == 3) {
                u32 padID = rd32(s, so), nb = rd32(s, so + 4), groupNum = rd32(s, so + 8), dist = rd32(s, so + 12);
                wle32(out, dd, padID); wle32(out, dd + 4, 0);
                wle32(out, dd + 8, nb ? (u32)RELOC(nb) : 0); wle32(out, dd + 12, 0);
                wle32(out, dd + 16, groupNum); wle32(out, dd + 20, dist);
            } else if (gi == 4) {
                u32 wpv = rd32(s, so);
                wle32(out, dd, wpv ? (u32)RELOC(wpv) : 0); wle32(out, dd + 4, 0);
                *wp(out, dd + 8, 1) = (u8)rd8(s, so + 4);
                *wp(out, dd + 9, 1) = (u8)rd8(s, so + 5);
                wle16(out, dd + 10, rdu16(s, so + 6));
                wle32(out, dd + 12, 0);
            } else if (gi == 5) {
                u32 al = rd32(s, so), ID = rd32(s, so + 4);
                wle32(out, dd, al ? (u32)RELOC(al) : 0); wle32(out, dd + 4, 0);
                wle32(out, dd + 8, ID); wle32(out, dd + 12, 0);
            } else {
                u32 p = rd32(s, so);
                wle32(out, dd, p ? (u32)RELOC(p) : 0); wle32(out, dd + 4, 0);
            }
        }
    }

    {
        s64 intro_base_delta = 0;
        if (H[F_INTRO] && mget(&delta_at, H[F_INTRO], &_dv)) intro_base_delta = _dv;
        for (long i = 0; i < intro_o.n; i++) {
            s64 o = intro_o.v[i], dd = o + intro_base_delta;
            int t = (int)intro_t.v[i];
            if (t == 6) {
                for (int k = 0; k < 10; k++) {
                    if (k == 7) {
                        wle16(out, dd + 4 * k, rdu16(s, o + 4 * k));
                        wle16(out, dd + 4 * k + 2, rdu16(s, o + 4 * k + 2));
                    } else {
                        wswap32(out, dd + 4 * k, s, o + 4 * k);
                    }
                }
            } else {
                for (int k = 0; k < INTRO_SZ[t] / 4; k++) wswap32(out, dd + 4 * k, s, o + 4 * k);
            }
        }
    }

    for (long i = 0; i < nleaves; i++) {
        s64 st = leaves[i].start, dd = RELOC(st);
        if (leaves[i].kind == K_LEAF_S32)
            for (s64 o = st; o < leaves[i].end; o += 4) wswap32(out, dd + (o - st), s, o);
        else
            wcopy(out, dd, s, st, (long)(leaves[i].end - st));
    }

    for (long i = 0; i < nt; i++) {
        SReg *r = &tiled[i];
        if (r->kind == K_PROPDEFS) {
            s64 dd = RELOC(r->start);
            if (propdefs_pc.len) memcpy(wp(out, dd, (long)propdefs_pc.len), propdefs_pc.data, propdefs_pc.len);
        } else if (r->kind == K_AISTREAM || r->kind == K_GAP) {
            wcopy(out, RELOC(r->start), s, r->start, (long)(r->end - r->start));
        }
    }
#undef RELOC
    if (g_failed) goto bad;
    *outlen = total_new;
    goto done;
bad:
    free(outp);
    outp = 0;
done:
    for (int g = 0; g < 8; g++) vfree(&G[g].starts);
    free(regs);
    free(tiled);
    free(leaves);
    mfree(&leafset);
    mfree(&delta_at);
    vfree(&intro_o);
    vfree(&intro_t);
    vfree(&ai);
    blob_free(&propdefs_pc);
    return outp;
}

/* ================================================================ driver === */
enum { J_D43, J_SEG, J_STAN, J_USETUP };
typedef struct {
    int kind;
    long name;       /* index into T.names */
} Job;

static struct {
    Job *jobs;
    long njobs, next;
    GeconvResult res;
    Emit em43, emcg;
    GeconvBlob tmp;
    int begun;
} S;

static int cmp_name_idx(const void *a, const void *b)
{
    return strcmp(T.names[*(const long *)a], T.names[*(const long *)b]);
}

void geconvEnd(void)
{
    free(T.fl);
    free(T.names);
    memset(&T, 0, sizeof(T));
    free(S.jobs);
    blob_free(&S.res.models);
    blob_free(&S.res.modelsManifest);
    blob_free(&S.res.cg);
    blob_free(&S.res.cgManifest);
    blob_free(&S.tmp);
    memset(&S, 0, sizeof(S));
}

int geconvBegin(const GeconvInput *in)
{
    geconvEnd();
    g_failed = 0;
    g_err[0] = 0;
    T.in = in;
    if (parse_filelist() || parse_restable())
        return -1;
    long cap = T.nnames * 2 + 8;
    S.jobs = (Job *)calloc((unsigned long)cap, sizeof(Job));
    long *setup = (long *)calloc((unsigned long)T.nnames + 1, sizeof(long));
    if (!S.jobs || !setup) { free(setup); fail("geconv", "out of memory", cap); return -1; }
    /* d43: is_model_name, table order */
    for (long i = 0; i < T.nnames; i++) {
        const char *n = T.names[i];
        if (n[0] && (n[0] == 'C' || n[0] == 'G' || n[0] == 'P') && ends(n, "Z") && !contains(n, "_stan"))
            S.jobs[S.njobs++] = (Job){ J_D43, i };
    }
    /* d69: table order */
    for (long i = 0; i < T.nnames; i++) {
        const char *n = T.names[i];
        if (starts(n, "bg/") && ends(n, ".seg")) S.jobs[S.njobs++] = (Job){ J_SEG, i };
        else if ((starts(n, "Tbg_") || !strcmp(n, "TbgxZ")) && ends(n, "Z")) S.jobs[S.njobs++] = (Job){ J_STAN, i };
    }
    /* d88: sorted(set(setup names)) */
    long ns = 0;
    for (long i = 0; i < T.nnames; i++) {
        const char *n = T.names[i];
        if ((starts(n, "Usetup") || starts(n, "Ump_setup")) && ends(n, "Z")) setup[ns++] = i;
    }
    msort(setup, ns, sizeof(long), cmp_name_idx);
    for (long i = 0; i < ns; i++)
        if (i == 0 || strcmp(T.names[setup[i]], T.names[setup[i - 1]]))
            S.jobs[S.njobs++] = (Job){ J_USETUP, setup[i] };
    free(setup);
    static const char hdr[] = "name,offset,size\r\n";
    blob_append(&S.res.modelsManifest, hdr, sizeof(hdr) - 1);
    blob_append(&S.res.cgManifest, hdr, sizeof(hdr) - 1);
    S.em43.bin = &S.res.models; S.em43.man = &S.res.modelsManifest;
    S.emcg.bin = &S.res.cg; S.emcg.man = &S.res.cgManifest;
    S.begun = 1;
    return g_failed ? -1 : (int)S.njobs + 1;
}

int geconvStepsTotal(void) { return (int)S.njobs + 1; }

static int step_job(const Job *j)
{
    const char *name = T.names[j->name];
    if (j->kind == J_D43) {
        if (d43_process(name, &S.tmp)) return -1;
        return emit_file(&S.em43, name, S.tmp.data, (long)S.tmp.len);
    }
    const FlRow *row;
    char base[64];
    if (j->kind == J_SEG) {
        const char *b = name;
        for (const char *q = name; *q; q++) if (*q == '/') b = q + 1;
        unsigned long bl = strlen(b) - 4;
        memcpy(base, b, bl);
        base[bl] = 0;
        row = fl_get(base);
    } else {
        row = fl_get(name);
    }
    if (!row || row->size == 0) return 0;   /* unused in this ROM build */
    if ((unsigned long)row->off + row->size > T.in->romlen) { fail(name, "filelist row outside the ROM", row->off); return -1; }
    const u8 *raw = T.in->rom + row->off;
    if (j->kind == J_SEG) {
        Src s = { raw, (long)row->size, name };
        long n = 0;
        u8 *o = d69_convert_seg(name, &s, &n);
        if (!o) return -1;
        int r = emit_file(&S.emcg, name, o, n);
        free(o);
        return r;
    }
    if (row->size < 2 || raw[0] != 0x11 || raw[1] != 0x72) { fail(name, "bad RZ magic", row->off); return -1; }
    u8 *dec = 0;
    long dn = 0;
    if (inflate_raw(raw + 2, (long)row->size - 2, &dec, &dn, name)) return -1;
    Src s = { dec, dn, name };
    long n = 0;
    u8 *o = j->kind == J_STAN ? d69_convert_stan(name, &s, &n) : d88_convert_usetup(name, &s, &n);
    free(dec);
    if (!o) return -1;
    int r = rz_compress(o, n, &S.tmp, name);
    free(o);
    if (r) return -1;
    return emit_file(&S.emcg, name, S.tmp.data, (long)S.tmp.len);
}

int geconvStep(void)
{
    if (!S.begun || g_failed) return -1;
    if (S.next < S.njobs) {
        if (step_job(&S.jobs[S.next]) || g_failed) return -1;
        S.next++;
        return (int)(S.njobs - S.next) + 1;
    }
    if (S.next == S.njobs) {
        /* pcmodels.bin ends 16-aligned; pccg.bin does not (as the Python writes them) */
        long off = S.em43.cur;
        if (off % 16 && blob_append(&S.res.models, 0, (unsigned long)(16 - off % 16))) return -1;
        S.next++;
    }
    return 0;
}

const char *geconvError(void) { return g_err; }
const GeconvResult *geconvResult(void) { return &S.res; }
