// First paint and click-to-update of the cdom table in headless Chrome, medians over fresh loads.
//   node tools/timing.mjs [runs] [cpu-throttle]
// Times are script (the handler, C, its DOM calls) and script + style + layout (forced at once).
// - first paint: the page's first-contentful-paint, and when cdom.start resolved (the wasm
//   fetched, compiled, instantiated and cd_main's first render done);
// - select: a click on a hand card (the selection, the pills asked of the kernel, the patches);
// - play: a click on the Attack pill (the move applied, the board re-read, the patches);
// - bot: the bot's step (bot_drive, then the patches), run where its timer would run it.
import { launch, serve, FOOLISH, sleep } from './cdp.mjs';

const RUNS = Number(process.argv[2] || 15), CPU = Number(process.argv[3] || 1);
// Cross-origin isolated, so performance.now() reads to microseconds rather than 0.1 ms.
const server = await serve(8720, FOOLISH, null, { 'cross-origin-opener-policy': 'same-origin', 'cross-origin-embedder-policy': 'require-corp' });
const b = await launch();
const med = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };
const rows = { fcp: [], ready: [], selScript: [], selFrame: [], playScript: [], playFrame: [], botScript: [], botFrame: [] };
try {
  await b.send('Network.enable');
  await b.send('Network.setCacheDisabled', { cacheDisabled: true });
  if (CPU > 1) await b.send('Emulation.setCPUThrottlingRate', { rate: CPU });
  for (let i = 0; i < RUNS; i++) {
    await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html');
    await b.until(`document.querySelector('[data-game-container]')?.dataset.turn === 'human'`);
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
      const bot = await time(() => x.cd_timer(1));
      return { fcp: performance.getEntriesByName('first-contentful-paint')[0]?.startTime, ready: window.appReady,
        sel, play, bot };
    })()`);
    rows.fcp.push(r.fcp); rows.ready.push(r.ready);
    for (const [s, f] of r.sel) { rows.selScript.push(s); rows.selFrame.push(f); }
    rows.playScript.push(r.play[0]); rows.playFrame.push(r.play[1]);
    rows.botScript.push(r.bot[0]); rows.botFrame.push(r.bot[1]);
  }
} finally { await b.close(); server.close(); }
const f = (v) => v.toFixed(2).padStart(7);
console.log(`cdom table, ${RUNS} fresh loads, CPU x${CPU}: median ms`);
console.log(`first contentful paint ${f(med(rows.fcp))}   cdom.start resolved ${f(med(rows.ready))}`);
console.log(`select a card          ${f(med(rows.selScript))} script   ${f(med(rows.selFrame))} with style and layout`);
console.log(`play (Attack pill)     ${f(med(rows.playScript))} script   ${f(med(rows.playFrame))} with style and layout`);
console.log(`bot step               ${f(med(rows.botScript))} script   ${f(med(rows.botFrame))} with style and layout`);
