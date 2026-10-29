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
// - select: a tap on a hand card, the press and the release (the selection, the pills asked of the
//   kernel, the patches; before the drag pass it was a click);
// - play: a click on the Attack pill (the move applied, the board re-read, the patches);
// - bot: the bot's step (bot_drive, then the patches), run where its timer would run it;
// - the run after play and after the bot: the frame and timer callbacks until the animations land;
// - GESTURES=1 adds, on the golden board put back (app_debug_board 0): a drag of the 6s, each
//   document move timed (the one that makes it a game action and asks the kernel, one whose answer
//   is the same, the one onto the 6d, which an attacker's card still attacks from), its drop on the table (the attack),
//   and keys (an arrow putting the cursor on, a number selecting).
import { launch, serve, FOOLISH, sleep, FORGET_GAME } from './cdp.mjs';

const RUNS = Number(process.argv[2] || 15), CPU = Number(process.argv[3] || 1);
// Cross-origin isolated, so performance.now() reads to microseconds rather than 0.1 ms.
const server = await serve(8720, FOOLISH, null, { 'cross-origin-opener-policy': 'same-origin', 'cross-origin-embedder-policy': 'require-corp' });
const b = await launch();
const med = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };
const rows = { fcp: [], ready: [], asked: [], answered: [], selScript: [], selFrame: [], playScript: [], playFrame: [], botScript: [], botFrame: [],
  playRun: [], playFrames: [], botRun: [], botFrames: [],
  dragOut: [], dragOutF: [], dragSame: [], dragSameF: [], dragOnto: [], dragOntoF: [], drop: [], dropF: [], keyArrow: [], keyArrowF: [], keyNum: [], keyNumF: [],
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
    await b.until('window.app.then((x) => x.app_game_store(1) > 0)'); // the board is drawn when the store answers
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
      // The run after an action: every frame and timer callback (C and its DOM calls) until its
      // animations have landed, summed, and the frames it took (0 for a build without animations).
      const rafNative = window.requestAnimationFrame.bind(window), timeoutNative = window.setTimeout.bind(window);
      let tracking = false, spent = 0, frames = 0;
      window.requestAnimationFrame = (cb) => rafNative((t) => { const t0 = performance.now(); cb(t); if (tracking) { spent += performance.now() - t0; frames++; } });
      window.setTimeout = (cb, ms, ...a) => timeoutNative(() => { const t0 = performance.now(); cb(...a); if (tracking) spent += performance.now() - t0; }, ms);
      const runOf = async () => { spent = 0; frames = 0; tracking = true;
        while (x.app_anim_busy && x.app_anim_busy()) await new Promise((r) => timeoutNative(r, 5));
        tracking = false; return [spent, frames]; };
      await frame();
      if (!crossOriginIsolated) throw new Error('not isolated');
      const sel = [];
      const tap = () => { const r = card().getBoundingClientRect(), p = { bubbles: true, cancelable: true, clientX: r.x + r.width / 2, clientY: r.y + r.height / 2 };
        card().dispatchEvent(new MouseEvent('mousedown', p)); document.dispatchEvent(new MouseEvent('mouseup', p)); };
      for (let k = 0; k < 9; k++) sel.push(await time(tap));   // ends selected (odd count)
      const play = await time(() => document.querySelector('[data-pill="Attack"]').click());
      const playRun = await runOf();
      const bot = await time(() => x.cd_timer(3)); // the bot's step (the test build's release path)
      const botRun = await runOf();
      let g = null;
      if (${JSON.stringify(!!process.env.GESTURES)}) {
        x.app_debug_board(0);
        while (x.app_anim_busy()) await new Promise((r) => timeoutNative(r, 5));
        const at = (sel) => { const r = document.querySelector(sel).getBoundingClientRect(); return { clientX: r.x + r.width / 2, clientY: r.y + r.height / 2 }; };
        const mouse = (target, type, p) => target.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, ...p }));
        const key = (k, code) => document.body.dispatchEvent(new KeyboardEvent('keydown', { bubbles: true, cancelable: true, key: k, keyCode: code }));
        const six = () => document.querySelector('[data-hand-plane] [data-card="0-5"]'), onto = at('[data-table-container] [data-battle-index][data-card="3-5"]');
        await frame();
        mouse(six(), 'mousedown', at('[data-hand-plane] [data-card="0-5"]'));
        const out = await time(() => mouse(document, 'mousemove', { clientX: 512, clientY: 300 }));
        const same = await time(() => mouse(document, 'mousemove', { clientX: 520, clientY: 292 }));
        const into = await time(() => mouse(document, 'mousemove', onto));
        mouse(document, 'mousemove', { clientX: 512, clientY: 300 });
        const drop = await time(() => mouse(document, 'mouseup', { clientX: 512, clientY: 300 }));
        while (x.app_anim_busy()) await new Promise((r) => timeoutNative(r, 5));
        x.app_debug_board(0);
        while (x.app_anim_busy()) await new Promise((r) => timeoutNative(r, 5));
        const arrow = await time(() => key('ArrowRight', 39));
        const num = await time(() => key('2', 50));
        g = { out, same, into, drop, arrow, num };
      }
      return { fcp: performance.getEntriesByName('first-contentful-paint')[0]?.startTime, ready: window.appReady,
        asked: x.app_game_store(0), answered: x.app_game_store(1),
        sel, play, bot, playRun, botRun, g };
    })()`);
    rows.fcp.push(r.fcp); rows.ready.push(r.ready); rows.asked.push(r.asked); rows.answered.push(r.answered);
    for (const [s, f] of r.sel) { rows.selScript.push(s); rows.selFrame.push(f); }
    rows.playScript.push(r.play[0]); rows.playFrame.push(r.play[1]);
    rows.botScript.push(r.bot[0]); rows.botFrame.push(r.bot[1]);
    rows.playRun.push(r.playRun[0]); rows.playFrames.push(r.playRun[1]); rows.botRun.push(r.botRun[0]); rows.botFrames.push(r.botRun[1]);
    if (r.g) for (const [k, row] of [['out', 'dragOut'], ['same', 'dragSame'], ['into', 'dragOnto'], ['drop', 'drop'], ['arrow', 'keyArrow'], ['num', 'keyNum']]) {
      rows[row].push(r.g[k][0]); rows[row + 'F'].push(r.g[k][1]); }
    await b.evaluate(FORGET_GAME); // the next load starts on the golden board again
  }
} finally { await b.close(); server.close(); }
const f = (v) => v.toFixed(2).padStart(7);
console.log(`cdom table (${process.env.PAGE || 'out/index.html'}), ${RUNS} fresh loads, CPU x${CPU}: median ms`);
const span = (a) => { const x = [...a].sort((p, q) => p - q); return `(${x[Math.floor(x.length / 4)].toFixed(1)}-${x[Math.floor(x.length * 3 / 4)].toFixed(1)})`; };
console.log(`first contentful paint ${f(med(rows.fcp))} ${span(rows.fcp)}   cdom.start resolved ${f(med(rows.ready))} ${span(rows.ready)}`);
console.log(`the kept game asked of the store ${f(med(rows.asked))}, answered ${f(med(rows.answered))} ${span(rows.answered)} (ms after navigation)`);
console.log(`select a card          ${f(med(rows.selScript))} script   ${f(med(rows.selFrame))} with style and layout`);
console.log(`play (Attack pill)     ${f(med(rows.playScript))} script   ${f(med(rows.playFrame))} with style and layout`);
console.log(`bot step               ${f(med(rows.botScript))} script   ${f(med(rows.botFrame))} with style and layout`);
console.log(`  play's run after it    ${f(med(rows.playRun))} script over ${med(rows.playFrames)} frames ${span(rows.playRun)}`);
console.log(`  bot's run after it     ${f(med(rows.botRun))} script over ${med(rows.botFrames)} frames ${span(rows.botRun)}`);
if (process.env.GESTURES) {
  for (const [label, row] of [['drag: the move out (a game action)', 'dragOut'], ['drag: a move, the same answer', 'dragSame'], ['drag: a move onto 6d (still Attack)', 'dragOnto'],
    ['drop on the table (attack)', 'drop'], ['key: ArrowRight (cursor on)', 'keyArrow'], ['key: 2 (select)', 'keyNum']])
    console.log(`${label.padEnd(36)} ${f(med(rows[row]))} script ${span(rows[row])}  ${f(med(rows[row + 'F']))} with style and layout`);
}
if (TEX) {
  console.log(`textures (${TEX}): url set at  fern ${f(med(rows.fernAt))}  wood ${f(med(rows.woodAt))}  wool ${f(med(rows.woolAt))}  (ms after navigation)`);
  console.log(`  made on the page's thread  fern ${f(med(rows.fernMs))}  wood ${f(med(rows.woodMs))}  wool ${f(med(rows.woolMs))}`);
  console.log(`  all three decoded at ${f(med(rows.decoded))}   wasm memory ${(med(rows.mem) / 1048576).toFixed(1)} MiB (max ${(Math.max(...rows.mem) / 1048576).toFixed(1)})`);
}
