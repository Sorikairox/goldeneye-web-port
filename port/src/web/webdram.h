/*
 * webdram.h — force-included into every TU of the web build (GE_WEB).
 *
 * The native builds define three game symbols as ABSOLUTE addresses inside the
 * N64-DRAM arena (port/src/dram_syms.S): cfb_16, _bssSegmentEnd and
 * animations_frame_buffer. wasm object files cannot carry absolute data
 * symbols, so on the web each name is turned into a dereferenced pointer that
 * holds the same address. The game's own declarations stay valid C:
 *
 *   extern u8 cfb_16[2][N];        -> extern u8 (*ge_web_cfb_16)[2][N];
 *   extern u32 *_bssSegmentEnd;    -> extern u32 *(*ge_web_bssSegmentEnd);
 *
 * and every use (`cfb_16[i]`, `&_bssSegmentEnd`, `sizeof cfb_16`) means the
 * same object at the same address as on native. The pointers are defined in
 * port/src/web/webdram.c. No game file is edited.
 */
#ifndef PORT_WEB_WEBDRAM_H
#define PORT_WEB_WEBDRAM_H
#define cfb_16 (*ge_web_cfb_16)
#define _bssSegmentEnd (*ge_web_bssSegmentEnd)
/* animations_frame_buffer is used in a static initializer (initanitable.c), where
 * a pointer variable is not a constant: scripts/web/web_srcfix.py --abs gives that
 * one TU a constant-address macro instead. */
#endif
