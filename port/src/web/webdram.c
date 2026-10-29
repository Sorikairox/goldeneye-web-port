/*
 * webdram.c — the web build's replacements for port/src/dram_syms.S
 * (see webdram.h). Same offsets inside the arena as the native symbols.
 * The names are spelled through the macros so this file defines exactly the
 * objects the game TUs reference; their declared types differ per TU, which
 * is harmless for wasm data symbols (all are one pointer wide).
 */
#include "portaddr.h"

void *ge_web_cfb_16                  = (void *)(PORT_DRAM_V1_BASE + 0x0);
void *ge_web_bssSegmentEnd           = (void *)(PORT_DRAM_V1_BASE + 0x50000);
