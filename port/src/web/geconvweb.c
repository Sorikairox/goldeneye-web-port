/*
 * geconvweb.c — page interface of the in-browser sidecar conversion (port/src/convert/geconv.c).
 *
 * The page (port/web/main.js) calls, before main() and after ge_web_romfill():
 *   n = ge_web_geconv_begin(romAddr, romLen)    -> number of steps (< 0: error)
 *   while ((r = ge_web_geconv_step()) > 0) ...  -> steps left; yields to the browser in between
 *   ge_web_geconv_blob(i, 0|1)                  -> address / length of blob i (0 pcmodels.bin,
 *                                                  1 its manifest.csv, 2 pccg.bin, 3 its manifest)
 *   ge_web_geconv_end()
 * and stores the four files in IndexedDB (key: ROM SHA-1 + GECONV_VERSION) and in MEMFS under
 * /data/pcmodels-<region>/ and /data/pccg-<region>/, where port/src/pcmodels.c and pccg.c load
 * them exactly as on native.
 *
 * Converter inputs: the filelist and file_resource_table texts and the list of model header
 * objects come from build/geconv_inputs.c (scripts/web/gen_geconv_inputs.py: names and offsets,
 * no ROM data); each model's numSwitches/numtextures are read from the game's own
 * ModelFileHeader objects, which ge_web_romfill() has filled from the ROM by then.
 */
#include <stddef.h>  /* port/shim/stddef.h: size_t before the Emscripten headers */
#include <emscripten.h>
#include <stdint.h>
#include <string.h>
#include "system.h"
#include "../convert/geconv.h"

struct GeconvHeaderRef {
    const char *stem;
    const unsigned char *hdr;   /* &<stem>_header (weak: NULL when not linked) */
};
extern const char ge_geconv_filelist[];
extern const unsigned long ge_geconv_filelist_len;
extern const char ge_geconv_restable[];
extern const unsigned long ge_geconv_restable_len;
extern const struct GeconvHeaderRef ge_geconv_headers[];
extern const int ge_geconv_nheaders;
/* struct ModelFileHeader field offsets in the LP64 layout (bondtypes.h): 3 pointers, then
 * s16 numSwitches, s16 numMatrices, f32 radius, s16 numRecords, s16 numtextures */
extern const int ge_geconv_off_numswitches, ge_geconv_off_numtextures;

static int geconvwebHeader(void *ctx, const char *key, int *ns, int *nt)
{
    (void)ctx;
    for (int i = 0; i < ge_geconv_nheaders; i++) {
        if (strcmp(ge_geconv_headers[i].stem, key) || !ge_geconv_headers[i].hdr)
            continue;
        int16_t a, b;
        memcpy(&a, ge_geconv_headers[i].hdr + ge_geconv_off_numswitches, 2);
        memcpy(&b, ge_geconv_headers[i].hdr + ge_geconv_off_numtextures, 2);
        *ns = a;
        *nt = b;
        return 1;
    }
    return 0;
}

static GeconvInput s_in;

EMSCRIPTEN_KEEPALIVE int ge_web_geconv_begin(uint32_t romAddr, uint32_t romLen)
{
    s_in.rom = (const unsigned char *)(uintptr_t)romAddr;
    s_in.romlen = romLen;
    s_in.filelist = ge_geconv_filelist;
    s_in.filelistlen = ge_geconv_filelist_len;
    s_in.restable = ge_geconv_restable;
    s_in.restablelen = ge_geconv_restable_len;
    s_in.header = geconvwebHeader;
    s_in.headerctx = NULL;
    int n = geconvBegin(&s_in);
    if (n < 0)
        sysLogPrintf(LOG_ERROR, "geconv: %s", geconvError());
    return n;
}

EMSCRIPTEN_KEEPALIVE int ge_web_geconv_step(void)
{
    int r = geconvStep();
    if (r < 0)
        sysLogPrintf(LOG_ERROR, "geconv: %s", geconvError());
    return r;
}

EMSCRIPTEN_KEEPALIVE uint32_t ge_web_geconv_blob(int which, int wantLen)
{
    const GeconvResult *r = geconvResult();
    const GeconvBlob *b = which == 0 ? &r->models : which == 1 ? &r->modelsManifest :
                          which == 2 ? &r->cg : &r->cgManifest;
    return wantLen ? (uint32_t)b->len : (uint32_t)(uintptr_t)b->data;
}

EMSCRIPTEN_KEEPALIVE int ge_web_geconv_version(void) { return GECONV_VERSION; }

EMSCRIPTEN_KEEPALIVE void ge_web_geconv_end(void) { geconvEnd(); }
