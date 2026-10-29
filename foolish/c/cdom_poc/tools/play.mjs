// Plays the human seat to the end of the game as a person would, each move once the last move's
// animations have landed. Returns the moves made. Four hands:
//   clicks  the first card any verb takes: select it, press its pill; else Good, else Pickup;
//   drag    the first card that drops somewhere as a move: dragged by the mouse onto each open
//           battle, then onto the open table, let go where the kernel reads a move (the shadow's
//           badge), else brought back to its place; Good and Pickup are pills (no drag makes them);
//   touch   the same by one finger;
//   keys    the arrow cursor alone: on each card in turn, Up (and Up again when it aims), else
//           Down (Take or Good).
import { key } from './input.mjs';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// onStep, when given, is awaited after every click (a pill may have come or gone).
export async function playToEnd(b, maxMoves = 400, onStep = null, mode = 'clicks') {
  const all = (sel, expr) => b.evaluate(`[...document.querySelectorAll(${JSON.stringify(sel)})].map((e) => ${expr})`);
  const turn = () => b.evaluate(`document.querySelector('[data-game-container]').dataset.turn`);
  const pills = () => all('[data-pill]', 'e.dataset.pill');
  const click = async (sel) => { await b.click(sel); if (onStep) await onStep(); };
  const runs = () => b.evaluate('window.app.then((x) => x.app_anim_runs())');
  const at = (sel) => b.evaluate(`(() => { const r = document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; })()`);
  const restingHand = () => b.until(`[...document.querySelectorAll('[data-hand-plane] [data-card]')].every((e) => { const c = getComputedStyle(e);
    return ['left', 'bottom', 'width', 'height'].every((p) => Math.abs(parseFloat(c[p]) - parseFloat(e.style[p])) < 0.05); })`, 2000);
  const touch = mode === 'touch';
  const point = (type, p) => touch
    ? b.send('Input.dispatchTouchEvent', { type: { press: 'touchStart', move: 'touchMove', release: 'touchEnd' }[type], touchPoints: type === 'release' ? [] : [{ x: p.x, y: p.y, id: 1 }] })
    : b.send('Input.dispatchMouseEvent', { type: { press: 'mousePressed', move: 'mouseMoved', release: 'mouseReleased' }[type], x: p.x, y: p.y, button: 'left', buttons: type === 'release' ? 0 : 1, clickCount: 1 });
  // A card dragged straight to each target in turn; let go on the first the kernel reads as a move.
  const dragPlay = async (card, targets) => {
    await restingHand();
    const from = await at(`[data-hand-plane] [data-card="${card}"]`);
    await point('press', from);
    for (const to of targets) {
      await point('move', to);
      await point('move', { x: to.x + 1, y: to.y });
      if (await b.evaluate('window.app.then((x) => x.app_drag(1))') >= 0) { await point('release', to); return true; }
    }
    await point('move', { x: from.x, y: from.y - 200 });   // back to its own place, from straight above it
    await point('move', from);
    await point('release', from);
    return false;
  };
  let moves = 0;
  for (; moves < maxMoves; moves++) {
    await b.until(`document.querySelector('[data-game-container]').dataset.turn !== 'bot'`, 15000);
    await b.until('window.app.then((x) => !x.app_anim_busy())', 30000); // the run has landed: the board is the game's
    if (onStep) await onStep();
    if ((await turn()) === 'over') return moves;
    let played = false;
    const hand = await all('[data-hand-plane] [data-card]', 'e.dataset.card');
    if (mode === 'clicks') {
      for (const card of hand) {
        await click(`[data-hand-plane] [data-card="${card}"]`);
        const p = (await pills()).filter((x) => x === 'Attack' || x === 'Cover' || x === 'Pass');
        if (p.length) { await click(`[data-pill="${p[0]}"]`); played = true; break; }
        await click(`[data-hand-plane] [data-card="${card}"]`);
      }
    } else if (mode === 'drag' || mode === 'touch') {
      const vw = await b.evaluate('innerWidth'), vh = await b.evaluate('innerHeight');
      const open = await all('[data-table-container] > [data-location="table"]', 'e.querySelectorAll("[data-card]").length === 1 ? e.querySelector("[data-card]").dataset.card : ""');
      const targets = [];
      for (const c of open.filter(Boolean)) targets.push(await at(`[data-table-container] [data-battle-index][data-card="${c}"]`));
      targets.push({ x: vw / 2, y: vh * 0.3 });   // the open table, above the row
      for (const card of hand) {
        const r0 = await runs();
        if (await dragPlay(card, targets)) { await b.until(`window.app.then((x) => x.app_anim_runs() > ${r0} || x.app_last_reject())`, 3000); played = true; break; }
      }
    } else {
      const state = () => b.evaluate(`(() => { const e = document.querySelector('[data-testid="kbd-state"]'); return [e.dataset.mode, e.dataset.sel]; })()`);
      for (let i = 0; i < hand.length && !played; i++) {
        for (let n = 0; n < hand.length + 1 && (await state())[1] !== hand[i]; n++) await key(b, 'ArrowRight');
        const r0 = await runs();
        await key(b, 'ArrowUp');
        if ((await state())[0] === 'target') await key(b, 'ArrowUp');
        played = await runs() > r0;
      }
    }
    if (played) continue;
    const p = await pills();
    if (mode === 'keys' && (p.includes('Good') || p.includes('Pickup'))) await key(b, 'ArrowDown');
    else if (p.includes('Good')) await click('[data-pill="Good"]');
    else if (p.includes('Pickup')) await click('[data-pill="Pickup"]');
    else {
      const sel = await all('[data-hand-plane] [data-card]', 'e.style.borderColor');
      const table = await all('[data-table-container] [data-card]', 'e.dataset.card');
      const reject = await b.evaluate('window.app.then((x) => x.app_last_reject())');
      throw new Error(`no move offered: hand ${hand.join(' ')} (borders ${sel.join(' ')}), table ${table.join(' ')}, pills ${p.join(' ')}, engine reject ${reject}`);
    }
    await sleep(0);
  }
  throw new Error(`no end after ${maxMoves} moves`);
}

// node tools/play.mjs [clicks|drag|touch|keys] [games]: games played out on the test build, the bot fast.
if (import.meta.url === `file://${process.argv[1]}`) {
  const { launch, serve, FOOLISH, openTable } = await import('./cdp.mjs');
  const mode = process.argv[2] || 'clicks', games = Number(process.argv[3] || 1);
  const server = await serve(8720, FOOLISH);
  const b = await launch();
  try {
    await openTable(b, 'http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug', { fast: true });
    await b.evaluate('window.app.then((x) => x.app_debug_anim_speed(8))');
    for (let g = 0; g < games; g++) {
      const t0 = Date.now(), moves = await playToEnd(b, 400, null, mode);
      console.log(`${mode}: game ${g + 1} over after ${moves} moves of mine (${Math.round((Date.now() - t0) / 1000)} s): ${await b.evaluate(`document.querySelector('[data-end] h1').textContent`)}`);
      await b.click('[data-end] button');
    }
  } finally { await b.close(); server.close(); }
}
