/*
 * romfill.c — boot-time "fill" half of template + fill (docs/web/ROMDATA.md).
 *
 * The shipped ge007.wasm is a template: every run of bytes in the game's and assets' data
 * symbols that also exists in the ROM (in the ROM image, or in one of its RZ-compressed streams,
 * possibly byte-swapped per 16/32-bit unit) was zeroed by tools_web/romfill.c at build time,
 * and ge007.romfill lists where each run comes from (offsets only). The page calls
 * ge_web_romfill() with the player's ROM before main(), so the game never sees the zeros.
 *
 * Checks: the CRC32 of the filled bytes (entry order) and of every listed data symbol's whole
 * extent must equal the values the build tool computed from the original binary; otherwise the
 * fill is reported as failed (the page stops: the game would run on wrong data).
 */
#include <stddef.h>  /* port/shim/stddef.h: size_t before the Emscripten headers */
#include <emscripten.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "realzlib.h"
#include "system.h"

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* inflate the RZ stream at rom+off (0x11 0x72 + raw deflate) */
static uint8_t *rf_inflate(const uint8_t *rom, uint32_t romlen, uint32_t off, uint32_t *outlen)
{
    if (off + 2 > romlen || rom[off] != 0x11 || rom[off + 1] != 0x72)
        return NULL;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK)
        return NULL;
    uint32_t cap = 1 << 16, len = 0;
    uint8_t *b = malloc(cap);
    zs.next_in = (Bytef *)(rom + off + 2);
    zs.avail_in = romlen - off - 2;
    int r;
    for (;;) {
        if (len == cap) {
            cap *= 2;
            uint8_t *nb = realloc(b, cap);
            if (!nb) { free(b); inflateEnd(&zs); return NULL; }
            b = nb;
        }
        zs.next_out = b + len;
        zs.avail_out = cap - len;
        r = inflate(&zs, Z_NO_FLUSH);
        len = cap - zs.avail_out;
        if (r == Z_STREAM_END) break;
        if (r != Z_OK && !(r == Z_BUF_ERROR && zs.avail_out == 0)) break;
    }
    inflateEnd(&zs);
    if (r != Z_STREAM_END) { free(b); return NULL; }
    *outlen = len;
    return b;
}

/* byte i of the stream seen with `order` (0 as is, 1 = 32-bit units swapped, 2 = 16-bit) */
static inline uint8_t rf_at(const uint8_t *s, uint32_t n, uint32_t i, int order)
{
    if (order == 1 && (i | 3) < n) return s[(i & ~3u) + 3 - (i & 3)];
    if (order == 2 && (i | 1) < n) return s[i ^ 1];
    return s[i];
}

/* JS passes 32-bit addresses (all below 2 GiB, see PLAN.md 2.2). Returns 0 on success. */
EMSCRIPTEN_KEEPALIVE int ge_web_romfill(uint32_t romAddr, uint32_t romLen, uint32_t tabAddr, uint32_t tabLen)
{
    const uint8_t *rom = (const uint8_t *)(uintptr_t)romAddr;
    const uint8_t *tab = (const uint8_t *)(uintptr_t)tabAddr;
    if (tabLen < 24 || rd32le(tab) != 0x46524547u || rd32le(tab + 4) != 1) {
        sysLogPrintf(LOG_ERROR, "romfill: bad table");
        return -1;
    }
    uint32_t count = rd32le(tab + 8), crcFillWant = rd32le(tab + 12), crcSymsWant = rd32le(tab + 16);
    uint32_t nsyms = rd32le(tab + 20);
    const uint8_t *symtab = tab + 24, *ent = symtab + 8 * (size_t)nsyms;
    if (24 + 8 * (size_t)nsyms + 20 * (size_t)count != tabLen) {
        sysLogPrintf(LOG_ERROR, "romfill: table size mismatch");
        return -1;
    }
    /* one inflated copy per RZ stream, cached while consecutive entries use it */
    uint32_t curOff = 0xffffffffu, curLen = 0;
    uint8_t *cur = NULL;
    uint32_t streams = 0, bytes = 0;
    uLong crc = crc32(0, NULL, 0);
    for (uint32_t i = 0; i < count; i++, ent += 20) {
        uint32_t dst = rd32le(ent), len = rd32le(ent + 4), romOff = rd32le(ent + 8), srcOff = rd32le(ent + 12);
        int kind = ent[16], order = ent[17];
        const uint8_t *s;
        uint32_t sn;
        if (kind == 0) {
            s = rom; sn = romLen;
        } else {
            if (romOff != curOff) {
                free(cur);
                cur = rf_inflate(rom, romLen, romOff, &curLen);
                curOff = romOff;
                streams++;
                if (!cur) {
                    sysLogPrintf(LOG_ERROR, "romfill: RZ stream at 0x%x does not inflate", romOff);
                    return -1;
                }
            }
            s = cur; sn = curLen;
        }
        if ((uint64_t)srcOff + len > sn) {
            free(cur);
            sysLogPrintf(LOG_ERROR, "romfill: entry %u out of range", i);
            return -1;
        }
        uint8_t *d = (uint8_t *)(uintptr_t)dst;
        if (order == 0) memcpy(d, s + srcOff, len);
        else for (uint32_t k = 0; k < len; k++) d[k] = rf_at(s, sn, srcOff + k, order);
        crc = crc32(crc, d, len);
        bytes += len;
    }
    free(cur);
    uLong crcs = crc32(0, NULL, 0);
    for (uint32_t i = 0; i < nsyms; i++)
        crcs = crc32(crcs, (const uint8_t *)(uintptr_t)rd32le(symtab + 8 * i), rd32le(symtab + 8 * i + 4));
    int ok = (uint32_t)crc == crcFillWant && (uint32_t)crcs == crcSymsWant;
    sysLogPrintf(ok ? LOG_INFO : LOG_ERROR,
                 "romfill: %u ranges, %u bytes from the ROM (%u RZ streams inflated) into %u data symbols; "
                 "crc fill %08x (want %08x), symbols %08x (want %08x): %s",
                 count, bytes, streams, nsyms, (unsigned)crc, crcFillWant, (unsigned)crcs, crcSymsWant,
                 ok ? "identical to the original build" : "MISMATCH");
    return ok ? 0 : -2;
}

/* malloc/free with 32-bit addresses for the page (MEMORY64=2 makes pointers BigInt in JS) */
EMSCRIPTEN_KEEPALIVE uint32_t ge_web_alloc(uint32_t size) { return (uint32_t)(uintptr_t)malloc(size ? size : 1); }
EMSCRIPTEN_KEEPALIVE void ge_web_free(uint32_t addr) { free((void *)(uintptr_t)addr); }
