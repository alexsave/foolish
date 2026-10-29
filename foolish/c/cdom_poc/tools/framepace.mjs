// framepace.mjs - are the flights smooth? Frame delivery during every flight, measured in the page and
// in Chrome's own trace, the port against the REAL React app (tools/react_rig.mjs) on the same kinds
// of move, in headless Chrome (--headless=new: the real compositor, its own 60 Hz begin-frame
// source; no window is ever shown on this shared Mac).
//   node tools/framepace.mjs [moves] [cpu]        both arms, `moves` moves of mine each (default 10)
//   ARMS=port node tools/framepace.mjs 10 1        the port alone (no React build or c_base needed)
//   FILM=1 node tools/framepace.mjs 8 1            and each session filmed (shots/run/flights_*.mp4),
//                                                  both side by side (shots/run/side_by_side_react_left_port_right_<W>x<H>.mp4)
// What is measured, over every flight window (from a flight's first transition to its last end):
//   - the page's frames: an rAF logger installed before any script, every frame's time; a gap is a
//     delta over 1.5 vsyncs, the frames it lost round(delta / vsync) - 1;
//   - the trace (devtools.timeline): every main-thread task over 16 ms in the window and what it
//     spent (script, style, layout, paint, prepaint/layerize, decode), and forced layouts: a Layout
//     or UpdateLayoutTree inside a FireAnimationFrame (a rect read after a write in the frame loop).
// A flight window is named by what started it: my move's pill (attack, cover, pass, take, sweep),
// the deal (DEAL=1 PAGE_QUERY=?debug, the port: a new game dealt first, as Play again deals it; a
// load draws its deal at once), or a bot (any window my last move did not start).
// PROBE=1 (GATE turns it on) reads every frame after it is painted: each flight's box and each
// card's, and reports per flight the frames it stood still at its start and how far short of the
// card the board then draws it was taken off (the landing's jump), and every card on the board that
// moved more than 6 px in one frame.
// GATE=1 makes it a check (the port alone): exit 1 when more than GATE_PCT (default 2) per cent of
// the flights' frame deltas are over 20 ms, when a flight stands more than one frame at its start,
// or when one is taken off more than 1 px from where its card is then drawn.
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { launch, serve, sleep, FORGET_GAME, FOOLISH, POC } from './cdp.mjs';
import { load } from './react_rig.mjs';

const MOVES = Number(process.argv[2] || 10), CPU = Number(process.argv[3] || 1);
const ARMS = (process.env.ARMS || 'port,react').split(',');
const W = Number(process.env.W || 1024), H = Number(process.env.H || 768);
const GATE = process.env.GATE === '1', GATE_PCT = Number(process.env.GATE_PCT || 2);
if (GATE) process.env.PROBE = '1';
// PAGE=out_<commit> measures an earlier build (tools/retake.sh <commit>)
const PORT_URL = `http://127.0.0.1:8720/c/cdom_poc/${process.env.PAGE || 'out'}/index.html` + (process.env.PAGE_QUERY || '');

// Installed before any script of the page: every rAF time, every flight transition's start and end
// (a flight: a child of the fixed overlay at z-index 10000, both tables), the last DOM change.
const LOGGER = `(() => { const PROBE = ${process.env.PROBE ? 1 : 0};
  const fp = window.__fp = { frames: [], runs: [], ends: [], mut: 0 };
  const loop = (t) => { fp.frames.push(t); requestAnimationFrame(loop); };
  requestAnimationFrame(loop);
  const isFlight = (e) => { const p = e.target.parentElement; return !!p && p.style.zIndex === '10000' && p.style.position === 'fixed'; };
  addEventListener('transitionrun', (e) => { if (e.propertyName === 'top' && isFlight(e)) fp.runs.push(performance.now()); }, true);
  const end = (e) => { if (e.propertyName === 'top' && isFlight(e)) fp.ends.push(performance.now()); };
  addEventListener('transitionend', end, true);
  addEventListener('transitioncancel', end, true);
  let rendered = false, dirty = false; // PROBE: the frame's rendering has run; the DOM changed since
  new MutationObserver(() => { fp.mut = performance.now(); if (rendered) dirty = true; }).observe(document, { subtree: true, childList: true, attributes: true, characterData: true });
  if (!PROBE) return;
  // PROBE: what each frame painted, read right after it (a task posted from its rAF runs after the
  // paint, and the transitions' clock is still that frame's): every flight and every card on the
  // board, its box, its face's text and whether it is shown, while a flight is up and 700 ms after.
  // A task can run between the paint and the read (a timer, a click): a frame whose DOM changed after
  // its rendering is marked dirty, its read not what was painted. The end of the rendering is a
  // ResizeObserver's callback (after every rAF callback and the layout), on a box in a shadow root
  // (out of the MutationObserver's sight) that changes size every frame.
  fp.probe = [];
  const host = document.createElement('div');
  host.style.cssText = 'position: fixed; left: 0; top: 0; width: 0; height: 0; overflow: hidden; pointer-events: none;';
  const tick = document.createElement('div');
  host.attachShadow({ mode: 'open' }).append(tick);
  addEventListener('DOMContentLoaded', () => document.documentElement.append(host));
  let odd = 0;
  new ResizeObserver(() => { rendered = true; }).observe(tick);
  let last = -1e9;
  const ch = new MessageChannel();
  const box = (e) => { const r = e.getBoundingClientRect(); return [Math.round(r.left * 10) / 10, Math.round(r.top * 10) / 10, Math.round(r.width * 10) / 10, Math.round(r.height * 10) / 10]; };
  const txt = (e) => (e.textContent || '').replace(/\\s+/g, '').slice(0, 3);
  ch.port1.onmessage = (m) => {
    const t = m.data, wasDirty = dirty;
    rendered = false; dirty = false;
    const ov = [...document.querySelectorAll('body *')].find((e) => e.style.zIndex === '10000' && e.style.position === 'fixed');
    const fl = ov ? [...ov.children] : [];
    if (fl.length) last = t;
    if (t - last > 700) return;
    const cards = [...document.querySelectorAll('[data-card]')].map((e) => {
      const c = getComputedStyle(e);
      return [e.dataset.card, txt(e), ...box(e), c.visibility === 'hidden' || c.opacity === '0' ? 0 : 1, e.closest('[data-hand-plane]') ? 'h' : 't'];
    });
    fp.probe.push({ t, dirty: wasDirty ? 1 : 0, fl: fl.map((e) => { const f = e.firstElementChild || e; return [txt(e), ...box(f), e.style.transition === 'none' ? 0 : 1]; }), cards });
  };
  const ploop = (t) => { tick.style.width = (odd ^= 1) + 'px'; ch.port2.postMessage(t); requestAnimationFrame(ploop); };
  requestAnimationFrame(ploop);
})();`;

const PILLS = `[...document.querySelectorAll('[data-touch-interactive] > [style*="z-index: 999"] button')]`;
const handSel = '[data-hand-plane] [data-card]';
const VERB = { Attack: 'attack', Cover: 'cover', Pass: 'pass', Pickup: 'take', Good: 'sweep' };

async function session(arm) {
  const DEAL = arm === 'port' && !!process.env.DEAL;
  if (DEAL && !/debug/.test(PORT_URL)) throw new Error('DEAL=1 needs the test build: PAGE_QUERY=?debug');
  let rig = null, server = null;
  if (arm === 'react') { const { startRig } = await import('./react_rig.mjs'); rig = await startRig(); }
  else server = await serve(8720, FOOLISH);
  const b = await launch({ width: W, height: H });
  const out = { arm, windows: [], tasks: [], forced: [], load0: load() };
  try {
    await b.send('Page.addScriptToEvaluateOnNewDocument', { source: LOGGER });
    const pills = () => b.evaluate(`${PILLS}.map((e) => e.textContent.trim())`);
    const centre = (sel) => b.evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null;
      const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
    const pillAt = (name) => b.evaluate(`(() => { const e = ${PILLS}.find((e) => e.textContent.trim() === ${JSON.stringify(name)}); if (!e) return null;
      const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
    const press = async (at) => {
      await b.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: at.x, y: at.y, button: 'left', buttons: 1, clickCount: 1 });
      await sleep(70);
      await b.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: at.x, y: at.y, button: 'left', buttons: 0, clickCount: 1 });
    };
    const quiet = (ms = 700, limit = 30000) => b.until(`performance.now() - window.__fp.mut > ${ms}`, limit);
    const over = () => arm === 'react' ? b.evaluate(`document.body.textContent.includes('Continue to lobby')`) : b.evaluate(`!!document.querySelector('[data-end]')`);
    const marks = []; // [page ms, label]: what my hand did, in page time
    const mark = async (label) => marks.push([await b.evaluate('performance.now()'), label]);

    // the trace runs over the whole session; its clock is mapped to the page's by the first mark
    const events = [];
    const offData = b.listen('Tracing.dataCollected', (p) => { for (const e of p.value) events.push(e); });
    let traceDone;
    const traceStart = async () => {
      traceDone = new Promise((r) => { const o = b.listen('Tracing.tracingComplete', () => { o(); r(); }); });
      await b.send('Tracing.start', { traceConfig: { includedCategories: ['devtools.timeline', 'disabled-by-default-devtools.timeline',
        'disabled-by-default-devtools.timeline.frame', 'toplevel', 'blink.user_timing', 'benchmark', ...(process.env.SHOTS ? ['disabled-by-default-devtools.screenshot'] : [])],
      recordMode: 'recordAsMuchAsPossible', traceBufferSizeInKb: 800000 }, transferMode: 'ReportEvents' });
    };
    const traceEnd = async () => { await b.send('Tracing.end'); await traceDone; };

    if (arm === 'react') {
      const { reactTable } = await import('./react_rig.mjs');
      const t = await reactTable(b);
      await b.goto(t.url); // a load of the table as a player opens it, the deal done
      await b.until(`!!document.querySelector('${handSel}')`, 60000);
    } else {
      await b.goto('http://127.0.0.1:8720/src/styles/index.css');
      await b.evaluate(FORGET_GAME);
      await b.goto(PORT_URL);
      await b.until('window.appReady && window.app.then((x) => x.app_game_store(1) > 0)', 30000);
      await b.until(`window.app.then((x) => x.app_tex_ms(x.app_tex_count()) === x.app_tex_count())`, 60000);
    }
    await quiet(1500, 60000);
    if (CPU > 1) await b.send('Emulation.setCPUThrottlingRate', { rate: CPU });
    await traceStart();
    // FILM=1: the session filmed as record.mjs films a game (the screencast's JPEGs, each acked as it
    // comes and written after), kept as shots/run/flights_<arm>_<W>x<H>.mp4 at 60 fps, real time
    const film = [], writes = [];
    const filmDir = path.join(POC, 'out/framepace', `film_${arm}`);
    let offFilm = null;
    if (process.env.FILM) {
      fs.rmSync(filmDir, { recursive: true, force: true });
      fs.mkdirSync(filmDir, { recursive: true });
      offFilm = b.listen('Page.screencastFrame', (f) => {
        b.send('Page.screencastFrameAck', { sessionId: f.sessionId }).catch(() => {});
        const file = path.join(filmDir, `${String(film.length).padStart(5, '0')}.jpg`);
        writes.push(fs.promises.writeFile(file, Buffer.from(f.data, 'base64')));
        film.push({ t: f.metadata.timestamp * 1000, file });
      });
      await b.send('Page.startScreencast', { format: 'jpeg', quality: 92, everyNthFrame: 1 });
    }
    await sleep(200);
    // the clock pair: a user timing mark in both
    const pageT0 = await b.evaluate(`(performance.mark('framepace0'), performance.getEntriesByName('framepace0')[0].startTime)`);
    const frames0 = await b.evaluate('window.__fp.frames.length');
    const runs0 = await b.evaluate('window.__fp.runs.length');
    const ends0 = await b.evaluate('window.__fp.ends.length');
    if (DEAL) { // a new game dealt first (the test build's app_debug_deal, Play again's own call)
      await mark('deal');
      await b.evaluate('window.app.then((x) => x.app_debug_deal())');
      await sleep(1000);
    }
    let moves = 0;
    for (let tries = 0; moves < MOVES && tries < MOVES * 6; tries++) {
      await quiet();
      if (await over()) break;
      // Chrome keeps a few hundred screenshots in one trace: with them, one trace per move, cut
      // while nothing flies (one clock: the first mark maps every trace)
      if (process.env.SHOTS && tries) { await traceEnd(); await traceStart(); }
      let played = false;
      for (const card of await b.evaluate(`[...document.querySelectorAll('${handSel}')].map((e) => e.dataset.card)`)) {
        const at = await centre(`${handSel}[data-card="${card}"]`);
        if (!at) continue;
        await press(at); await sleep(200);
        const p = (await pills()).find((x) => x === 'Attack' || x === 'Cover' || x === 'Pass');
        if (p) { await sleep(150); await mark(VERB[p]); await press(await pillAt(p)); played = true; break; }
        await press(at); await sleep(150);
      }
      if (!played) {
        const p = (await pills()).find((x) => x === 'Good' || x === 'Pickup');
        if (p) { await mark(VERB[p]); await press(await pillAt(p)); played = true; }
      }
      if (played) { moves++; await sleep(1500); } else await sleep(1000); // the bot's pace
    }
    await quiet(3500, 30000).catch(() => {}); // the bot's last answer flies
    await quiet(1000, 30000);
    const fp = await b.evaluate(`({ frames: window.__fp.frames.slice(${frames0}), runs: window.__fp.runs.slice(${runs0}), ends: window.__fp.ends.slice(${ends0}), probe: window.__fp.probe || [] })`);
    out.probe = fp.probe;
    out.timeOrigin = await b.evaluate('performance.timeOrigin');
    await traceEnd();
    offData();
    if (offFilm) {
      await b.send('Page.stopScreencast');
      offFilm();
      await Promise.all(writes);
      const list = path.join(filmDir, 'list.txt'), end = film.at(-1).t + 100;
      fs.writeFileSync(list, film.map((f, i) => `file '${f.file}'\nduration ${(((film[i + 1]?.t ?? end) - f.t) / 1000).toFixed(4)}`).join('\n') + `\nfile '${film.at(-1).file}'\n`);
      out.filmFrames = film.map((f) => [f.t, f.file]);
      out.film = path.join(POC, 'shots/run', `flights_${arm}${process.env.PAGE ? '_' + process.env.PAGE : ''}_${W}x${H}.mp4`);
      execFileSync('ffmpeg', ['-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', list,
        '-vf', 'fps=60,scale=trunc(iw/2)*2:trunc(ih/2)*2', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '20', out.film]);
    }
    out.load1 = load();
    out.moves = moves;
    out.frames = fp.frames;
    out.marks = marks;

    // the flight windows: transitions that overlap or touch (within 80 ms: a beat ends a frame early) are one window
    const iv = fp.runs.map((s) => [s, fp.ends.find((e) => e >= s) ?? s + 600]).sort((x, y) => x[0] - y[0]);
    const wins = [];
    for (const [s, e] of iv) {
      const w = wins.at(-1);
      if (w && s <= w.end + 80) w.end = Math.max(w.end, e); else wins.push({ start: s, end: e });
    }
    // named by my last mark before it, when it began within 1.2 s of it; else a bot's
    for (const w of wins) {
      const m = marks.filter(([t]) => t <= w.start).at(-1);
      w.label = m && w.start - m[0] < 1200 ? m[1] : 'bot';
      // the flight's start frame stands 25 ms before its transition runs: the window opens there
      const f = fp.frames.filter((t) => t >= w.start - 30 && t <= w.end + 1);
      const d = f.slice(1).map((t, i) => t - f[i]);
      w.frames = f.length; w.deltas = d;
    }
    out.windows = wins;

    // the trace: the page's main thread (the renderer's CrRendererMain with our mark)
    if (process.env.DUMP) { const c = {}; for (const e of events) c[e.cat + ' ' + e.name] = (c[e.cat + ' ' + e.name] || 0) + 1; fs.writeFileSync(path.join(POC, 'out/framepace', `${arm}_names.json`), JSON.stringify(c, null, 1)); fs.writeFileSync(path.join(POC, 'out/framepace', `${arm}_pipeline.json`), JSON.stringify(events.filter((e) => e.name === 'PipelineReporter').slice(0, 400))); }
    const mk = events.find((e) => e.name === 'framepace0');
    if (!mk) throw new Error(`${arm}: the trace has no mark`);
    const { pid, tid } = mk;
    const toPage = (ts) => (ts - mk.ts) / 1000 + pageT0;
    const mine = events.filter((e) => e.pid === pid && e.tid === tid && e.ph === 'X');
    const inWin = (t) => wins.find((w) => t >= w.start - 30 && t <= w.end + 1);
    const tasks = mine.filter((e) => e.name === 'RunTask').sort((x, y) => x.ts - y.ts);
    const kids = mine.filter((e) => e.name !== 'RunTask').sort((x, y) => x.ts - y.ts);
    const KIND = { FunctionCall: 'script', EvaluateScript: 'script', TimerFire: 'script', FireAnimationFrame: 'script', EventDispatch: 'script', 'v8.callFunction': 'script',
      UpdateLayoutTree: 'style', RecalculateStyles: 'style', Layout: 'layout', Paint: 'paint', PrePaint: 'prepaint', Layerize: 'layerize',
      'Decode Image': 'decode', 'Decode LazyPixelRef': 'decode', ImageDecodeTask: 'decode', CommitLoad: 'other', ParseHTML: 'other' };
    for (const t of tasks) {
      const at = toPage(t.ts), w = inWin(at);
      if (!w || t.dur < 16000) continue;
      const parts = {};
      for (const e of kids) {
        if (e.ts < t.ts || e.ts > t.ts + t.dur) continue;
        const k = KIND[e.name];
        if (!k || k === 'script') continue; // script is what is left
        parts[k] = (parts[k] || 0) + e.dur / 1000;
      }
      const s = (t.dur / 1000) - Object.values(parts).reduce((a, v) => a + v, 0);
      out.tasks.push({ at, label: w.label, ms: t.dur / 1000, parts: { script: Math.max(0, s), ...parts } });
    }
    // the compositor's own account of each frame (PipelineReporter, the frames the Performance panel
    // marks dropped): in the windows, frames that animate, presented or dropped
    out.pipe = {};
    for (const e of events) {
      const fr = e.name === 'PipelineReporter' && e.ph === 'b' && e.args?.frame_reporter;
      if (!fr || !(fr.affects_smoothness || fr.has_main_animation || fr.has_compositor_animation)) continue;
      const w = inWin(toPage(e.ts));
      if (!w) continue;
      const k = w.label + ' ' + fr.state;
      out.pipe[k] = (out.pipe[k] || 0) + 1;
    }
    // SHOTS=1: every frame the compositor drew in each window (the trace's own screenshots, no
    // ack, each with its time), from 150 ms before it to 400 ms after, as JPEGs named by ms since the window opened plus 1000
    if (process.env.SHOTS) {
      const shots = events.filter((e) => e.name === 'Screenshot' && e.args?.snapshot).map((e) => ({ at: toPage(e.ts), data: e.args.snapshot }));
      const dir = path.join(POC, 'out/framepace', `${arm}${process.env.PAGE ? '_' + process.env.PAGE : ''}_shots`);
      fs.rmSync(dir, { recursive: true, force: true });
      wins.forEach((w, i) => {
        const d = path.join(dir, `${String(i).padStart(2, '0')}_${w.label}`);
        fs.mkdirSync(d, { recursive: true });
        for (const f of shots.filter((f) => f.at >= w.start - 150 && f.at <= w.end + 400))
          fs.writeFileSync(path.join(d, `${Math.round(f.at - w.start + 1000)}.jpg`), Buffer.from(f.data, 'base64'));
      });
      out.shots = shots.length;
    }
    // forced layouts: a layout or style recalc inside an rAF callback (a read after a write in the frame loop)
    const rafs = kids.filter((e) => e.name === 'FireAnimationFrame');
    for (const e of kids) {
      if (e.name !== 'Layout' && e.name !== 'UpdateLayoutTree') continue;
      const r = rafs.find((f) => e.ts >= f.ts && e.ts + e.dur <= f.ts + f.dur);
      if (!r) continue;
      const at = toPage(e.ts), w = inWin(at);
      if (e.dur < 50) continue; // an empty check (nothing dirty) is not a layout
      out.forced.push({ at, label: w ? w.label : '-', name: e.name, ms: e.dur / 1000 });
    }
  } finally {
    await b.close();
    if (server) server.close();
    if (rig) await rig.stop();
  }
  return out;
}

fs.mkdirSync(path.join(POC, 'out/framepace'), { recursive: true });
const results = [];
process.on('exit', () => {
  // FILM with both arms: React left, the port right, as long as the shorter
  const films = Object.fromEntries(results.filter((r) => r.film).map((r) => [r.arm, r.film]));
  if (!films.port || !films.react) return;
  // (this ffmpeg has no drawtext: the file's name says which is which)
  execFileSync('ffmpeg', ['-y', '-loglevel', 'error', '-i', films.react, '-i', films.port, '-filter_complex',
    '[0:v][1:v]hstack=inputs=2:shortest=1', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '20',
    path.join(POC, 'shots/run', `side_by_side_react_left_port_right_${W}x${H}.mp4`)]);
});
for (const arm of ARMS) {
  const r = await session(arm);
  results.push(r);
  fs.mkdirSync(path.join(POC, 'out/framepace'), { recursive: true });
  fs.writeFileSync(path.join(POC, 'out/framepace', `${arm}${process.env.PAGE ? '_' + process.env.PAGE : ''}_cpu${CPU}.json`), JSON.stringify(r)); // debugging output
}

// Every flight's path from the probe: its centre frame by frame (a flight is known by its face's
// text: a back has none and is left out), the frames it stood at its start, and the distance from its
// last centre to the centre of its card on the board the frame it went (NaN: not on the board, the
// discard).
function flightPaths(P) {
  const c = (b) => [b[0] + b[2] / 2, b[1] + b[3] / 2];
  const done = [], open = new Map();
  P = P.filter((p) => !p.dirty); // a read after a task that changed the page is not what was painted
  for (let i = 0; i < P.length; i++) {
    const gap = i > 0 && P[i].t - P[i - 1].t > 40; // the probe paused: whatever was open is unknown
    if (gap) open.clear();
    const names = new Set(P[i].fl.map((f) => f[0]));
    for (const f of P[i].fl) { if (!f[0]) continue; if (!open.has(f[0])) open.set(f[0], { T: f[0], pts: [] }); open.get(f[0]).pts.push(c(f.slice(1))); }
    for (const [k, v] of open) {
      if (names.has(k)) continue;
      const b = P[i].cards.find((q) => q[1] === k && q[6]);
      const p = v.pts, last = p.at(-1);
      const steps = p.slice(1).map((q, j) => Math.hypot(q[0] - p[j][0], q[1] - p[j][1]));
      const still = steps.findIndex((x) => x > 0.5);
      done.push({ T: k, frames: p.length, still: still < 0 ? p.length : still + 1, rem: b ? Math.hypot(c(b.slice(2, 6))[0] - last[0], c(b.slice(2, 6))[1] - last[1]) : NaN });
      open.delete(k);
    }
  }
  return done.filter((f) => f.frames > 3);
}

// ---- the report ------------------------------------------------------------------------------
const med = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.floor(s.length / 2)] : NaN; };
let gateFail = false;
for (const r of results) {
  const all = r.windows.flatMap((w) => w.deltas);
  const vsync = med(r.frames.slice(1).map((t, i) => t - r.frames[i]).filter((d) => d > 4 && d < 40));
  console.log(`\n${r.arm}, CPU x${CPU}, ${r.moves} moves, vsync ${vsync.toFixed(2)} ms; load ${r.load0} -> ${r.load1}`);
  console.log('| flights | windows | ms in the air | frames | expected | lost | deltas > 20 ms | worst gap | long tasks (> 16 ms) |');
  console.log('| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |');
  const labels = [...new Set(r.windows.map((w) => w.label))];
  for (const label of [...labels, 'all']) {
    const ws = label === 'all' ? r.windows : r.windows.filter((w) => w.label === label);
    const d = ws.flatMap((w) => w.deltas);
    const ms = ws.reduce((s, w) => s + (w.end - w.start + 30), 0);
    const lost = d.reduce((s, x) => s + Math.max(0, Math.round(x / vsync) - 1), 0);
    const tasks = r.tasks.filter((t) => label === 'all' || t.label === label);
    const tdesc = tasks.length ? `${tasks.length}, worst ${Math.max(...tasks.map((t) => t.ms)).toFixed(1)} ms` : '0';
    console.log(`| ${label} | ${ws.length} | ${ms.toFixed(0)} | ${d.length + ws.length} | ${(ms / vsync).toFixed(0)} | ${lost} | ${d.filter((x) => x > 20).length} (${(100 * d.filter((x) => x > 20).length / Math.max(1, d.length)).toFixed(1)}%) | ${d.length ? Math.max(...d).toFixed(1) : '-'} | ${tdesc} |`);
  }
  for (const t of r.tasks.sort((x, y) => y.ms - x.ms).slice(0, 8))
    console.log(`  long task ${t.ms.toFixed(1)} ms (${t.label}): ${Object.entries(t.parts).filter(([, v]) => v >= 0.5).map(([k, v]) => `${k} ${v.toFixed(1)}`).join(', ')}`);
  console.log(`  compositor frames in the windows (PipelineReporter): ${Object.entries(r.pipe).sort().map(([k, v]) => `${k} ${v}`).join(', ')}`);
  const forcedIn = r.forced.filter((f) => f.label !== '-');
  console.log(`  forced layout/style inside rAF: ${r.forced.length} (${forcedIn.length} in flight windows, ${r.forced.reduce((s, f) => s + f.ms, 0).toFixed(1)} ms)`);
  for (const f of r.forced.slice(0, 6)) console.log(`    ${f.name} ${f.ms.toFixed(2)} ms (${f.label})`);
  if (r.probe?.length) {
    const fl = flightPaths(r.probe);
    const ends = fl.filter((f) => Number.isFinite(f.rem));
    const st = {};
    for (const f of fl) st[f.still] = (st[f.still] || 0) + 1;
    const rs = ends.map((f) => f.rem).sort((x, y) => x - y);
    console.log(`  flights probed: ${fl.length}; still frames at the start: ${Object.entries(st).map(([k, v]) => `${k}: ${v}`).join(', ')}; ` +
      `taken off short of the card drawn (px, ${ends.length} landings): median ${med(rs).toFixed(1)}, max ${rs.length ? rs.at(-1).toFixed(1) : '-'}, over 1 px: ${rs.filter((x) => x > 1).length}`);
    r.paths = fl;
  }
  if (GATE && r.arm === 'port') {
    const fl = r.paths || [];
    const stood = fl.filter((f) => f.still > 1), short = fl.filter((f) => f.rem > 1);
    if (stood.length) { console.log(`GATE: ${stood.length} flights stood more than one frame at their start`); gateFail = true; }
    if (short.length) { console.log(`GATE: ${short.length} flights were taken off more than 1 px short (${short.slice(0, 5).map((f) => `${f.T} ${f.rem.toFixed(1)}`).join(', ')})`); gateFail = true; }
    const pct = 100 * all.filter((x) => x > 20).length / Math.max(1, all.length);
    if (pct > GATE_PCT) { console.log(`GATE: ${pct.toFixed(1)}% of flight frame deltas over 20 ms (limit ${GATE_PCT}%)`); gateFail = true; }
    if (!r.windows.length) { console.log('GATE: no flight was measured'); gateFail = true; }
  }
}
if (gateFail) process.exit(1);
