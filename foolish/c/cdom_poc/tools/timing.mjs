// First paint and click-to-update of the cdom table in headless Chrome, medians over fresh loads.
//   node tools/timing.mjs [runs] [cpu-throttle]     (PAGE=/c/cdom_poc/out/index.html?notex&debug for another page)
// The page is the test build (?debug: the same table with app_debug_*), its bot held so the bot's
// step runs where the test says; the game each load keeps is forgotten before the next load.
// TEX=cold or TEX=warm also times the textures: cold empties the page's store before every load
// (every texture made), warm loads once first and then each load finds all three in the store.
// Per texture: when its url was set (ms after navigation), how long making it took on the page's
// thread, when all three had decoded, and the wasm memory then.
// Times are script (the handler, C, its DOM calls) and script + style + layout (forced at once).
// - first paint: the page's first-contentful-paint, and when cdom.start resolved (the wasm
//   fetched, compiled, instantiated and cd_main's first render done);
// - select: a click on a hand card (the selection, the pills asked of the kernel, the patches);
// - play: a click on the Attack pill (the move applied, the board re-read, the patches);
// - bot: the bot's step (bot_drive, then the patches), run where its timer would run it.
import { launch, serve, FOOLISH, sleep, FORGET_GAME } from './cdp.mjs';

const RUNS = Number(process.argv[2] || 15), CPU = Number(process.argv[3] || 1);
// Cross-origin isolated, so performance.now() reads to microseconds rather than 0.1 ms.
const server = await serve(8720, FOOLISH, null, { 'cross-origin-opener-policy': 'same-origin', 'cross-origin-embedder-policy': 'require-corp' });
const b = await launch();
const med = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };
const rows = { fcp: [], ready: [], selScript: [], selFrame: [], playScript: [], playFrame: [], botScript: [], botFrame: [],
  fernAt: [], woodAt: [], woolAt: [], fernMs: [], woodMs: [], woolMs: [], decoded: [], mem: [] };
const TEX = process.env.TEX, ORIGIN = 'http://127.0.0.1:8720';
const texDone = `window.app.then((x) => x.app_tex_ms(x.app_tex_count()) === x.app_tex_count())`;
try {
  await b.send('Network.enable');
  await b.send('Network.setCacheDisabled', { cacheDisabled: true });
  if (CPU > 1) await b.send('Emulation.setCPUThrottlingRate', { rate: CPU });
  if (TEX === 'warm') { // one load to fill the store
    await b.goto(ORIGIN + (process.env.PAGE || '/c/cdom_poc/out/index.html'));
    await b.until(texDone, 120000); await sleep(1000);
  }
  for (let i = 0; i < RUNS; i++) {
    if (TEX === 'cold') await b.send('Storage.clearDataForOrigin', { origin: ORIGIN, storageTypes: 'indexeddb' });
    // BLANK=1: each load starts from a blank page, in a fresh renderer, with nothing left running.
    // Needed to time a page that makes textures: the previous load's slices otherwise hold the
    // thread into the next load (measured: +18 ms at x4). Slower absolute numbers (a cold process).
    if (process.env.BLANK) { await b.goto('about:blank'); await sleep(300); }
    await b.goto('http://127.0.0.1:8720' + (process.env.PAGE || '/c/cdom_poc/out/index.html?debug'));
    await b.until('window.appReady');
    await b.evaluate('window.app.then((x) => x.app_debug_hold(1))'); // the bot's step is the test's to run
    if (await b.evaluate('window.app.then((x) => x.app_game_store(2))')) throw new Error('a kept game was adopted: not the golden board');
    if (TEX) {
      await b.until(texDone, 120000);
      const t = await b.evaluate(`window.app.then(async (x) => {
        const u = ['--tex-fern', '--tex-wood', '--tex-wool', '--tex-wood-end'].map((v) => document.getElementById('root').style.getPropertyValue(v).slice(4, -1));
        await Promise.all(u.map((s) => { const i = new Image(); i.src = s; return i.decode(); }));
        if (![0, 1, 2, 3].every((k) => x.app_tex_stored(k) === (${JSON.stringify(TEX)} === 'warm' ? 1 : 0))) throw new Error('not a ${TEX} load');
        return { at: [0, 1, 2].map((k) => x.app_tex_at(k)), ms: [0, 1, 2].map((k) => x.app_tex_ms(k)), decoded: performance.now(), mem: x.memory.buffer.byteLength }; })`);
      rows.fernAt.push(t.at[0]); rows.woodAt.push(t.at[1]); rows.woolAt.push(t.at[2]);
      rows.fernMs.push(t.ms[0]); rows.woodMs.push(t.ms[1]); rows.woolMs.push(t.ms[2]); rows.decoded.push(t.decoded); rows.mem.push(t.mem);
    }
    await sleep(200);
    const r = await b.evaluate(`(async () => {
      const x = await window.app;
      const frame = () => new Promise((r) => requestAnimationFrame(() => setTimeout(r, 0)));
      // script: the click handler, C and its DOM calls; layout: then style and layout forced at once.
      const time = async (f) => { await frame(); const t0 = performance.now(); f(); const t1 = performance.now();
        document.body.getBoundingClientRect(); document.querySelector('[data-hand-plane]').offsetHeight;
        return [t1 - t0, performance.now() - t0]; };
      const card = () => document.querySelector('[data-hand-plane] [data-card="0-5"]');
      await frame();
      if (!crossOriginIsolated) throw new Error('not isolated');
      const sel = [];
      for (let k = 0; k < 9; k++) sel.push(await time(() => card().click()));   // ends selected (odd count)
      const play = await time(() => document.querySelector('[data-pill="Attack"]').click());
      const bot = await time(() => x.cd_timer(3)); // the bot's step (the test build's release path)
      return { fcp: performance.getEntriesByName('first-contentful-paint')[0]?.startTime, ready: window.appReady,
        sel, play, bot };
    })()`);
    rows.fcp.push(r.fcp); rows.ready.push(r.ready);
    for (const [s, f] of r.sel) { rows.selScript.push(s); rows.selFrame.push(f); }
    rows.playScript.push(r.play[0]); rows.playFrame.push(r.play[1]);
    rows.botScript.push(r.bot[0]); rows.botFrame.push(r.bot[1]);
    await b.evaluate(FORGET_GAME); // the next load starts on the golden board again
  }
} finally { await b.close(); server.close(); }
const f = (v) => v.toFixed(2).padStart(7);
console.log(`cdom table (${process.env.PAGE || 'out/index.html'}), ${RUNS} fresh loads, CPU x${CPU}: median ms`);
console.log(`first contentful paint ${f(med(rows.fcp))}   cdom.start resolved ${f(med(rows.ready))}`);
console.log(`select a card          ${f(med(rows.selScript))} script   ${f(med(rows.selFrame))} with style and layout`);
console.log(`play (Attack pill)     ${f(med(rows.playScript))} script   ${f(med(rows.playFrame))} with style and layout`);
console.log(`bot step               ${f(med(rows.botScript))} script   ${f(med(rows.botFrame))} with style and layout`);
if (TEX) {
  console.log(`textures (${TEX}): url set at  fern ${f(med(rows.fernAt))}  wood ${f(med(rows.woodAt))}  wool ${f(med(rows.woolAt))}  (ms after navigation)`);
  console.log(`  made on the page's thread  fern ${f(med(rows.fernMs))}  wood ${f(med(rows.woodMs))}  wool ${f(med(rows.woolMs))}`);
  console.log(`  all three decoded at ${f(med(rows.decoded))}   wasm memory ${(med(rows.mem) / 1048576).toFixed(1)} MiB (max ${(Math.max(...rows.mem) / 1048576).toFixed(1)})`);
}
