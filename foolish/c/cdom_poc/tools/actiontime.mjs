// What a move costs the page's main thread, the port against the REAL React app (its production
// build against a local c_base, tools/react_rig.mjs), paired and interleaved in one Chrome, measured
// the same way for both: Chrome's own trace (CDP Tracing, devtools.timeline), every top-level task
// on the page's main thread (RunTask: the script, its microtasks, style, layout and paint it ran).
//   node tools/actiontime.mjs [rounds] [cpu-throttle]
// Each round loads both tables, in an order that alternates, as a player opens them (the port's
// release build, out/index.html; React's table at /<game id>), textures warm in both stores, and on
// each: waits for a move of mine (a card whose pills offer Attack, Cover or Pass; Good or Take is
// pressed untimed and the bot waited for, when no card has one), then, traced:
//   select   a real tap on that card (CDP mouse press and release): the tasks of its events, and
//            those plus the frames in the 300 ms after (what draws the selection);
//   play     a real click on its pill: the tasks of its events (the port applies the move and arms
//            the flight; React sends it, judges it and enqueues its flight), and the run after: every
//            task in the 2 s after (the flights' frames, timers, paint), with its frame count;
//   bot      the bot's answer, which both pace 3 s after the last flight lands (React's may wait for
//            its 5 s bump besides): the longest task that a timer (the port's bot step) or a socket
//            message (React's push) ran after the play's run, and every task in the 2.5 s after it.
// A frame is a task that rendered (AnimationFrame::Render or Paint in it).
// The two tables do not hold the same board (React's game is a live server game that goes on from
// round to round, the port's is the golden board): the move is whatever card is playable first, so
// the samples mix attacks, covers and passes in both arms.
import { launch, sleep, FORGET_GAME } from './cdp.mjs';
import { startRig, reactTable, load, top, PORT_ORIGIN } from './react_rig.mjs';

const ROUNDS = Number(process.argv[2] || 5), CPU = Number(process.argv[3] || 1);
const PORT = PORT_ORIGIN + '/c/cdom_poc/out/index.html';
const rig = await startRig();
const b = await launch();
// The page's own quiet: the last DOM change (both tables), installed before any script of the page.
await b.send('Page.addScriptToEvaluateOnNewDocument', { source: `window.__mut = 0; new MutationObserver(() => { window.__mut = performance.now(); })
  .observe(document, { subtree: true, childList: true, attributes: true, characterData: true });` });

const all = (sel, expr) => b.evaluate(`[...document.querySelectorAll(${JSON.stringify(sel)})].map((e) => ${expr})`);
// The pill column is the same DOM in both (ActionButtons.tsx; the port copies it): buttons in the
// box at z-index 999. A pill's name is its text.
const PILLS = `[...document.querySelectorAll('[data-touch-interactive] > [style*="z-index: 999"] button')]`;
const pills = () => b.evaluate(`${PILLS}.map((e) => e.textContent.trim())`);
const centre = (sel) => b.evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null;
  const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
const pillAt = (name) => b.evaluate(`(() => { const e = ${PILLS}.find((e) => e.textContent.trim() === ${JSON.stringify(name)}); if (!e) return null;
  const r = e.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
const press = async (at) => {
  await b.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: at.x, y: at.y, button: 'left', buttons: 1, clickCount: 1 });
  await sleep(60);
  await b.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: at.x, y: at.y, button: 'left', buttons: 0, clickCount: 1 });
};
// Quiet: no DOM change for ms (the run has landed, the hand has stopped sliding).
const quiet = (ms = 700, limit = 30000) => b.until(`performance.now() - window.__mut > ${ms}`, limit);
const handSel = '[data-hand-plane] [data-card]';
const MOVES = ['Attack', 'Cover', 'Pass'];
const SAY = { Attack: 'Attack', Cover: 'Cover', Pass: 'Pass', Good: 'Good', Pickup: 'Pickup' }; // the pills' English text, the same in both

// Waits for a move of mine and returns its card and pill (untimed taps find it; the card left unselected).
async function findMove(arm) {
  for (let tries = 0; tries < 12; tries++) {
    await quiet();
    if (arm === 'react' && await b.evaluate(`document.body.textContent.includes('Continue to lobby')`)) return 'over';
    if (arm === 'port' && await b.evaluate(`!!document.querySelector('[data-end]')`)) return 'over';
    for (const card of await all(handSel, 'e.dataset.card')) {
      const at = await centre(`${handSel}[data-card="${card}"]`);
      if (!at) continue;
      await press(at); await sleep(150);
      const p = (await pills()).filter((x) => MOVES.some((m) => SAY[m] === x));
      await press(at); await sleep(150);
      if (p.length) return { card, pill: p[0] };
    }
    const p = await pills();
    const other = p.find((x) => x === SAY.Good || x === SAY.Pickup);
    if (other) { await press(await pillAt(other)); await sleep(500); }
    await sleep(3500); // the bot's pace
  }
  throw new Error(`${arm}: no move of mine came`);
}

// One trace: the select, the play and the bot's answer.
async function traced(arm, move) {
  const events = [];
  const off = b.listen('Tracing.dataCollected', (p) => { for (const e of p.value) events.push(e); });
  const done = new Promise((r) => { const o = b.listen('Tracing.tracingComplete', () => { o(); r(); }); });
  await b.send('Tracing.start', { traceConfig: { includedCategories: ['devtools.timeline', 'disabled-by-default-devtools.timeline', 'toplevel'], recordMode: 'recordAsMuchAsPossible' }, transferMode: 'ReportEvents' });
  await sleep(100);
  await press(await centre(`${handSel}[data-card="${move.card}"]`));
  await sleep(400);
  const at = await pillAt(move.pill);
  if (!at) throw new Error(`${arm}: the ${move.pill} pill went`);
  await press(at);
  // the run lands; then the bot's answer comes (both pace it 3 s after the landing; React's may wait
  // for its 5 s bump besides) and its flights land
  await sleep(1000);
  await quiet(700, 30000);
  const settled = await b.evaluate('performance.now()');
  await b.until(`window.__mut > ${settled}`, 30000);
  await sleep(3000);
  await b.send('Tracing.end');
  await done;
  off();
  // The page's main thread: the renderer thread that dispatched the taps.
  const taps = events.filter((e) => e.name === 'EventDispatch' && e.args?.data?.type === 'mousedown');
  if (taps.length < 2) throw new Error(`${arm}: ${taps.length} mousedowns in the trace`);
  const { pid, tid } = taps[0];
  const mine = events.filter((e) => e.pid === pid && e.tid === tid);
  const tasks = mine.filter((e) => e.name === 'RunTask' && e.ph === 'X').sort((x, y) => x.ts - y.ts);
  const inside = (t, e) => e.ts >= t.ts && e.ts <= t.ts + t.dur;
  const holding = (pred) => tasks.filter((t) => mine.some((e) => pred(e) && inside(t, e)));
  const sum = (list) => list.reduce((s, t) => s + t.dur, 0) / 1000;
  const between = (from, to) => tasks.filter((t) => t.ts >= from && t.ts < to);
  const frames = (list) => list.filter((t) => mine.some((e) => (e.name === 'AnimationFrame::Render' || e.name === 'Paint') && inside(t, e))).length;
  const pointer = (e) => e.name === 'EventDispatch' && ['mousedown', 'mouseup', 'click'].includes(e.args?.data?.type);
  const input = holding(pointer);
  const [d0, d1] = taps.map((e) => e.ts);
  const selTasks = input.filter((t) => t.ts + t.dur >= d0 && t.ts < d1 - 1000);
  const playTasks = input.filter((t) => t.ts + t.dur >= d1);
  const selEnd = Math.max(...selTasks.map((t) => t.ts + t.dur)), playEnd = Math.max(...playTasks.map((t) => t.ts + t.dur));
  const selAfter = between(selEnd, selEnd + 300000).filter((t) => !selTasks.includes(t));
  const playRun = between(playEnd, playEnd + 2000000).filter((t) => !playTasks.includes(t));
  // the bot's answer: the longest task 2.5 s or more after the play that a timer (the port) or a
  // message (React's socket) ran
  const late = between(playEnd + 2500000, Infinity);
  const carriers = late.filter((t) => mine.some((e) => inside(t, e) && (e.name === 'TimerFire' || (e.name === 'EventDispatch' && e.args?.data?.type === 'message'))));
  const bot = carriers.sort((x, y) => y.dur - x.dur)[0];
  const botRun = bot ? between(bot.ts + bot.dur, bot.ts + bot.dur + 2500000) : [];
  return {
    sel: sum(selTasks), selDrawn: sum(selTasks) + sum(selAfter),
    play: sum(playTasks), playRun: sum(playRun), playFrames: frames(playRun),
    bot: bot ? bot.dur / 1000 : NaN, botRun: sum(botRun), botFrames: frames(botRun), pill: move.pill,
  };
}

let reactUrl = null;
async function open(arm) {
  await b.goto('about:blank');
  await sleep(300);
  if (arm === 'port') {
    await b.goto(PORT);
    await b.until('window.appReady && window.app.then((x) => x.app_game_store(1) > 0)', 30000);
  } else {
    await b.goto(reactUrl);
    await b.until(`!!document.querySelector('${handSel}')`, 60000);
  }
  return b.until(`performance.getEntriesByName('first-contentful-paint')[0]?.startTime`, 10000);
}
async function sample(arm) {
  let fcp = await open(arm);
  let move = await findMove(arm);
  if (move === 'over') { // a finished game: a new one (React: a new table; the port: Play again)
    if (arm === 'react') { reactUrl = (await reactTable(b)).url; fcp = await open(arm); }
    else { await press(await centre('[data-end] button')); }
    move = await findMove(arm);
  }
  const r = await traced(arm, move);
  return { ...r, fcp };
}

const arms = { port: [], react: [] };
const notes = [];
try {
  reactUrl = (await reactTable(b)).url;
  // the port's store forgotten once (the golden board), then both warmed by one load each
  await b.goto(PORT_ORIGIN + '/src/styles/index.css');
  await b.evaluate(FORGET_GAME);
  for (const arm of ['port', 'react']) { await open(arm); await sleep(3000); }
  notes.push({ when: 'before', load: load(), top: top() });
  if (CPU > 1) await b.send('Emulation.setCPUThrottlingRate', { rate: CPU });
  for (let i = 0; i < ROUNDS; i++) {
    for (const arm of i % 2 ? ['react', 'port'] : ['port', 'react']) {
      const r = await sample(arm);
      arms[arm].push(r);
      console.log(`round ${i + 1} ${arm.padEnd(5)} ${r.pill.padEnd(6)} fcp ${r.fcp?.toFixed(0)} select ${r.sel.toFixed(2)}/${r.selDrawn.toFixed(2)} play ${r.play.toFixed(2)} run ${r.playRun.toFixed(2)}/${r.playFrames} bot ${r.bot.toFixed(2)} run ${r.botRun.toFixed(2)}/${r.botFrames}`);
    }
  }
  notes.push({ when: 'after', load: load(), top: top() });
} catch (e) { console.error(e); process.exitCode = 1; }
finally { await b.close(); await rig.stop(); }
if (process.exitCode) process.exit();

const q = (a, p) => { const s = a.filter((v) => Number.isFinite(v)).sort((x, y) => x - y); return s.length ? s[Math.min(s.length - 1, Math.floor(p * (s.length - 1) + 0.5))] : NaN; };
const cell = (a, d = 2) => `${q(a, 0.5).toFixed(d)} (${q(a, 0.25).toFixed(d)}-${q(a, 0.75).toFixed(d)})`;
console.log(`\na move's main-thread cost, port against the React app, ${ROUNDS} rounds, CPU x${CPU}: median (25th-75th), ms`);
for (const n of notes) console.log(`load ${n.when}: ${n.load} | ${n.top}`);
const rows = [['first contentful paint (warm)', 'fcp', 0], ['select: its tasks', 'sel'], ['select: with the frames that draw it', 'selDrawn'],
  ['play: its tasks', 'play'], ['play: the run after (2 s)', 'playRun'], ['play: frames in the run', 'playFrames', 0], ['play: per frame of the run', 'playPer'],
  ['bot: the task that brings it', 'bot'], ['bot: the run after (2.5 s)', 'botRun'], ['bot: frames in the run', 'botFrames', 0], ['bot: per frame of the run', 'botPer']];
for (const arm of ['port', 'react']) for (const r of arms[arm]) { r.playPer = r.playRun / Math.max(1, r.playFrames); r.botPer = r.botRun / Math.max(1, r.botFrames); }
console.log(`| | port | React | port / React |\n| --- | ---: | ---: | ---: |`);
for (const [label, k, d] of rows) {
  const p = arms.port.map((r) => r[k]), rr = arms.react.map((r) => r[k]);
  console.log(`| ${label} | ${cell(p, d ?? 2)} | ${cell(rr, d ?? 2)} | ${(q(p, 0.5) / q(rr, 0.5)).toFixed(2)} |`);
}
console.log(`moves: port ${arms.port.map((r) => r.pill).join(' ')}; React ${arms.react.map((r) => r.pill).join(' ')}`);
