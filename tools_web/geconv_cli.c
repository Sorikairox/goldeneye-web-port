/*
 * geconv_cli — native driver for port/src/convert/geconv.c (the C port of the sidecar
 * converters), used to check that it is byte-identical to the Python converters
 * (scripts/web/geconv_check.sh). Reads the same inputs as the Python scripts, from the repo:
 *   data/ge007.<region>.z64, scripts/filelist.<u|e|j>.csv,
 *   assets/obseg/file_resource_table.inc.c, assets/<any>/<name>modelfileheader.inc.c (counts)
 * and writes <outdir>/pcmodels-<region>/{pcmodels.bin,manifest.csv}, <outdir>/pccg-<region>/...
 *
 *   cc -O2 -DGECONV_HOST -Iport/src/convert tools_web/geconv_cli.c port/src/convert/geconv.c -lz
 *   ./geconv_cli pal-final <outdir>
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "geconv.h"

static char *slurp(const char *path, unsigned long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { fprintf(stderr, "read %s failed\n", path); exit(1); }
    b[n] = 0;
    fclose(f);
    *len = n;
    return b;
}

/* nsnt: MODELFILEHEADER(a0, ..., a4=NUMSWITCHES, ..., a8=NUMTEXTURES) from every
 * *modelfileheader.inc.c under assets/ (os.walk order does not matter unless a name repeats;
 * like the Python dict, the last one parsed wins, here in readdir order). */
typedef struct { char name[64]; int ns, nt; } Hdr;
static Hdr *hdrs;
static int nhdrs, caphdrs;

/* Python int(x, 0): optional sign, 0x/0o/0b prefixes, no leading zeros on decimals */
static int py_int0(const char *s, int *out)
{
    const char *p = s;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    long v = 0;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) { base = 8; p += 2; }
    else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
    else if (p[0] == '0') { for (const char *q = p; *q; q++) if (*q != '0') return 0; *out = 0; return 1; }
    if (!*p) return 0;
    for (; *p; p++) {
        int d;
        if (*p == '_') continue;
        if (isdigit((unsigned char)*p)) d = *p - '0';
        else if (isalpha((unsigned char)*p)) d = tolower((unsigned char)*p) - 'a' + 10;
        else return 0;
        if (d >= base) return 0;
        v = v * base + d;
    }
    *out = (int)(neg ? -v : v);
    return 1;
}

static void strip(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (*b && isspace((unsigned char)*b)) b++;
    memmove(s, b, strlen(b) + 1);
}

static void parse_hdr_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *m = strstr(line, "MODELFILEHEADER(");
        if (!m) continue;
        /* greedy (.*) up to the last ')' followed only by \s*;?\s*$ */
        char *a = m + 16, *close = NULL;
        for (char *q = a; *q; q++) {
            if (*q != ')') continue;
            char *r = q + 1;
            while (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\f' || *r == '\v') r++;
            if (*r == ';') r++;
            while (isspace((unsigned char)*r)) r++;
            if (!*r) close = q;
        }
        if (!close) continue;
        *close = 0;
        char *fields[32];
        int n = 0;
        char *p = a;
        for (;;) {
            char *c = strchr(p, ',');
            if (n < 32) fields[n++] = p;
            if (!c) break;
            *c = 0;
            p = c + 1;
        }
        if (n < 9) continue;
        for (int i = 0; i < n; i++) strip(fields[i]);
        int ns, nt;
        if (!py_int0(fields[4], &ns) || !py_int0(fields[8], &nt)) continue;
        int k;
        for (k = 0; k < nhdrs; k++) if (!strcmp(hdrs[k].name, fields[0])) break;
        if (k == nhdrs) {
            if (nhdrs == caphdrs) { caphdrs = caphdrs ? caphdrs * 2 : 512; hdrs = realloc(hdrs, caphdrs * sizeof(Hdr)); }
            nhdrs++;
            snprintf(hdrs[k].name, sizeof hdrs[k].name, "%s", fields[0]);
        }
        hdrs[k].ns = ns;
        hdrs[k].nt = nt;
    }
    fclose(f);
}

static void walk(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[4096];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { walk(p); continue; }
        size_t l = strlen(e->d_name);
        const char *suf = "modelfileheader.inc.c";
        size_t sl = strlen(suf);
        if (l >= sl && !strcasecmp(e->d_name + l - sl, suf)) parse_hdr_file(p);
    }
    closedir(d);
}

static int header_cb(void *ctx, const char *key, int *ns, int *nt)
{
    (void)ctx;
    for (int k = 0; k < nhdrs; k++)
        if (!strcmp(hdrs[k].name, key)) { *ns = hdrs[k].ns; *nt = hdrs[k].nt; return 1; }
    return 0;
}

static void put(const char *dir, const char *file, const GeconvBlob *b)
{
    char p[4096];
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "wb");
    if (!f || fwrite(b->data, 1, b->len, f) != b->len) { fprintf(stderr, "write %s failed\n", p); exit(1); }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *region = argc > 1 ? argv[1] : "pal-final";
    const char *outdir = argc > 2 ? argv[2] : "data";
    const char *fl = !strcmp(region, "ntsc-final") ? "u" : !strcmp(region, "pal-final") ? "e" : "j";
    char p[4096];
    GeconvInput in = {0};
    snprintf(p, sizeof p, "data/ge007.%s.z64", region);
    in.rom = (const unsigned char *)slurp(p, &in.romlen);
    snprintf(p, sizeof p, "scripts/filelist.%s.csv", fl);
    in.filelist = slurp(p, &in.filelistlen);
    in.restable = slurp("assets/obseg/file_resource_table.inc.c", &in.restablelen);
    walk("assets");
    in.header = header_cb;
    int total = geconvBegin(&in);
    if (total < 0) { fprintf(stderr, "geconv: %s\n", geconvError()); return 1; }
    int r;
    while ((r = geconvStep()) > 0)
        ;
    if (r < 0) { fprintf(stderr, "geconv: %s\n", geconvError()); return 1; }
    const GeconvResult *res = geconvResult();
    mkdir(outdir, 0755);
    snprintf(p, sizeof p, "%s/pcmodels-%s", outdir, region);
    put(p, "pcmodels.bin", &res->models);
    put(p, "manifest.csv", &res->modelsManifest);
    snprintf(p, sizeof p, "%s/pccg-%s", outdir, region);
    put(p, "pccg.bin", &res->cg);
    put(p, "manifest.csv", &res->cgManifest);
    printf("geconv %s: %d steps, pcmodels %lu bytes, pccg %lu bytes\n", region, total,
           res->models.len, res->cg.len);
    geconvEnd();
    return 0;
}
