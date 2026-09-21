/* THE PORT, HELD AGAINST THE ORIGINAL.
 *
 *     make -C uttt/c rough-diff
 *
 * uttt_pen.c is rough.js transcribed into C, and the rulebook button is the
 * shape that uses all of it - a bowed double line, a scan-line hachure, a
 * seeded stream shared by a fill and an outline. So this runs the VENDORED
 * rough.js (docs/vendor/rough.umd.js, the same file the design document is
 * drawn with), flattens its cubics exactly the way the C flattens them, and
 * compares every sample with what the kernel emits.
 *
 * It is the only thing that can catch a drift that still looks like a
 * drawing: swap two random draws and the button is still a hachured square
 * with a book on it, still the right colours, still the right number of
 * strokes - just not the one that was approved. The smoke test cannot see
 * that, because the smoke test does not know what rough.js would have done.
 *
 * Needs node, so it is its own target and not part of `make all`.
 */
import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const src = readFileSync(join(here, '../../docs/vendor/rough.umd.js'), 'utf8');
const rough = new Function(src + '; return rough;')();

const SIDE = 54;    /* the button that ships */
const SEG  = 14;    /* uttt_rule.c's flattening */

/* ---- what rough.js draws, as centrelines ------------------------------- */
function flatten(sets) {
  const runs = [];
  for (const set of sets)
    for (const op of set.ops) {
      if (op.op === 'move') { runs.push([[op.data[0], op.data[1]]]); continue; }
      if (op.op !== 'bcurveTo') throw new Error('unexpected op ' + op.op);
      const run = runs[runs.length - 1], p0 = run[run.length - 1];
      const [x1, y1, x2, y2, x3, y3] = op.data;
      for (let i = 1; i <= SEG; i++) {
        const t = i / SEG, u = 1 - t;
        run.push([u*u*u*p0[0] + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3,
                  u*u*u*p0[1] + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3]);
      }
    }
  return runs;
}

const W = SIDE, H = SIDE, gen = rough.generator();
const pg = 'rgba(226,232,244,.55)';
const shapes = [
  /* docs/UI.html, initRbook() - copied, not retyped */
  gen.rectangle(4, 4, W - 8, H - 8, {
    fill: '#25376b', fillStyle: 'hachure', hachureGap: 4.2, fillWeight: 1.4,
    hachureAngle: -41, stroke: '#1b2a52', strokeWidth: 1.8,
    roughness: 1.5, bowing: 1.3, seed: 19 }),
  gen.polygon([[15, 17], [W/2, 20], [W/2, H - 15], [15, H - 18]],
    { fill: pg, fillStyle: 'hachure', hachureGap: 2.6, fillWeight: .9,
      hachureAngle: 38, stroke: pg, strokeWidth: 1.3, roughness: 1.3, seed: 23 }),
  gen.polygon([[W - 15, 17], [W/2, 20], [W/2, H - 15], [W - 15, H - 18]],
    { fill: pg, fillStyle: 'hachure', hachureGap: 2.6, fillWeight: .9,
      hachureAngle: -38, stroke: pg, strokeWidth: 1.3, roughness: 1.3, seed: 29 }),
];

/* A scan line through a corner is a line of no length; rough.js emits it and
 * jitters it by nothing, so neither side has anything to draw. It is not
 * EXACTLY zero on either side - a cubic whose four points coincide still
 * rounds - so this asks whether it is shorter than a thousandth of a point,
 * where the shortest stroke the button really has is seventy times that. */
const live = ([a, b]) => Math.abs(a[0] - b[0]) > 1e-3 || Math.abs(a[1] - b[1]) > 1e-3;
const want = [];
for (const s of shapes)
  for (const run of flatten(s.sets))
    for (let i = 0; i + 1 < run.length; i++)
      if (live([run[i], run[i + 1]])) want.push([...run[i], ...run[i + 1]]);

/* ---- what the kernel draws --------------------------------------------- */
const out = execFileSync(join(here, '../build/uttt_render'),
                         ['rulebook', String(SIDE), 'dump'], { encoding: 'utf8' });
const got = out.trim().split('\n').map(l => l.trim().split(/\s+/).map(Number))
               .filter(q => live([[q[0], q[1]], [q[2], q[3]]]));

/* ---- and they are the same shape --------------------------------------- */
let worst = 0, bad = 0;
for (let i = 0; i < Math.min(want.length, got.length); i++) {
  const d = Math.max(...want[i].map((v, k) => Math.abs(v - got[i][k])));
  if (d > worst) worst = d;
  if (d > 0.01 && bad++ < 5)
    console.log(`  segment ${i}: rough.js ${want[i].map(v => v.toFixed(3))}`
              + `  kernel ${got[i].map(v => v.toFixed(3))}`);
}
const same = want.length === got.length && bad === 0;
console.log(`  rough.js ${want.length} segments, kernel ${got.length},`
          + ` worst disagreement ${worst.toFixed(5)} points`);
console.log(same ? '\nthe port is the original\n'
                 : `\n${bad || 'LENGTH'} MISMATCHED\n`);
process.exit(same ? 0 : 1);
