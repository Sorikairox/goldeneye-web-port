# ROM-derived data in the web build: inventory, mechanisms, gates

Rule (user decision 2026-10-29, `orchestrator.md` section 2): bulk media (logos, textures, fonts,
audio) and game data tables (weapon stats, model headers, skeletons, image tables, setup data)
compiled into the binary are ROM data and must come from the player's ROM in the browser. Small
initialised constants inside the decompiled game C files count as code and may stay. The shipped
site is scanned for ROM byte runs before every deploy.

State after G4 (2026-10-30). Numbers are for the EU ROM (SHA-1 `167c3c43...`).

## 1. What the page builds from the player's ROM, before `main()`

| Step | Code | What |
|---|---|---|
| 1. fill | `port/src/web/romfill.c` (`ge_web_romfill`) | the template binary's zeroed data (section 3) from `ge007.romfill`; CRC32 of the filled bytes and of every listed symbol checked against the original build ("identical to the original build", else the page stops) |
| 2. sidecars | `port/src/convert/geconv.c` via `port/src/web/geconvweb.c` | `pcmodels-pal-final/` and `pccg-pal-final/` (models, bg segs, stan, stage setups) converted in wasm, ~620 steps, ~225 ms in headless Chrome, progress bar; cached in IndexedDB (`sidecars-v<GECONV_VERSION>-<ROM SHA-1>`); written to MEMFS `/data/` for `pcmodels.c` / `pccg.c` |
| 3. ROM | `romdata.c` (unchanged) | the ROM itself in the cart window, as before |

`port/web/main.js` drives the three; `?assets=URL` (Python dev sidecars from a local server) is a
dev-only override, never deployed.

## 2. Sidecars: C port of the Python converters

`port/src/convert/geconv.c` is a line-by-line port of `tools_pc/d43_emit.py` (models),
`d69_emit.py` (bg + stan) and `d88_emit.py` + `d88_propdefs.py` (setups, the `--regen` order), with
the same orderings (dict insertion order, stable sorts, last-write-wins maps) and the same RZ framing
(`0x11 0x72` + raw deflate level 6, memLevel 8). Not ported: d43's diagnostic round-trip validation
and buffer-fit cross-checks (they never change the output).

Inputs: the ROM; `scripts/filelist.<r>.csv` and `file_resource_table.inc.c` as texts (names and
offsets, generated into `geconv_inputs.c` by `scripts/web/gen_geconv_inputs.py`); each model's
numSwitches / numtextures from the game's own `<stem>_header` objects (filled in step 1).

Gate: `scripts/web/geconv_check.sh` builds `tools_web/geconv_cli.c` with emcc (the game's zlib) and
compares with the Python output: pcmodels.bin (1,320,288 B), pccg.bin (3,658,258 B) and both
manifests identical. In the page, `&fillcheck` logs the SHA-256 of the four files produced inside the
game wasm: identical to the Python files too. US/JP code paths are kept (filelist / region switch)
but untested (no ROMs).

## 3. Template + fill for data compiled into the binary

Build half, `tools_web/romfill.c` (run by `scripts/web/romfill.sh` from `build.sh` after the link):

- **Selection:** every data symbol of the wasm-ld map from `src/`, `srcfix/` (build copies of game
  files) and `assets/` objects, the linker-merged string pool `<internal>:(.rodata)`, and the read-only
  tables of the port's C files (not `port/src/web/`, not C++: constructors run before the fill;
  not port `.data`: constructors may change it). EU: 2,336 symbols, 836,995 bytes, 170,256 non-zero.
- **Corpus:** the ROM, every RZ stream in it (6,186 streams, 15.7 MB decompressed: models, setups,
  bg rooms, the game's compressed data segment at ROM 0x1F850, ...), each also with every 32-bit word
  and every 16-bit half byte-swapped.
- **Pass 1:** byte runs >= 16 bytes (with a non-zero byte) shared with the corpus (hash index of
  8-byte blocks, rolling hash, extended both ways).
- **Pass 2:** display lists (PC `Gfx` = two u64 holding the N64 words: rebuilt N64 image matched
  word by word); then anchored pieces: walking each symbol in address order, the next unfilled
  non-zero, non-pointer byte is looked up near the predicted source offset in the four most recently
  used streams, as single-order matches or as mixed 4-byte units (each unit as is / 32-bit / 16-bit
  swapped, or split into two 16-bit halves), longest match wins. Pointer-like slots (8-aligned values
  in the data / cart / DRAM address ranges) are never filled, so relocation stays exactly as linked.
- **Output:** `build-web/site/ge007.wasm` with the filled bytes zeroed in its data section (the
  template) and `build-web/site/ge007.romfill` (288 KB, offsets only: dst address, length, ROM offset
  of the raw ROM or RZ stream, offset inside, byte order, plus two CRC32s and the symbol extents),
  sorted by source stream (20 RZ streams to inflate at boot). The tool applies its own table to the
  template and refuses to write unless the result equals the original image.

EU result: 13,462 ranges, 155,056 bytes filled (82,093 of them non-zero); the template differs from
the original in 82,093 bytes.

Gates:
- runtime CRC check in `ge_web_romfill` (filled bytes + whole symbol extents);
- `scripts/web/fillcheck.py ORIGINAL.wasm site/ge007.romfill` vs the page's `&fillcheck` line:
  SHA-256 over all 2,336 symbol extents (836,995 bytes) computed independently in Python from the
  unstripped binary and in the page right after the fill (before `main`): equal;
- `scripts/web/route_dam.sh` (power-on to Dam gameplay) with the stripped binary + browser sidecars.

Without `data/ge007.<region>.z64` the build fails (and deletes the site's wasm); `GE_NOSTRIP=1` builds
an unstripped dev site marked `DEV-UNSTRIPPED`. A stripped build needs the dev ROM copy at build time
(the fill table is produced from it); the table itself holds no ROM bytes.

## 4. Deploy gate

`scripts/web/deploy_check.sh [site]` must exit 0 before any deploy: no `DEV-UNSTRIPPED` marker,
`ge007.romfill` present, no ROM / sidecar files in the site, and `scripts/web/romscan.sh` passes:
`tools_web/romscan.c` finds runs >= 64 bytes shared with the same corpus as above (32-byte aligned
blocks, so no run >= 63 is missed; runs with < 8 distinct byte values ignored), and
`tools_web/romscan_attr.py` maps every hit in the wasm to its data symbols through the link map; only
`scripts/web/romscan.allow` entries are accepted (third-party constants that coincide with ROM bytes:
zlib's deflate base tables, an ASCII run in two SDL keymaps).

| romscan of build-web/site (runs >= 64 B) | runs | bytes |
|---|---|---|
| before G4 (G2 build, everything compiled in) | 178 | 58,097 |
| after G4 (template + fill) | 4 | 280, all allowlisted |

At 16 bytes (`-DBLK=8`, audit only) the remaining hits are names: the filelist / file-table texts the
converter needs (`geconv_inputs.c`, class A of PLAN 3.1), ~50 short string-pool runs and single runs
of a few game tables (< 60 B each).

## 5. What is left (EU, after G4)

Not ROM data in the shipped site any more: logos (`rarewarelogo.c`: 15,772 of 15,926 non-zero bytes
filled; the rest are pointers / 18 B), `oddtextures.c` display lists, weapon tables, the global image
table (`image.c`), skeletons, blood/sine/asin tables, audio tables (`drvrNew`, `env`, the port mixer's
resample table), VI modes. `assets/font_chardata{e,j}.c`, `font_dl.c` and the `assets/font/*.c` fonts
are not linked at all (the game reads fonts from the ROM), so they never reach the wasm.

Still in the binary, by object (non-zero bytes left that are neither filled nor pointer-like):

| object | symbols | non-zero | filled | pointer-like | left |
|---|---|---|---|---|---|
| `<internal>` (merged string pool, all TUs incl. SDL/libc/port) | 1 | 88,395 | 21,170 | 0 | 67,225 |
| `src/game/ob.c` (`file_resource_table`: ids + name pointers) | 4 | 6,625 | 46 | 5,340 | 1,239 |
| `src/game/pobjdata.c` (`PitemZ_entries`, prop headers) | 368 | 8,444 | 4,152 | 3,269 | 1,023 |
| `src/game/mp_weapon.c` (MP weapon sets) | 16 | 767 | 33 | 56 | 678 |
| `src/game/bg.c` (`levelinfotable`, fog DL LUTs) | 73 | 1,807 | 937 | 439 | 431 |
| `src/game/gun.c` (`gitem_structs`) | 99 | 5,929 | 4,716 | 862 | 351 |
| `src/game/chr.c` | 148 | 6,529 | 5,198 | 1,018 | 313 |
| `src/game/cobjdata.c` (model headers, `c_item_entries`) | 82 | 5,170 | 4,185 | 727 | 258 |
| `src/game/propobj.c` | 108 | 2,129 | 1,616 | 327 | 186 |
| others (each < 150) | 1,437 | | | | 1,195 |

Model headers (513 `*_header` objects): 3,269 of 5,200 non-zero bytes filled, 1,377 pointer bytes,
~550 left (counts whose N64 location the anchored search does not reach).

Nature of what is left: short strings (file names, debug/format strings: code-like constants, and
mostly not game strings at all: SDL, libc, the port), and small scattered integers inside game tables
whose PC layout differs from the N64 one (pointer widening, `#ifdef PORT` re-layouts): ids, flags,
1-2 byte fields between pointers. None of it forms a run of 16 bytes or more with the ROM.

## 6. Next steps (recommended)

1. Struct-aware fill for the tables in section 5 (file_resource_table, PitemZ_entries,
   c_item_entries, levelinfotable, mp weapon sets, model headers): generate the N64 layout of each
   table from the type (clang `-ast-dump=json` or DWARF of the object files: the final wasm's DWARF is
   unusable after wasm-opt) and fill field by field; or decide with the user that these residual
   fields (ids, counts, flags) are "small constants".
2. Game strings: map `<internal>:(.rodata)` pieces back to their TUs (link map of the object files'
   `.rodata.str*` sections) and fill game-TU strings from the data segment; leave library strings.
3. Make the stripped build reproducible without the ROM: commit the fill table keyed by
   (symbol, offset) instead of absolute addresses, so builds on machines without a ROM produce the
   same template (romfill.c already has everything per symbol).
4. Shrink the table (288 KB; ~13k entries of 20 B): delta-encode and deflate it.
