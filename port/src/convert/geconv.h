/*
 * geconv — the PC sidecar converters (tools_pc/d43_emit.py, d69_emit.py, d88_emit.py +
 * d88_propdefs.py) ported to C, so the web build can produce data/pcmodels-<region>/ and
 * data/pccg-<region>/ from the player's ROM in the browser (docs/web/PLAN.md 3.2, W2).
 *
 * Output is byte-identical to the Python converters (checked by scripts/web/geconv_check.sh).
 * Pure C99 + zlib; no libc/game headers (the decomp shadows several), so the same file builds into the wasm game and into the
 * native-style check tool (tools_web/geconv_cli.c).
 *
 * Inputs: the ROM image (z64 byte order), the region's filelist CSV text (scripts/filelist.*.csv,
 * offsets + names), the text of assets/obseg/file_resource_table.inc.c (names, manifest order)
 * and a callback giving each model's (numSwitches, numTextures) from its ModelFileHeader.
 *
 * Use: geconvBegin(); while ((r = geconvStep()) > 0) show progress; r < 0 = error (geconvError());
 * then the two images + manifests are in geconvResult(); geconvEnd() frees everything.
 */
#ifndef GECONV_H
#define GECONV_H

#define GECONV_VERSION 1   /* bump when the output format changes (IndexedDB cache key) */

typedef int (*GeconvHeaderFn)(void *ctx, const char *key, int *numSwitches, int *numTextures);

typedef struct GeconvInput {
    const unsigned char *rom;
    unsigned long romlen;
    const char *filelist;          /* scripts/filelist.<r>.csv text */
    unsigned long filelistlen;
    const char *restable;          /* assets/obseg/file_resource_table.inc.c text */
    unsigned long restablelen;
    GeconvHeaderFn header;         /* model header counts by header symbol stem */
    void *headerctx;
} GeconvInput;

typedef struct GeconvBlob {
    unsigned char *data;
    unsigned long len, cap;
} GeconvBlob;

typedef struct GeconvResult {
    GeconvBlob models;             /* pcmodels.bin */
    GeconvBlob modelsManifest;     /* manifest.csv (name,offset,size, CRLF like Python's csv) */
    GeconvBlob cg;                 /* pccg.bin */
    GeconvBlob cgManifest;
} GeconvResult;

int geconvBegin(const GeconvInput *in);   /* returns the number of steps, < 0 on error */
int geconvStep(void);                     /* > 0: steps left, 0: done, < 0: error */
int geconvStepsTotal(void);
const char *geconvError(void);
const GeconvResult *geconvResult(void);
void geconvEnd(void);

#endif
