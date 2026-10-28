// Texture timing, paired and interleaved: the port's page against the React app's own texture code,
// cold (nothing stored: every texture made) and warm (all three read back from the store), load by
// load in one Chrome, so a busy machine weighs on every arm alike. Each load starts from a blank
// page in a fresh renderer (as timing.mjs BLANK=1).
//   node tools/textime.mjs [loads] [cpu-throttle]
// Arms every round: the port with ?notex (first paint without textures), then the port and the TS
// cold, then both warm (which goes first alternates). The stores are emptied before the cold loads.
// The TS arm (out/ts_tex.html) runs the app's functions as the app starts them on mount, each
// checking its TextureCacheDB first: generateFernPattern (fernFractal.tsx), WoodTexture's
// generateWoodTexture (WebGL), and WoolBackground's generateWoolTexture, whose weave runs on the
// app's wool worker (woolWorker.ts, bundled as the app's worker). Times are ms after navigation:
// when each texture's url was ready (the port: set on the mount; the TS: its promise resolved),
// and when all had decoded (the port's four: its wood is two pictures, the end screen's made last). The port's figures also give its first contentful paint.
// BEFORE=dir (a build of an earlier port, under foolish/c/) adds that build, cold, to the first
// three rounds: only when all three textures had decoded is read from it.
import fs from 'node:fs';
import path from 'node:path';
import { execSync } from 'node:child_process';
import { launch, serve, FOOLISH, POC, sleep } from './cdp.mjs';

const LOADS = Number(process.argv[2] || 7), CPU = Number(process.argv[3] || 1);
const MAIN_MODULES = '/Users/alex/Dev/foolish/foolish/node_modules';
const { build } = await import(path.join(MAIN_MODULES, 'esbuild/lib/main.js'));
const out = path.join(POC, 'out');
const common = { bundle: true, nodePaths: [MAIN_MODULES], logLevel: 'error', jsx: 'automatic', define: { 'process.env.NODE_ENV': '"production"' } };
fs.writeFileSync(path.join(out, 'ts_tex_entry.ts'), `
export { generateFernPattern } from '${FOOLISH}/src/utils/fernFractal';
export { generateWoodTexture } from '${FOOLISH}/src/components/WoodTexture';
export { default as generateWoolTexture } from '${FOOLISH}/src/components/WoolBackground';
`);
await build({ ...common, entryPoints: [path.join(out, 'ts_tex_entry.ts')], format: 'iife', globalName: 'TSAPP', outfile: path.join(out, 'ts_tex.js'),
  plugins: [{ name: 'app', setup(b) {
    // generateWoodTexture and generateFernPattern are their modules' own; the wool worker's URL is the bundled worker's.
    b.onLoad({ filter: /WoodTexture\.tsx$/ }, (a) => ({ contents: fs.readFileSync(a.path, 'utf8') + '\nexport { generateWoodTexture };', loader: 'tsx', resolveDir: path.dirname(a.path) }));
    b.onLoad({ filter: /fernFractal\.tsx$/ }, (a) => ({ contents: fs.readFileSync(a.path, 'utf8') + '\nexport { generateFernPattern };', loader: 'tsx', resolveDir: path.dirname(a.path) }));
    b.onLoad({ filter: /WoolBackground\.tsx$/ }, (a) => {
      const src = fs.readFileSync(a.path, 'utf8'), from = "new URL('../utils/woolWorker.ts', import.meta.url)";
      if (!src.includes(from)) throw new Error('the wool worker moved');
      return { contents: src.replace(from, "'ts_wool_worker.js'"), loader: 'tsx', resolveDir: path.dirname(a.path) };
    });
  } }] });
await build({ ...common, entryPoints: [path.join(FOOLISH, 'src/utils/woolWorker.ts')], format: 'iife', outfile: path.join(out, 'ts_wool_worker.js') });
fs.writeFileSync(path.join(out, 'ts_tex.html'), `<!doctype html><html><head><meta charset="utf-8"></head><body>
<script src="ts_tex.js"></script><script>
const t = {}, land = (k) => (u) => { t[k] = performance.now(); return u; };
window.texAll = Promise.all([TSAPP.generateFernPattern().then(land('fern')), TSAPP.generateWoodTexture().then(land('wood')),
  TSAPP.generateWoolTexture().then(land('wool'))]).then(async (us) => {
  await Promise.all(us.map((u) => { const i = new Image(); i.src = u; return i.decode(); }));
  t.decoded = performance.now(); return t; });
</script></body></html>`);

const load = () => execSync('uptime').toString().trim().replace(/.*load averages?: /, '');
// the busiest processes over one second (top's first sample has no CPU figures)
const top = () => execSync('top -l 2 -s 1 -o cpu -n 6 -stats command,cpu').toString().trim().split('\n').slice(-6).map((l) => l.trim().replace(/\s+/g, ' ')).join('; ');
const before = { load: load(), top: top() };
const server = await serve(8720, FOOLISH, null, { 'cross-origin-opener-policy': 'same-origin', 'cross-origin-embedder-policy': 'require-corp' });
const b = await launch();
const ORIGIN = 'http://127.0.0.1:8720', PORT = ORIGIN + '/c/cdom_poc/out/index.html', TSP = ORIGIN + '/c/cdom_poc/out/ts_tex.html';
const arms = { notex: [], port_cold: [], ts_cold: [], port_warm: [], ts_warm: [], before: [] };
const blank = async () => { await b.goto('about:blank'); await sleep(300); };
const clear = () => b.send('Storage.clearDataForOrigin', { origin: ORIGIN, storageTypes: 'indexeddb' });
async function port(arm, notex) {
  await blank();
  await b.goto(PORT + (notex ? '?notex' : ''));
  // the table is up once the store has answered for the game (the board waits for it); the turn
  // may then be the bot's for its pace, which is not what this times
  await b.until('window.appReady && window.app.then((x) => x.app_game_store(1) > 0)', 30000);
  if (notex) { await sleep(300); arms[arm].push({ fcp: await b.evaluate(`performance.getEntriesByName('first-contentful-paint')[0]?.startTime`) }); return; }
  await b.until(`window.app.then((x) => x.app_tex_ms(x.app_tex_count()) === x.app_tex_count())`, 300000);
  const r = await b.evaluate(`window.app.then(async (x) => {
    const u = ['--tex-fern', '--tex-wood', '--tex-wool', '--tex-wood-end'].map((v) => document.getElementById('root').style.getPropertyValue(v).slice(4, -1));
    await Promise.all(u.map((s) => { const i = new Image(); i.src = s; return i.decode(); }));
    return { fcp: performance.getEntriesByName('first-contentful-paint')[0]?.startTime, fern: x.app_tex_at(0), wood: x.app_tex_at(1), wool: x.app_tex_at(2), woodEnd: x.app_tex_at(3), store: [0, 1, 2, 3].map((k) => x.app_tex_store(k, 1) - x.app_tex_store(k, 0)),
      decoded: performance.now(), stored: [0, 1, 2, 3].map((k) => x.app_tex_stored(k)).join(''), made: [0, 1, 2, 3].map((k) => x.app_tex_ms(k)),
      mem: x.memory.buffer.byteLength }; })`);
  const want = arm === 'port_warm' ? '1111' : '0000';
  if (r.stored !== want) throw new Error(`${arm}: stored ${r.stored}`);
  arms[arm].push(r);
  await sleep(500); // the store's puts are fire-and-forget
}
async function beforeBuild() {
  await blank();
  await b.goto(ORIGIN + '/c/' + process.env.BEFORE + '/out/index.html');
  await b.until(`window.app?.then((x) => x.app_tex_ms(3) === 3)`, 600000);
  await b.until(`document.getElementById('root').style.getPropertyValue('--tex-wool') !== ''`, 60000);
  arms.before.push(await b.evaluate(`(async () => {
    const u = ['--tex-fern', '--tex-wood', '--tex-wool'].map((v) => document.getElementById('root').style.getPropertyValue(v).slice(4, -1));
    await Promise.all(u.map((s) => { const i = new Image(); i.src = s; return i.decode(); }));
    return { decoded: performance.now(), mem: (await window.app).memory.buffer.byteLength }; })()`));
}
async function ts(arm) {
  await blank();
  await b.goto(TSP);
  const r = await b.evaluate('window.texAll');
  arms[arm].push(r);
  await sleep(500); // setCachedTexture is fire-and-forget too
}
try {
  if (CPU > 1) await b.send('Emulation.setCPUThrottlingRate', { rate: CPU });
  for (let i = 0; i < LOADS; i++) {
    // The two stores are separate databases, so one clear makes both cold loads cold; each then
    // fills its own store for the warm loads. Which arm goes first alternates round by round.
    await port('notex', true);
    if (process.env.BEFORE && i < 3) await beforeBuild();
    await clear();
    if (i % 2) { await ts('ts_cold'); await port('port_cold'); await ts('ts_warm'); await port('port_warm'); }
    else { await port('port_cold'); await ts('ts_cold'); await port('port_warm'); await ts('ts_warm'); }
  }
} catch (e) { console.error(e); process.exitCode = 1; } finally { await b.close(); server.close(); }
const after = { load: load(), top: top() };

const q = (a, p) => { const s = a.filter((v) => typeof v === 'number').sort((x, y) => x - y); return s.length ? s[Math.min(s.length - 1, Math.floor(p * (s.length - 1) + 0.5))] : NaN; };
const cell = (a) => `${q(a, 0.5).toFixed(0)} (${q(a, 0.25).toFixed(0)}-${q(a, 0.75).toFixed(0)})`;
console.log(`textures, ${LOADS} rounds, CPU x${CPU}: median (25th-75th) ms after navigation`);
console.log(`load before: ${before.load} | ${before.top}`);
console.log(`load after:  ${after.load} | ${after.top}`);
console.log(`first contentful paint: port ?notex ${cell(arms.notex.map((r) => r.fcp))}, port cold ${cell(arms.port_cold.map((r) => r.fcp))}, port warm ${cell(arms.port_warm.map((r) => r.fcp))}`);
for (const arm of ['port_cold', 'ts_cold', 'port_warm', 'ts_warm'])
  console.log(`${arm.padEnd(10)} fern ${cell(arms[arm].map((r) => r.fern)).padEnd(16)} wood ${cell(arms[arm].map((r) => r.wood)).padEnd(16)} wool ${cell(arms[arm].map((r) => r.wool)).padEnd(16)}${arm.startsWith('port') ? ' end-screen wood ' + cell(arms[arm].map((r) => r.woodEnd)).padEnd(16) : ''} all decoded ${cell(arms[arm].map((r) => r.decoded))}`);
for (const arm of ['port_cold', 'port_warm'])
  console.log(`${arm} the store's answer took: ${['fern', 'wood', 'wool', 'end-screen wood'].map((n, k) => n + ' ' + cell(arms[arm].map((r) => r.store[k]))).join(', ')}`);
for (const arm of ['port_cold', 'port_warm'])
  console.log(`${arm} made on the page's thread: fern ${cell(arms[arm].map((r) => r.made[0]))} wood ${cell(arms[arm].map((r) => r.made[1]))} wool ${cell(arms[arm].map((r) => r.made[2]))} end-screen wood ${cell(arms[arm].map((r) => r.made[3]))}; wasm memory ${(q(arms[arm].map((r) => r.mem), 0.5) / 1048576).toFixed(1)} MiB`);
if (arms.before.length) console.log(`before (${process.env.BEFORE}, cold, ${arms.before.length} rounds): all decoded ${cell(arms.before.map((r) => r.decoded))}; wasm memory ${(q(arms.before.map((r) => r.mem), 0.5) / 1048576).toFixed(1)} MiB`);
const ratio = (a, c) => q(arms[a].map((r) => r.decoded), 0.5) / q(arms[c].map((r) => r.decoded), 0.5);
console.log(`all decoded, port / TS: cold ${ratio('port_cold', 'ts_cold').toFixed(2)}, warm ${ratio('port_warm', 'ts_warm').toFixed(2)}`);
