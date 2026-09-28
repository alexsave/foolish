// The wasm side of 7.3.7 (tests/pk_cross.c): instantiate the wasm32 build of
// the kernel and the cross harness, play the games there, and print one line
// a game exactly as the native program does, for `make cross` to compare.
//
//     node tests/pk_cross.mjs build/pk_cross.wasm [games]
import { readFileSync } from "node:fs";

const [file, gamesArg] = process.argv.slice(2);
const games = Number(gamesArg ?? 100);
const { instance } = await WebAssembly.instantiate(readFileSync(file), {});
const { memory, pk_cross_run, pk_cross_hashes } = instance.exports;
const n = pk_cross_run(games);
const at = pk_cross_hashes();
const hashes = new BigUint64Array(memory.buffer, at, n);
const out = [];
for (let k = 0; k < n; k++) out.push(`${k} ${hashes[k].toString(16).padStart(16, "0")}`);
process.stdout.write(out.join("\n") + "\n");
process.exit(n === games ? 0 : 1);
