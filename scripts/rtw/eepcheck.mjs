// Check a GoldenEye EEPROM image in the cart's native (big-endian) format: the six checksummed regions
// (block 0 smallSave, five save_data slots) against the game's own CRC (src/game/crc.c fileGenerateCRC with
// port/src/random.c randomGetNextFrom). Also reports which regions would validate in the PC port's
// little-endian order.   node scripts/rtw/eepcheck.mjs <file.eep>      (or import { regions })
import { readFileSync } from 'node:fs';
const M64 = (1n << 64n) - 1n;
function next(seed) {
  let a3 = seed.v, a2 = (a3 << 63n) & M64, a1 = (a3 << 31n) & M64;
  a2 >>= 31n; a1 >>= 32n; a3 = (a3 << 44n) & M64; a2 |= a1; a3 >>= 32n; a2 ^= a3;
  a3 = (a2 >> 20n) & 0xfffn; a3 ^= a2; seed.v = a3;
  return Number(a3 & 0xffffffffn) | 0;
}
export function crc(b, from, to) {
  const s = { v: 0x8F809F473108B3C1n };
  let shift = 0, c1 = 0, c2 = 0;
  for (let i = from; i < to; i++, shift += 7) { s.v = (s.v + BigInt(b[i] << (shift & 15))) & M64; c1 ^= next(s); }
  for (let i = to - 1; i >= from; i--, shift += 3) { s.v = (s.v + BigInt(b[i] << (shift & 15))) & M64; c2 ^= next(s); }
  return [c1 >>> 0, c2 >>> 0];
}
export function regions(b) {
  const out = [];
  for (let i = 0; i < 6; i++) {
    const off = i === 0 ? 0 : 32 + (i - 1) * 96, len = i === 0 ? 32 : 96;
    const [c1, c2] = crc(b, off + 8, off + len);
    const dv = new DataView(b.buffer, b.byteOffset + off, 8);
    out.push({ region: i, native: dv.getUint32(0, false) === c1 && dv.getUint32(4, false) === c2,
               port: dv.getUint32(0, true) === c1 && dv.getUint32(4, true) === c2 });
  }
  return out;
}
if (import.meta.url === `file://${process.argv[1]}`) {
  const b = new Uint8Array(readFileSync(process.argv[2]));
  console.log(`${b.length} bytes`);
  for (const r of regions(b)) console.log(`region ${r.region}: native ${r.native ? 'valid' : '-'}, port order ${r.port ? 'valid' : '-'}`);
}
