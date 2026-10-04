// The cup roll, measured: runs the PHYS block of UI.html in node over N seeds.  node roll_test.mjs [UI.html] [seeds]
import fs from 'fs';
const src = fs.readFileSync(process.argv[2] || new URL('./UI.html', import.meta.url).pathname, 'utf8');
const code = src.slice(src.indexOf('// ==== PHYS begin'), src.indexOf('// ==== PHYS end'));
const hash = (ix, iy, seed) => { let h = Math.imul(ix, 0x8da6b343) ^ Math.imul(iy, 0xd8163841) ^ Math.imul(seed, 0xcb1ab31f); h = Math.imul(h ^ (h >>> 15), 0x2c1b3c6d); h = Math.imul(h ^ (h >>> 12), 0x297a2d39); h ^= h >>> 15; return (h >>> 0) / 4294967296; };
const { PHYS, CUPROLL } = new Function(code + '\nreturn { PHYS, CUPROLL };')();
const N = +(process.argv[3] || 100), R = 59, spec = { mcx: 179, mcy: 540, R, rc: R * .72, h: R * 1.8, d: 24, ring: 34.7 };
const settles = []; let same = 0, opp = 0, total = 0, worst = 1e9, worstSeed = 0, settleMax = 0, forced = 0, faces = [0, 0, 0, 0, 0, 0, 0], notIdle = 0, speedSum = 0, speedN = 0, stackedAtSlam = 0, outAfter = 0;
const t0 = Date.now();
for (let s = 0; s < N; s++) {
  const sim = CUPROLL.make(spec, 1000 + s * 131, hash);
  let margin = 1e9, prevSettled = 0, lastFree = 0, pre = null;
  const AX = [[0,1],[0,-1],[1,1],[1,-1],[2,1],[2,-1]], upIdx = b => { const uf = PHYS.upFace(b); return AX.findIndex(([a, g]) => a === uf.axis && g === uf.sign); };
  while (sim.T() < 7 && sim.phase() !== 'idle') {
    sim.advance(1 / 60);
    const T = sim.T();
    if (!pre && sim.phase() === 'flip') pre = sim.bodies.map(upIdx);
    if (T < sim.times.T_SLAM - .01) { const m = sim.mouthMargin(); if (m < margin) margin = m; }
    if (sim.phase() === 'shake') { for (const b of sim.bodies) { speedSum += Math.hypot(...b.v); speedN++; } }
    if (sim.phase() === 'settle' && T > sim.times.T_SLAM + 2.95 && T < sim.times.T_SLAM + 3) lastFree = sim.bodies.filter(b => !b.settled).length;
  }
  if (lastFree) forced += lastFree;
  sim.bodies.forEach((b, i) => { const f = upIdx(b); total++; if (f === pre[i]) same++; if (f === (pre[i] ^ 1)) opp++; });
  if (sim.phase() !== 'idle') notIdle++;
  const settleT = sim.T() - sim.times.T_SLAM; settles.push(settleT); if (settleT > settleMax) settleMax = settleT;
  if (margin < worst) { worst = margin; worstSeed = s; }
  // every die under the mouth's footprint, on the table?
  for (const b of sim.bodies) { const uf = PHYS.upFace(b); const ax = [[0,1],[0,-1],[1,1],[1,-1],[2,1],[2,-1]]; faces[1 + ax.findIndex(([a, g]) => a === uf.axis && g === uf.sign)]++; if (Math.hypot(b.p[0] - spec.mcx, b.p[1] - spec.mcy) > R - spec.d * .5 * 1.2) outAfter++; }
}
console.log(`seeds ${N}  ms/seed ${((Date.now() - t0) / N).toFixed(0)}`);
console.log(`mouth margin min ${worst.toFixed(1)} (seed ${worstSeed})  (0 = a corner at the mouth plane; negative = out)`);
console.log(`settle: slowest ${settleMax.toFixed(2)} s after the slam; forced ${forced}; not idle by 7 s: ${notIdle}; dice outside the mouth's footprint after: ${outAfter}`);
settles.sort((a, b) => a - b); console.log(`settle after the slam: median ${settles[settles.length >> 1].toFixed(2)} s, 90% ${settles[(settles.length * .9) | 0].toFixed(2)} s`);
console.log(`mean die speed while shaking ${(speedSum / speedN).toFixed(0)} pt/s`);
console.log(`final up face vs the face up before the flip: same ${(same / total * 100).toFixed(0)}%  opposite ${(opp / total * 100).toFixed(0)}%  (fair: 17% each)`);
console.log('up-face axis histogram (+x,-x,+y,-y,+z,-z):', faces.slice(1).join(' '));
