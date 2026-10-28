// Plays the human seat to the end of the game by clicks, as a person would: the first card any
// verb takes (select it, press its pill), else Good, else Pickup. Returns the moves made.
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// onStep, when given, is awaited after every click (a pill may have come or gone).
export async function playToEnd(b, maxMoves = 400, onStep = null) {
  const all = (sel, expr) => b.evaluate(`[...document.querySelectorAll(${JSON.stringify(sel)})].map((e) => ${expr})`);
  const turn = () => b.evaluate(`document.querySelector('[data-game-container]').dataset.turn`);
  const pills = () => all('[data-pill]', 'e.dataset.pill');
  const click = async (sel) => { await b.click(sel); if (onStep) await onStep(); };
  let moves = 0;
  for (; moves < maxMoves; moves++) {
    await b.until(`document.querySelector('[data-game-container]').dataset.turn !== 'bot'`, 15000);
    if (onStep) await onStep();
    if ((await turn()) === 'over') return moves;
    let played = false;
    const hand = await all('[data-hand-plane] [data-card]', 'e.dataset.card');
    for (const card of hand) {
      await click(`[data-hand-plane] [data-card="${card}"]`);
      const p = (await pills()).filter((x) => x === 'Attack' || x === 'Cover' || x === 'Pass');
      if (p.length) { await click(`[data-pill="${p[0]}"]`); played = true; break; }
      await click(`[data-hand-plane] [data-card="${card}"]`);
    }
    if (played) continue;
    const p = await pills();
    if (p.includes('Good')) await click('[data-pill="Good"]');
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
