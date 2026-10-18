// Runs each fm_exp wasm module (tests/fm_exp_wasm_test.c) and fails unless its
// digest is the committed one the native suite is held to. On a mismatch it
// prints the first input whose bits differ from the native build's, when the
// native list is given (build/fm_exp_native_bits, one hex word per line).
import { readFileSync, existsSync } from 'node:fs';

const [, , nativeBits, ...modules] = process.argv;
const native = existsSync(nativeBits)
  ? readFileSync(nativeBits, 'utf8').trim().split('\n').map((h) => BigInt('0x' + h))
  : null;
let failed = 0;
for (const path of modules) {
  const { instance } = await WebAssembly.instantiate(readFileSync(path), {});
  const e = instance.exports;
  const got = BigInt.asUintN(64, e.fm_exp_digest_value());
  const want = BigInt.asUintN(64, e.fm_exp_digest_expected());
  let firstDiff = -1;
  if (native) {
    for (let i = 0; i < native.length; i++) {
      if (BigInt.asUintN(64, e.fm_exp_bits(i)) !== native[i]) { firstDiff = i; break; }
    }
  }
  const ok = got === want && firstDiff < 0;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${path}: digest 0x${got.toString(16)}, committed 0x${want.toString(16)}` +
    (native ? `, ${firstDiff < 0 ? `all ${native.length} native bit patterns match` : `input ${firstDiff} differs from native`}` : ''));
  if (!ok) failed++;
}
process.exit(failed ? 1 : 0);
