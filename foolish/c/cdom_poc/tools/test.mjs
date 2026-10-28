// The cdom table in headless Chrome: the first paint is the React golden's DOM for the same
// board, a click sequence plays a legal move and the bot answers through keyed patches, and a
// game played out by clicks reaches its end screen. Exits non-zero on the first failure.
//
//   node tools/test.mjs        (after ./build.sh)
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { launch, serve, FOOLISH } from './cdp.mjs';
import { playToEnd } from './play.mjs';
import { consistRun, READ_DOM, clickCard } from './consist.mjs';

const golden = fs.readFileSync(path.join(FOOLISH, 'e2e/fixtures/ui_dom/bout_2p_attacker.html'), 'utf8');
const server = await serve(8720, FOOLISH);
const b = await launch();
const q = (sel) => b.evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? e.outerHTML : null; })()`);
const all = (sel, expr) => b.evaluate(`[...document.querySelectorAll(${JSON.stringify(sel)})].map((e) => ${expr})`);
const turn = () => b.evaluate(`document.querySelector('[data-game-container]').dataset.turn`);
const pills = () => all('[data-pill]', 'e.dataset.pill');
const settle = async () => { await b.until(`document.querySelector('[data-game-container]').dataset.turn !== 'bot'`, 15000); };
// The test build (?debug): the bot's steps held for the test to release one by one, or its waits
// made a hundredth of the kernel's for the games played out.
const hold = () => b.evaluate(`window.app.then((x) => x.app_debug_hold(1))`);
const fast = () => b.evaluate(`window.app.then((x) => x.app_debug_fast(1))`);
const releaseBot = async () => {
  await b.until(`window.app.then((x) => x.app_debug_due())`, 10000);
  await b.evaluate(`window.app.then((x) => x.app_debug_release())`);
  await b.until(`window.app.then((x) => !x.app_debug_due())`);
  await new Promise((r) => setTimeout(r, 50));
};
const ready = async () => { await b.until(`window.appReady`); await b.until(`window.app.then((x) => x.app_game_store(1) > 0)`); };
let failed = 0;
async function check(name, fn) {
  try { await fn(); console.log(`ok   ${name}`); } catch (e) { failed++; console.log(`FAIL ${name}\n     ${e.message.split('\n').join('\n     ')}`); }
}

try {
  await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug');
  await b.until(`window.appReady`);
  await hold(); // the bot may act on the golden's board (it defends): its step waits for the test

  await check('first paint: the React golden DOM for the same board, but for what this port leaves out', async () => {
    // The texture hooks are var()s that fall back to the golden's look until a texture lands
    // (textures are off on this load): taken out, the rest is the golden's DOM.
    const mine = (await b.evaluate(`document.getElementById('root').innerHTML`))
      .replace(/var\(--tex-wood, (repeating-linear-gradient\(90deg, transparent, transparent 1px, rgba\(0, 0, 0, 0\.1\) 1px, rgba\(0, 0, 0, 0\.1\) 2px\))\)/g, '$1')
      .replace(/var\(--tex-fern-bg, (rgb\(180, 14, 9\))\)/g, '$1')
      .replace(/ background-image: var\(--tex-fern\);/g, '')
      .replace(' style="background-image: var(--tex-wool);"', '');
    let theirs = await b.evaluate(`(() => { const d = document.createElement('div'); d.innerHTML = ${JSON.stringify(golden)};
      d.querySelector('[data-testid="kbd-state"]').remove();   // keyboard play: not ported
      d.querySelector('svg marker#arrowhead').closest('svg').remove();   // the cover arrows' layer: not ported
      return d.innerHTML; })()`);
    // jsdom could not parse the action area's bottom (max() and env()); the seat ring's
    // 49.99999999999999% is JS printing -sin(pi) * 35 + 50, which C prints to four places.
    theirs = theirs.replace('position: absolute; left: 0px; right: 0px; justify-content: end;',
      'position: absolute; bottom: max(10px, env(safe-area-inset-bottom)); left: 0px; right: 0px; justify-content: end;')
      .replace('left: 49.99999999999999%', 'left: 50%')
      .replace('class="game-container">', 'class="game-container" data-turn="bot">');
    theirs = theirs.trim();
    if (mine !== theirs) {
      const a = theirs.split('><'), m = mine.split('><');
      const i = a.findIndex((x, k) => x !== m[k]);
      assert.fail(`first difference at tag ${i}:\n  react: <${a[i]}>\n  cdom:  <${m[i]}>`);
    }
  });

  await check('first paint: the seeded deal, read back', async () => {
    assert.deepEqual(await all('[data-hand-plane] [data-card]', 'e.dataset.card'), ['0-5', '1-6', '3-11', '3-13']);
    assert.deepEqual(await all('[data-table-container] > [data-location="table"]', 'e.querySelectorAll("[data-card]").length'), [2, 1]);
    assert.equal(await b.evaluate(`document.querySelector('[data-location="deck"] p').textContent`), '21');
    assert.equal(await b.evaluate(`document.querySelector('[data-location="deck"] > div').children.length`), 20);
    assert.deepEqual(await all('[data-role-mark]', 'e.dataset.roleMark'), ['leadSword', 'shield']);
    assert.deepEqual(await pills(), []);
  });

  await check('a resize lays the hand out again for the new width (the kernel\'s hand_layout)', async () => {
    const lefts = () => all('[data-hand-plane] [data-card]', 'e.style.left');
    assert.deepEqual(await lefts(), ['399px', '451px', '503px', '555px']);   // 1024 wide: the golden's
    await b.send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: false });
    try {
      await b.until(`document.querySelector('[data-hand-plane] [data-card]').style.left === '82px'`, 2000);
      assert.deepEqual(await lefts(), ['82px', '134px', '186px', '238px']);    // 370 of plane: (370 - 206) / 2
    } finally {   // the checks after this one play at the golden's width either way
      await b.send('Emulation.setDeviceMetricsOverride', { width: 1024, height: 768, deviceScaleFactor: 1, mobile: false });
    }
    await b.until(`document.querySelector('[data-hand-plane] [data-card]').style.left === '399px'`);
  });

  await check('a click selects a card and the kernel offers its pill; a second click takes it back', async () => {
    await b.click('[data-hand-plane] [data-card="1-6"]');   // 7h: nothing on the table to throw it on
    assert.deepEqual(await pills(), []);
    assert.match(await q('[data-hand-plane] [data-card="1-6"]'), /border: 2px solid red/);
    await b.click('[data-hand-plane] [data-card="1-6"]');
    assert.match(await q('[data-hand-plane] [data-card="1-6"]'), /border: 2px solid black/);
    await b.click('[data-hand-plane] [data-card="0-5"]');   // 6s: a six is on the table
    assert.deepEqual(await pills(), ['Attack']);
  });

  await check('the move plays, the bot answers, and the DOM is patched, not rebuilt', async () => {
    // Every node that should survive the move is tagged; a rebuild would make new ones.
    await b.evaluate(`document.querySelectorAll('[data-hand-plane] [data-card], [data-table-container] > *, [data-role-seat]')
      .forEach((e) => { e.$kept = 1; })`);
    await b.click('[data-pill="Attack"]');
    assert.deepEqual(await all('[data-table-container] [data-battle-index]', 'e.dataset.card'), ['1-5', '3-5', '0-5']);
    assert.deepEqual(await all('[data-hand-plane] [data-card]', 'e.style.borderColor'), ['black', 'black', 'black'], 'Attack spent the selection');
    assert.deepEqual(await all('[data-hand-plane] [data-card]', 'e.dataset.card'), ['1-6', '3-11', '3-13']);
    assert.equal(await turn(), 'bot');
    assert.equal(await b.evaluate(`[...document.querySelectorAll('[data-hand-plane] [data-card]')].every((e) => e.$kept)`), true,
      'the three cards left in my hand are the nodes that were there');
    assert.equal(await b.evaluate(`[...document.querySelectorAll('[data-table-container] > [data-location="table"]')].filter((e) => e.$kept).length`), 2,
      'the two battles that were down are the same nodes');
    await releaseBot();
    await settle();
    // The handwritten bot faces 6h/8h, 6d and 6s: it covers both open sixes, or it takes all five cards.
    const table = await all('[data-table-container] > [data-location="table"]', 'e.querySelectorAll("[data-card]").length');
    const anna = await b.evaluate(`document.querySelector('[data-player-id="u-anna-0001"]').lastElementChild.textContent`);
    assert.ok((table.length === 3 && table.every((n) => n === 2)) || (table.length === 0 && anna === '8'),
      `a legal answer: table ${JSON.stringify(table)}, Anna holds ${anna}`);
    assert.equal(await turn(), 'human');
    // Anna's mini hand: the cards, then the count as the last child, whatever was added before it.
    assert.deepEqual(await b.evaluate(`(() => { const h = document.querySelector('[data-player-id="u-anna-0001"]');
      return [h.lastElementChild.textContent, String(h.children.length - 1)]; })()`).then(([t, n]) => t === n), true,
      'the count badge is the mini hand\'s last child');
    assert.equal(await b.evaluate(`[...document.querySelectorAll('[data-role-seat]')].every((e) => e.$kept)`), true,
      'the seat badges are patched in place');
  });

  await check('the textures land after the first paint, through the var()s, and the backs turn black under the fern', async () => {
    // Cold: the store emptied first (the delete waits for this page's connection to close).
    await b.evaluate(`indexedDB.deleteDatabase('cdom'), 1`);
    await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html');
    await b.until(`document.querySelector('[data-game-container]')?.dataset.turn === 'human'`);
    const vars = () => b.evaluate(`['--tex-fern', '--tex-wood', '--tex-wool', '--tex-wood-end', '--tex-fern-bg'].map((v) => document.getElementById('root').style.getPropertyValue(v).slice(0, 9))`);
    assert.deepEqual(await vars(), ['', '', '', '', ''], 'nothing has landed by the first paint');
    await b.until(`window.app.then((x) => x.app_tex_ms(x.app_tex_count()) === x.app_tex_count())`, 60000);
    await b.until(`document.getElementById('root').style.getPropertyValue('--tex-wool') !== ''`, 10000);
    assert.deepEqual(await vars(), ['url(blob:', 'url(blob:', 'url(blob:', 'url(blob:', '#000000'], 'every texture set on the mount');
    const look = await b.evaluate(`(() => { const cs = (e) => getComputedStyle(e);
      return [cs(document.querySelector('[data-location="deck"] > div > div')).backgroundImage.slice(0, 9),
        cs(document.querySelector('[data-location="deck"] > div > div')).backgroundColor,
        cs(document.querySelector('.btn-icon')).backgroundImage.slice(0, 9), cs(document.querySelector('.bg-wool')).backgroundImage.slice(0, 9)]; })()`);
    assert.deepEqual(look, ['url("blob', 'rgb(0, 0, 0)', 'url("blob', 'url("blob']);
    assert.deepEqual(await b.evaluate(`window.app.then((x) => [0, 1, 2, 3].map((k) => [x.app_tex_stored(k), x.app_tex_ms(k) > 0]))`),
      [[0, true], [0, true], [0, true], [0, true]], 'made here, on a cold load');
    // The wool is woven only in its middle but laid out as the app's: a 3840 x 2160 image.
    assert.deepEqual(await b.evaluate(`(async () => { const i = new Image();
      i.src = document.getElementById('root').style.getPropertyValue('--tex-wool').slice(4, -1); await i.decode(); return [i.naturalWidth, i.naturalHeight]; })()`),
      [3840, 2160], 'the wool image is the app\'s size');
    // Warm: the next load sets all three from the store and makes none.
    await new Promise((r) => setTimeout(r, 300)); // the puts are fire-and-forget
    await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html');
    await b.until(`window.app.then((x) => x.app_tex_ms(x.app_tex_count()) === x.app_tex_count())`, 10000);
    assert.deepEqual(await b.evaluate(`window.app.then((x) => [0, 1, 2, 3].map((k) => [x.app_tex_stored(k), x.app_tex_ms(k)]))`),
      [[1, 0], [1, 0], [1, 0], [1, 0]], 'every texture from the store, no generator run');
    assert.deepEqual(await vars(), ['url(blob:', 'url(blob:', 'url(blob:', 'url(blob:', '#000000'], 'every texture set on the mount');
  });

  await check('the bot waits as the kernel says (bot_wait_ms: 3000 ms after the last move shown), and a move of mine in between puts its step back', async () => {
    await b.evaluate(`indexedDB.deleteDatabase('cdom'), 1`);
    await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug');
    await ready();
    // The golden's board: the bot defends against 6d, and I may throw in my 6s meanwhile.
    assert.equal(await turn(), 'bot');
    assert.equal(await b.evaluate(`window.app.then((x) => x.app_bot_wait())`), 3000, 'the kernel\'s wait with a human in the game');
    await new Promise((r) => setTimeout(r, 1000));
    await b.click('[data-hand-plane] [data-card="0-5"]');
    await b.click('[data-pill="Attack"]');
    const t1 = await b.evaluate('performance.now()');
    const SHOWN = `[...document.querySelectorAll('[data-table-container] [data-card]')].length + ':' + document.querySelector('[data-player-id="u-anna-0001"]').lastElementChild.textContent`;
    const before = await b.evaluate(SHOWN);
    assert.equal(before, '4:4', 'my throw-in is on the table, the bot has not moved');
    await b.until(`(${SHOWN}) !== ${JSON.stringify(before)}`, 8000);
    const t2 = await b.evaluate('performance.now()');
    console.log(`     (the bot moved ${Math.round(t2 - t1)} ms after my throw-in, ${Math.round(t2)} ms after the load)`);
    assert.ok(t2 - t1 >= 2900 && t2 - t1 < 4500, `the bot's step 3000 ms after my move, not after the load: ${Math.round(t2 - t1)} ms`);
  });

  await check('the table is the kernel\'s game and keeps React\'s rules at every click and bot step of played games (tools/consist.mjs)', async () => {
    await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html?debug');
    await ready();
    const r = await consistRun(b, { games: 3, seed: 1 });
    console.log(`     (${r.games} games, ${r.steps} steps; ${r.presses} pill presses, ${r.stillOffered} with the pill still offered after its move)`);
    const lines = [...r.kinds].map(([kind, { n, first }]) => `${kind} x${n}: at step ${first.step} after ${first.after} (before: ${first.recent.join(', ')}): ${first.detail}`);
    assert.deepEqual(lines, [], 'no disagreement');
    assert.equal(r.games, 3, 'three games played out');
  });

  await check('a reload lands in the same game, which plays on to its end; after Play again a reload lands in the new one', async () => {
    const page = 'http://127.0.0.1:8720/c/cdom_poc/out/index.html?debug';
    const board = async () => { const d = await b.evaluate(READ_DOM); d.hand = d.hand.map((h) => [h.card, h.left, h.bottom]); return JSON.stringify(d); };
    const reload = async () => { await new Promise((r) => setTimeout(r, 200)); await b.goto(page); await ready(); await hold(); };
    await b.evaluate(`indexedDB.deleteDatabase('cdom'), 1`);
    await reload();
    assert.equal(await b.evaluate(`window.app.then((x) => x.app_game_store(2))`), 0, 'nothing stored: the seeded deal');
    // three moves of mine and the bot's answers, then a card left selected (not kept, as in React)
    await b.click('[data-hand-plane] [data-card="0-5"]'); await b.click('[data-pill="Attack"]');
    await releaseBot();
    for (let m = 0; m < 2 && (await turn()) !== 'over'; m++) {
      const p = await pills();
      const hand = await all('[data-hand-plane] [data-card]', 'e.dataset.card');
      let moved = false;
      for (const c of hand) {
        await clickCard(b, c);
        const q = (await pills()).filter((x) => x === 'Attack' || x === 'Cover' || x === 'Pass');
        if (q.length) { await b.click(`[data-pill="${q[0]}"]`); moved = true; break; }
        await clickCard(b, c);
      }
      if (!moved && p.includes('Good')) await b.click('[data-pill="Good"]');
      else if (!moved && p.includes('Pickup')) await b.click('[data-pill="Pickup"]');
      if (await b.evaluate(`window.app.then((x) => x.app_debug_due() || document.querySelector('[data-game-container]').dataset.turn === 'bot')`)) await releaseBot();
    }
    const kept = await board();
    const card = await b.evaluate(`document.querySelector('[data-hand-plane] [data-card]')?.dataset.card`);
    if (card) await b.click(`[data-hand-plane] [data-card="${card}"]`);
    await reload();
    assert.equal(await b.evaluate(`window.app.then((x) => x.app_game_store(2))`), 1, 'the stored game adopted');
    assert.equal(await board(), kept, 'the DOM after the reload is the DOM before it, the selection aside');
    await b.evaluate(`window.app.then((x) => { x.app_debug_hold(0); x.app_debug_fast(1); })`);
    await playToEnd(b);
    assert.equal(await turn(), 'over');
    const end = await board();
    await reload();
    assert.equal(await board(), end, 'a finished game reloads to its end screen');
    await b.click('[data-end] button');
    await b.until(`!document.querySelector('[data-end]')`);
    const fresh = await board();
    assert.equal(await all('[data-hand-plane] [data-card]', '1').then((x) => x.length), 6, 'a new deal');
    await reload();
    assert.equal(await board(), fresh, 'the new game after a reload');
  });

  // Every surface the wood is laid on (the back button, each pill as it comes, the end screen) shows
  // only tile pixels inside a rectangle the C worked the wood out in; elsewhere the tile is the
  // base colour. Checked after every click of a game played out, games played until every pill showed.
  const pillsSeen = new Set();
  const woodCovered = async () => {
    const bad = await b.evaluate(`window.app.then((x) => {
      // the table's rectangles for --tex-wood, the end screen's for --tex-wood-end
      const R = [[], []]; for (let k = 0; k < x.app_wood_rects(); k++) R[+(k >= x.app_wood_rects_now())].push([0, 1, 2, 3].map((f) => x.app_wood_rect(k, f)));
      const out = [], seen = [];
      for (const e of document.querySelectorAll('[style]')) { const cs = getComputedStyle(e);
        if (cs.backgroundSize !== '1920px 1080px' || e.getClientRects().length === 0) continue;
        if (e.dataset.pill) seen.push(e.dataset.pill);
        const mine = R[+e.style.backgroundImage.includes('--tex-wood-end')];
        const inR = (tx, ty) => mine.some(([rx, ry, rw, rh]) => tx >= rx && tx < rx + rw && ty >= ry && ty < ry + rh);
        const [px, py] = cs.backgroundPosition.split(' ').map(parseFloat), r = e.getBoundingClientRect();
        const bl = parseFloat(cs.borderLeftWidth), bt = parseFloat(cs.borderTopWidth);
        const x0 = Math.floor(-bl - px), y0 = Math.floor(-bt - py), x1 = Math.ceil(-bl - px + r.width), y1 = Math.ceil(-bt - py + r.height);
        let miss = 0;
        for (let y = y0; y < y1; y++) for (let xx = x0; xx < x1; xx++) if (!inR(((xx % 1920) + 1920) % 1920, ((y % 1080) + 1080) % 1080)) miss++;
        if (miss) out.push((e.dataset.pill || e.className) + ' ' + miss + ' pixels outside');
      }
      return [out, seen]; })`);
    for (const p of bad[1]) pillsSeen.add(p);
    assert.deepEqual(bad[0], [], 'every wood surface inside the rectangles');
  };

  await check('a game played out by clicks reaches the end screen, and Play again deals a new one', async () => {
    await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html?debug');
    await ready();
    await fast();
    const moves = await playToEnd(b, 400, woodCovered);
    await woodCovered();
    console.log(`     (${moves} moves of mine to the end)`);
    assert.equal(await turn(), 'over');
    assert.equal(await b.evaluate(`document.querySelector('[data-end] h1').textContent`), 'done - Two of us');
    await b.click('[data-end] button');
    await settle();
    assert.equal(await b.evaluate(`document.querySelector('[data-end]')`), null);
    assert.equal(await all('[data-hand-plane] [data-card]', '1').then((x) => x.length), 6, 'a fresh hand of six');
  });

  await check('every wood surface lies inside a rectangle the C made the wood in, at every step, until every pill has shown', async () => {
    for (let games = 0; pillsSeen.size < 5 && games < 8; games++) {
      await playToEnd(b, 400, woodCovered);
      await woodCovered();
      await b.click('[data-end] button');
      await settle();
    }
    console.log(`     (pills seen: ${[...pillsSeen].sort().join(' ')})`);
    assert.equal(pillsSeen.size, 5, 'every pill showed');
  });
} finally {
  await b.close();
  server.close();
}
if (failed) { console.log(`${failed} failed`); process.exit(1); }
console.log('all passed');
