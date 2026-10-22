// gzip.mjs - writes a .gz twin beside every text file of the export, at the highest level, so the
// server answers gzip without compressing on the request path (server/src/web.c).
import { readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { gzipSync, constants } from 'node:zlib';

const root = process.argv[2] ?? 'out';
const TEXT = /\.(html|js|css|svg|json|txt|wasm|webmanifest)$/;
let n = 0, raw = 0, gz = 0;
const walk = (dir) => {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) { walk(p); continue; }
    if (!TEXT.test(name)) continue;
    const body = readFileSync(p);
    const z = gzipSync(body, { level: constants.Z_BEST_COMPRESSION });
    if (z.length < body.length) { writeFileSync(p + '.gz', z); n++; raw += body.length; gz += z.length; }
  }
};
walk(root);
console.log(`gzip: ${n} files, ${raw} B -> ${gz} B`);
