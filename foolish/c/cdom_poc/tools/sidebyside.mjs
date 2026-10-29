// sidebyside.mjs - the port beside the REAL React app (its build on a local c_base, react_rig.mjs),
// moment by moment, at one window size, in shots/side_by_side/<moment>_<W>x<H>.png (React left,
// the port right, each labelled):
//   midgame   the table at rest on my turn, nothing selected
//   selected  a card with a move selected: its red border, its pill
//   drag      that card held over the open table: the shadow, its badge, the drop cell
//   cursor    the arrows' cursor under a card
//   target    a card aimed by the arrows (the red arrow); the port on its board 1 (?board=1, the
//             trump that covers either battle), React when its game deals a card that aims
//   flight    200 ms after the pill: the card in the air at scale, the hand laid out without it
//   end       the end screen: React's golden win_screen.html under the app's CSS (no live game is
//             played to its end here), the port's after a game played out
// Both tables are what a player opens, textures on and warm. The boards differ (React's is a live
// server game, the port's the golden's): what is compared is the look of each moment.
//   node tools/sidebyside.mjs            (W=390 H=844 for a phone's window)
import fs from 'node:fs';
import path from 'node:path';
import { launch, sleep, FORGET_GAME, POC, FOOLISH, openTable } from './cdp.mjs';
import { startRig, reactTable, PORT_ORIGIN, REACT } from './react_rig.mjs';
import { key } from './input.mjs';
import { playToEnd } from './play.mjs';

const W = Number(process.env.W || 1024), H = Number(process.env.H || 768), SIZE = `${W}x${H}`;
const OUT = path.join(POC, 'shots/side_by_side');
const RAW = path.join(POC, 'out/side_by_side');
fs.mkdirSync(OUT, { recursive: true });
fs.mkdirSync(RAW, { recursive: true });
const rig = await startRig();
const b = await launch({ width: W, height: H });
const handSel = '[data-hand-plane] [data-card]';
const PILLS = `[...document.querySelectorAll('[data-touch-interactive] > [style*="z-index: 999"] button')]`;
const MOVES = ['Attack', 'Cover', 'Pass'];
const mouse = (type, p) => b.send('Input.dispatchMouseEvent', { type, x: p.x, y: p.y, button: 'left', buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1 });
const centre = (sel) => b.evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
const tap = async (sel) => { const at = await centre(sel); await mouse('mousePressed', at); await sleep(60); await mouse('mouseReleased', at); await sleep(300); };
const pills = () => b.evaluate(`${PILLS}.map((e) => e.textContent.trim())`);
const mode = () => b.evaluate(`document.querySelector('[data-testid="kbd-state"]')?.dataset.mode`);
const shot = (arm, moment) => b.shot(path.join(RAW, `${moment}_${SIZE}_${arm}.png`));
// The page's quiet: no DOM change for ms (installed before any script of the page).
await b.send('Page.addScriptToEvaluateOnNewDocument', { source: `window.__mut = 0; new MutationObserver(() => { window.__mut = performance.now(); })
  .observe(document, { subtree: true, childList: true, attributes: true, characterData: true });` });
const quiet = (ms = 800) => b.until(`performance.now() - window.__mut > ${ms}`, 30000);

// A card of mine with a move, and its first move pill; untimed taps find it, the card left unselected.
async function moveCard() {
  for (let tries = 0; tries < 15; tries++) {
    await quiet();
    for (const card of await b.evaluate(`[...document.querySelectorAll('${handSel}')].map((e) => e.dataset.card)`)) {
      await tap(`${handSel}[data-card="${card}"]`);
      const p = await pills();
      await tap(`${handSel}[data-card="${card}"]`);
      const move = p.find((x) => MOVES.includes(x));
      if (move) return { card, pill: move, aims: p.includes('Cover') && p.includes('Pass') };
    }
    const other = (await pills()).find((x) => x === 'Good' || x === 'Pickup');
    if (other) await tap(`[data-touch-interactive] > [style*="z-index: 999"] button`);
    await sleep(3500);
  }
  throw new Error('no move of mine came');
}

async function moments(arm, aimBoard) {
  const m = await moveCard();
  await quiet();
  await shot(arm, 'midgame');
  await tap(`${handSel}[data-card="${m.card}"]`);
  await shot(arm, 'selected');
  await tap(`${handSel}[data-card="${m.card}"]`);
  const from = await centre(`${handSel}[data-card="${m.card}"]`);
  await mouse('mousePressed', from);
  for (let k = 1; k <= 10; k++) await mouse('mouseMoved', { x: from.x + (W / 2 - from.x) * k / 10, y: from.y + (H * 0.39 - from.y) * k / 10 });
  await sleep(400);
  await shot(arm, 'drag');
  await mouse('mouseMoved', { x: from.x, y: from.y - 200 }); await mouse('mouseMoved', from); await mouse('mouseReleased', from);
  await quiet();
  await key(b, 'ArrowRight'); await sleep(400);
  await shot(arm, 'cursor');
  await key(b, 'ArrowLeft'); await sleep(200);
  if (m.aims) { // React's own aim, when its board deals one: the cursor onto the card, Up aims
    for (let n = 0; n < 12 && (await b.evaluate(`document.querySelector('[data-testid="kbd-state"]').dataset.sel`)) !== m.card; n++) await key(b, 'ArrowRight');
    await key(b, 'ArrowUp'); await sleep(400);
    if (await mode() === 'target') { await shot(arm, 'target'); await key(b, 'ArrowDown'); await sleep(200); }
  }
  await quiet();
  await tap(`${handSel}[data-card="${m.card}"]`);
  const at = await b.evaluate(`(() => { const e = ${PILLS}.find((e) => e.textContent.trim() === ${JSON.stringify(m.pill)}); const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
  await mouse('mousePressed', at); await mouse('mouseReleased', at);
  await sleep(200);
  await shot(arm, 'flight');
  if (aimBoard) await aimBoard();
  return m;
}

// The two shots of a moment side by side, labelled, drawn by Chrome in a canvas.
async function compose(moment, note = {}) {
  const files = ['react', 'port'].map((a) => path.join(RAW, `${moment}_${SIZE}_${a}.png`));
  if (!fs.existsSync(files[1])) return null;
  await b.goto(PORT_ORIGIN + '/src/styles/index.css');
  const png = await b.evaluate(`(async () => {
    const urls = ${JSON.stringify(files.map((f) => fs.existsSync(f) ? 'data:image/png;base64,' + fs.readFileSync(f).toString('base64') : null))};
    const labels = ${JSON.stringify([['React (its build, live on c_base)', note.react || ''], ['port', note.port || '']])};
    const imgs = await Promise.all(urls.map((u) => u && new Promise((r) => { const i = new Image(); i.onload = () => r(i); i.src = u; })));
    const gap = 12, bar = 38, w = imgs[1].width, h = imgs[1].height;
    const c = new OffscreenCanvas(w * 2 + gap, h + bar), g = c.getContext('2d');
    g.fillStyle = '#111'; g.fillRect(0, 0, c.width, c.height);
    imgs.forEach((im, k) => { if (im) g.drawImage(im, k * (w + gap), bar); g.fillStyle = '#fff'; g.font = '13px sans-serif';
      g.fillText(labels[k][0], k * (w + gap) + 6, 15); g.fillText(labels[k][1], k * (w + gap) + 6, 31, w - 12); });
    const blob = await c.convertToBlob({ type: 'image/png' });
    const bytes = new Uint8Array(await blob.arrayBuffer()); let s = ''; for (const v of bytes) s += String.fromCharCode(v);
    return btoa(s); })()`);
  const out = path.join(OUT, `${moment}_${SIZE}.png`);
  fs.writeFileSync(out, Buffer.from(png, 'base64'));
  return out;
}

try {
  // React: a live table, warm (one load first)
  const { url } = await reactTable(b);
  await b.goto(url); await sleep(4000);
  await b.goto('about:blank'); await b.goto(url); await sleep(4000);
  const rm = await moments('react');
  // the port: warm, the golden board, the bot's first answer awaited by moveCard
  await b.goto(PORT_ORIGIN + '/src/styles/index.css'); await b.evaluate(FORGET_GAME);
  await b.goto(PORT_ORIGIN + '/c/cdom_poc/out/index.html');
  await b.until(`window.app.then((x) => x.app_tex_ms(x.app_tex_count()) === x.app_tex_count())`, 60000); await sleep(1000);
  await b.evaluate(FORGET_GAME);
  await b.goto(PORT_ORIGIN + '/c/cdom_poc/out/index.html'); await sleep(500);
  await moments('port', async () => { // the port's aim, on its board 1
    await openTable(b, PORT_ORIGIN + '/c/cdom_poc/out/index.html?debug&board=1', { hold: true });
    await sleep(800);
    for (let i = 0; i < 3; i++) await key(b, 'ArrowRight');   // 9h, Kd, 6c
    await key(b, 'ArrowUp'); await sleep(400);
    await shot('port', 'target');
  });
  // the end: React's golden WinScreen under the app's CSS; the port's after a game played out (the
  // test build, its bot fast)
  const golden = fs.readFileSync(path.join(FOOLISH, 'e2e/fixtures/ui_dom/win_screen.html'), 'utf8');
  await b.goto(PORT_ORIGIN + '/src/styles/index.css');
  await b.evaluate(`(() => { document.documentElement.setAttribute('data-theme', 'default'); document.head.innerHTML = '<link href="/src/styles/index.css" rel="stylesheet"/>';
    document.body.innerHTML = '<div id="root">' + ${JSON.stringify(golden)} + '</div>'; })()`);
  await b.evaluate(`new Promise((r) => { const l = document.querySelector('link'); if (l.sheet) r(1); else l.onload = () => r(1); }).then(() => document.fonts.ready).then(() => 1)`);
  await sleep(300);
  await shot('react', 'end');
  await openTable(b, PORT_ORIGIN + '/c/cdom_poc/out/index.html?debug&notex', { fast: true });
  await b.evaluate('window.app.then((x) => x.app_debug_anim_speed(8))');
  await playToEnd(b);
  await sleep(500);
  await shot('port', 'end');
  const notes = { end: { react: 'golden win_screen.html (3 seats, rated), no textures', port: 'a game played out, no textures' },
    target: { react: rm.aims ? 'its own board' : 'no card that aims came up', port: 'board 1' } };
  for (const moment of ['midgame', 'selected', 'drag', 'cursor', 'target', 'flight', 'end']) {
    const out = await compose(moment, notes[moment]);
    console.log(out ? path.relative(POC, out) : `${moment}: no shot`);
  }
} finally { await b.close(); await rig.stop(); }
