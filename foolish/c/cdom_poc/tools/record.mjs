// record.mjs - a whole game recorded as a player sees it, from the first paint through the end
// screen and Play again, for the owner to watch (README, Done).
//
//   node tools/record.mjs                 1024x768, mouse and keys: the moves rotate click, drag,
//                                         number and letter keys, the arrow cursor
//   W=390 H=844 TOUCH=1 node tools/record.mjs   a phone's window with touch emulated, every move
//                                         a touch tap or a touch drag
//   MOVES=n stops after n moves of mine (a short recording); the game otherwise runs to its end.
//
// The page is the one a player opens: out/index.html, the release build, textures on and both
// stores emptied first (a cold load: the textures are made on this load), the bot at the kernel's
// own pace, the flights at 1x. Nothing is driven from inside the page: every move is real CDP input.
// The recording is the compositor's own frames (Page.startScreencast, a frame only when the page
// changes, each with its timestamp), written to out/run/<name>/ as they come; kept in shots/run/:
//   <name>.mp4         every frame, each shown for as long as it was on screen (ffmpeg, 60 fps)
// The screencast hands over the next frame only once the last is acked: each is acked the moment it
// arrives and written to disk after, off the hot path, and is a JPEG (Chrome encodes a 1024x768 PNG
// in tens of ms, which held the old recording to about 14 frames a second: the film stuttered where
// the page did not). FILM_CHECK=1 counts, over every flight, the frames the film holds against the
// frames the page drew (an rAF log), and fails under 90 per cent.
//   <name>_moves.txt   every move: its time in the film, by what hand, what it was
//   <name>_sheet.png   one frame a second, its time on it
// and in out/run/<name>_50ms/ (not committed: about 2 MB a frame), one frame every 50 ms while
// anything moves (a flight, a coin, a slide), named by its time in the film.
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { launch, serve, FOOLISH, POC, sleep, FORGET_GAME } from './cdp.mjs';
import { key } from './input.mjs';

const W = Number(process.env.W || 1024), H = Number(process.env.H || 768);
const TOUCH = process.env.TOUCH === '1';
const MAX_MOVES = Number(process.env.MOVES || 400);
const name = `run_${W}x${H}${TOUCH ? '_touch' : ''}${process.env.MOVES ? `_${MAX_MOVES}moves` : ''}`;
const URL_TABLE = `http://127.0.0.1:8720/c/cdom_poc/${process.env.PAGE || 'out'}/index.html`; // PAGE=out_<commit>: an earlier build (retake.sh)
const raw = path.join(POC, 'out/run', name);
const keep = path.join(POC, 'shots/run');
fs.rmSync(raw, { recursive: true, force: true });
fs.mkdirSync(raw, { recursive: true });
fs.mkdirSync(keep, { recursive: true });

const server = await serve(8720, FOOLISH);
const b = await launch({ width: W, height: H });
if (TOUCH) {
  await b.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
  await b.send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: true });
}

// ---- the recorder: every frame to disk as it comes, with its compositor time ----------------
const frames = [];
let t0 = null;
const writes = [];
const off = b.listen('Page.screencastFrame', (f) => {
  b.send('Page.screencastFrameAck', { sessionId: f.sessionId }).catch(() => {});
  const ts = f.metadata.timestamp * 1000;
  if (t0 === null) { t0 = ts; firstWall = Date.now(); }
  const file = path.join(raw, `${String(frames.length).padStart(5, '0')}.jpg`);
  writes.push(fs.promises.writeFile(file, Buffer.from(f.data, 'base64')));
  frames.push({ t: ts - t0, file, ts });
});
// The page's own frames and its flights (FILM_CHECK): every rAF time, every flight's transition start
// and end, installed before any script of the page.
await b.send('Page.addScriptToEvaluateOnNewDocument', { source: `(() => { const fp = window.__fp = { frames: [], runs: [], ends: [] };
  const loop = (t) => { fp.frames.push(performance.timeOrigin + t); requestAnimationFrame(loop); }; requestAnimationFrame(loop);
  const isFlight = (e) => { const p = e.target.parentElement; return !!p && p.style.zIndex === '10000' && p.style.position === 'fixed'; };
  addEventListener('transitionrun', (e) => { if (e.propertyName === 'top' && isFlight(e)) fp.runs.push(performance.timeOrigin + performance.now()); }, true);
  const end = (e) => { if (e.propertyName === 'top' && isFlight(e)) fp.ends.push(performance.timeOrigin + performance.now()); };
  addEventListener('transitionend', end, true); addEventListener('transitioncancel', end, true); })();` });
const moves = [];
// A move's time in the film: the wall clock since the first frame came (the compositor's clock is
// the frames' own; the two run together within a frame).
let firstWall = null;
const now = () => (firstWall === null ? 0 : Date.now() - firstWall);
const log = (hand, what) => { moves.push({ t: Math.round(now()), hand, what }); console.log(`${(now() / 1000).toFixed(2).padStart(7)} s  ${hand.padEnd(7)} ${what}`); };

// ---- the page, read as a player would read it -----------------------------------------------
const all = (sel, expr) => b.evaluate(`[...document.querySelectorAll(${JSON.stringify(sel)})].map((e) => ${expr})`);
const pills = () => all('[data-pill]', 'e.dataset.pill');
const turn = () => b.evaluate(`document.querySelector('[data-game-container]')?.dataset.turn`);
const runs = () => b.evaluate('window.app.then((x) => x.app_anim_runs())');
const centre = (sel) => b.evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null;
  const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
const restingHand = () => b.until(`[...document.querySelectorAll('[data-hand-plane] [data-card]')].every((e) => { const c = getComputedStyle(e);
  return ['left', 'bottom', 'width', 'height'].every((p) => Math.abs(parseFloat(c[p]) - parseFloat(e.style[p])) < 0.05); })`, 3000);
const point = (type, p) => TOUCH
  ? b.send('Input.dispatchTouchEvent', { type: { press: 'touchStart', move: 'touchMove', release: 'touchEnd' }[type], touchPoints: type === 'release' ? [] : [{ x: p.x, y: p.y, id: 1 }] })
  : b.send('Input.dispatchMouseEvent', { type: { press: 'mousePressed', move: 'mouseMoved', release: 'mouseReleased' }[type], x: p.x, y: p.y, button: 'left', buttons: type === 'release' ? 0 : 1, clickCount: 1 });
// A tap or a click: pressed and released where the element rests, a human's 80 ms apart.
const tap = async (sel) => {
  await restingHand();
  const at = await centre(sel);
  await point('press', at); await sleep(80); await point('release', at);
  await sleep(150);
};
// A drag as a hand makes it: moves 16 ms apart, 12 steps to each target, a pause over it.
const glide = async (from, to) => {
  for (let k = 1; k <= 12; k++) { await point('move', { x: from.x + (to.x - from.x) * k / 12, y: from.y + (to.y - from.y) * k / 12 }); await sleep(16); }
};
const dragPlay = async (card, targets) => {
  await restingHand();
  const from = await centre(`[data-hand-plane] [data-card="${card}"]`);
  await point('press', from);
  let at = from;
  for (const to of targets) {
    await glide(at, to); at = to;
    await sleep(250);
    const verb = await b.evaluate(`document.querySelector('.drag-shadow__indicator')?.textContent || ''`);
    if (verb) { await point('release', to); return verb.trim(); }
  }
  await glide(at, { x: from.x, y: from.y - 200 });
  await glide({ x: from.x, y: from.y - 200 }, from);
  await point('release', from);
  return '';
};
const DIGITS = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '='];

// One move of mine by `hand`; returns what it was, or '' when this hand found none.
async function move(hand) {
  const cards = await all('[data-hand-plane] [data-card]', 'e.dataset.card');
  const r0 = await runs();
  if (hand === 'click' || hand === 'tap') {
    for (const c of cards) {
      await tap(`[data-hand-plane] [data-card="${c}"]`);
      const p = (await pills()).filter((x) => ['Attack', 'Cover', 'Pass'].includes(x));
      if (p.length) { await sleep(250); await tap(`[data-pill="${p[0]}"]`); return `${c} selected, ${p[0]} pressed`; }
      await tap(`[data-hand-plane] [data-card="${c}"]`);
    }
    return '';
  }
  if (hand === 'drag' || hand === 'touch-drag') {
    const open = await all('[data-table-container] > [data-location="table"]', 'e.querySelectorAll("[data-card]").length === 1 ? e.querySelector("[data-card]").dataset.card : ""');
    const targets = [];
    for (const c of open.filter(Boolean)) targets.push(await centre(`[data-table-container] [data-battle-index][data-card="${c}"]`));
    targets.push({ x: W / 2, y: H * 0.3 });
    for (const c of cards) {
      const verb = await dragPlay(c, targets);
      if (verb) {
        await b.until(`window.app.then((x) => x.app_anim_runs() > ${r0} || x.app_last_reject())`, 3000);
        return `${c} dragged: ${verb}`;
      }
    }
    return '';
  }
  if (hand === 'letters') {
    for (let i = 0; i < Math.min(cards.length, 12); i++) {
      await key(b, DIGITS[i]); await sleep(200);
      const p = await pills();
      const verb = p.includes('Attack') ? 'a' : p.includes('Cover') ? 'c' : p.includes('Pass') ? 'p' : '';
      if (verb) { await key(b, verb); await sleep(100); return `key ${DIGITS[i]} (${cards[i]}), then ${verb.toUpperCase()}`; }
      await key(b, DIGITS[i]); await sleep(100);
    }
    return '';
  }
  // arrows: the cursor on each card in turn, Up (Up again when it aims)
  const state = () => b.evaluate(`(() => { const e = document.querySelector('[data-testid="kbd-state"]'); return [e.dataset.mode, e.dataset.sel]; })()`);
  for (let i = 0; i < cards.length; i++) {
    for (let n = 0; n < cards.length + 1 && (await state())[1] !== cards[i]; n++) { await key(b, 'ArrowRight'); await sleep(120); }
    await key(b, 'ArrowUp'); await sleep(200);
    let aimed = false;
    if ((await state())[0] === 'target') { aimed = true; await sleep(300); await key(b, 'ArrowUp'); }
    await sleep(50);
    if (await runs() > r0) return `arrows to ${cards[i]}, Up${aimed ? ' (aimed), Up' : ''}`;
  }
  return '';
}

try {
  // Both stores emptied (the kept game and the textures): a cold load, a fresh game.
  await b.goto('http://127.0.0.1:8720/src/styles/index.css');
  await b.evaluate(FORGET_GAME);
  await b.evaluate(`new Promise((r) => { const q = indexedDB.deleteDatabase('cdom'); q.onsuccess = q.onerror = q.onblocked = () => r(1); })`);
  await b.goto('about:blank');
  await b.send('Page.startScreencast', { format: 'jpeg', quality: 92, everyNthFrame: 1 });
  await b.send('Page.navigate', { url: URL_TABLE });
  log('-', 'navigated: a cold load of out/index.html');
  await b.until('window.appReady', 10000);
  await b.until('window.app.then((x) => x.app_game_store(1) > 0)', 5000);
  await b.until(`window.app.then((x) => x.app_tex_ms(x.app_tex_count()) === x.app_tex_count())`, 60000);
  log('-', 'the textures have landed');
  const hands = TOUCH ? ['tap', 'touch-drag'] : ['click', 'drag', 'letters', 'arrows'];
  let mine = 0, k = 0;
  for (; mine < MAX_MOVES;) {
    await b.until(`document.querySelector('[data-game-container]').dataset.turn !== 'bot'`, 30000);
    await b.until('window.app.then((x) => !x.app_anim_busy())', 30000);
    if ((await turn()) === 'over') break;
    await sleep(400); // a player looks before moving
    let did = '', by = '';
    for (let tries = 0; tries < hands.length && !did; tries++) { by = hands[(k + tries) % hands.length]; did = await move(by); }
    k++;
    if (!did) {
      const p = await pills();
      const verb = p.includes('Good') ? 'Good' : p.includes('Pickup') ? 'Pickup' : '';
      if (!verb) throw new Error(`no move offered: pills ${p.join(' ')}`);
      by = hands[k % hands.length];
      if (by === 'arrows') { await key(b, 'ArrowDown'); did = `Down (${verb})`; }
      else if (by === 'letters') { await key(b, verb === 'Good' ? 'g' : 'u'); did = `key ${verb === 'Good' ? 'G' : 'U'} (${verb})`; }
      else { await tap(`[data-pill="${verb}"]`); did = `${verb} pressed`; }
    }
    log(by, did);
    mine++;
    await sleep(100);
  }
  if ((await turn()) === 'over') {
    await sleep(1500);
    log('-', `the end screen: ${await b.evaluate(`document.querySelector('[data-end] h1').textContent`)}`);
    await sleep(1500);
    await tap('[data-end] button');
    log(TOUCH ? 'tap' : 'click', 'Play again');
    await b.until('window.app.then((x) => !x.app_anim_busy())', 30000);
    await sleep(800);
    log('-', 'the new deal has landed');
  }
  await sleep(500);
} catch (e) {
  await b.send('Page.stopScreencast').catch(() => {});
  await b.close();
  server.close();
  throw e;
}
const pageLog = await b.evaluate('({ frames: window.__fp.frames, runs: window.__fp.runs, ends: window.__fp.ends })');
await b.send('Page.stopScreencast').catch(() => {});
off();
await Promise.all(writes);

// ---- what is kept ---------------------------------------------------------------------------
try {
const end = frames.at(-1).t + 100;
fs.writeFileSync(path.join(keep, `${name}_moves.txt`), moves.map((m) => `${(m.t / 1000).toFixed(2).padStart(7)} s  ${m.hand.padEnd(10)} ${m.what}`).join('\n') + '\n');
// The MP4: each frame for as long as it was on screen.
const list = path.join(raw, 'list.txt');
fs.writeFileSync(list, frames.map((f, i) => `file '${f.file}'\nduration ${(((frames[i + 1]?.t ?? end) - f.t) / 1000).toFixed(4)}`).join('\n') + `\nfile '${frames.at(-1).file}'\n`);
execFileSync('ffmpeg', ['-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', list,
  '-vf', 'fps=60,scale=trunc(iw/2)*2:trunc(ih/2)*2', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '20', path.join(keep, `${name}.mp4`)]);
// The film against the page: over every flight (a transition's start to its end), the frames the
// screencast delivered against the frames the page drew. The screencast's timestamps are wall-clock
// seconds, the page's performance.timeOrigin-based ms: the same clock.
const flightsIv = pageLog.runs.map((s) => [s, pageLog.ends.find((e) => e >= s) ?? s + 600]);
let pageN = 0, filmN = 0;
for (const [s, e] of flightsIv) {
  pageN += pageLog.frames.filter((t) => t >= s && t <= e).length;
  filmN += frames.filter((f) => f.ts >= s && f.ts <= e).length;
}
const filmPct = pageN ? (100 * filmN / pageN) : NaN;
console.log(`film check: over ${flightsIv.length} flights the page drew ${pageN} frames and the film holds ${filmN} (${filmPct.toFixed(1)}%)`);
if (process.env.FILM_CHECK && !(filmPct >= 90)) { console.log('FILM_CHECK: the film drops frames the page drew'); process.exitCode = 1; }
// One frame every 50 ms while the page changes (a frame came within the last 50 ms), as PNGs.
const dir = path.join(POC, 'out/run', `${name}_50ms`);
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
let kept = 0;
for (let at = 0, i = 0; at <= end; at += 50) {
  const before = i;
  while (i + 1 < frames.length && frames[i + 1].t <= at) i++;
  if (i === before && at > 0) continue; // nothing new was painted in this 50 ms
  fs.copyFileSync(frames[i].file, path.join(dir, `${String(Math.round(at)).padStart(7, '0')}ms.jpg`));
  kept++;
}
// The contact sheet, one frame a second with its time on it, drawn by Chrome in a canvas.
const picked = [];
for (let at = 0, i = 0; at <= end; at += 1000) {
  while (i + 1 < frames.length && frames[i + 1].t <= at) i++;
  picked.push({ at, file: frames[i].file });
}
await b.goto('http://127.0.0.1:8720/src/styles/index.css'); // the frames' origin: the canvas stays readable
const cell = W >= 1000 ? 256 : 130;
const sheet = await b.evaluate(`(async () => {
  const urls = ${JSON.stringify(picked.map((p) => 'http://127.0.0.1:8720/' + path.relative(FOOLISH, p.file)))};
  const ats = ${JSON.stringify(picked.map((p) => p.at))};
  const imgs = await Promise.all(urls.map((u) => new Promise((r) => { const i = new Image(); i.onload = () => r(i); i.src = u; })));
  const w = ${cell}, h = Math.round(w * imgs[0].height / imgs[0].width), cols = 10, rows = Math.ceil(imgs.length / cols);
  const c = document.createElement('canvas'); c.width = cols * w; c.height = rows * h;
  const g = c.getContext('2d');
  g.fillStyle = '#000'; g.fillRect(0, 0, c.width, c.height);
  imgs.forEach((im, k) => {
    const x = (k % cols) * w, y = Math.floor(k / cols) * h;
    g.drawImage(im, x, y, w - 2, h - 2);
    g.fillStyle = 'rgba(0,0,0,0.7)'; g.fillRect(x, y, 44, 16);
    g.fillStyle = '#fff'; g.font = '12px monospace'; g.fillText((ats[k] / 1000) + ' s', x + 3, y + 12);
  });
  return c.toDataURL('image/png').slice(22);
})()`);
fs.writeFileSync(path.join(keep, `${name}_sheet.png`), Buffer.from(sheet, 'base64'));
console.log(`${name}: ${frames.length} frames over ${(end / 1000).toFixed(1)} s, ${moves.length} entries; shots/run/${name}.mp4, ${name}_moves.txt, ${name}_sheet.png, out/run/${name}_50ms/ (${kept} frames, 50 ms apart while the page changes)`);
} finally {
  await b.close();
  server.close();
}
