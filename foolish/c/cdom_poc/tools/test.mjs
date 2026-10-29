// The cdom table in headless Chrome: the first paint is the React golden's DOM for the same
// board, a click sequence plays a legal move and the bot answers through keyed patches, and a
// game played out by clicks reaches its end screen. Exits non-zero on the first failure.
//
//   node tools/test.mjs        (after ./build.sh)
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { launch, serve, FOOLISH, openTable } from './cdp.mjs';
import { playToEnd } from './play.mjs';
import { consistRun, READ_DOM, clickCard, kernel } from './consist.mjs';
import { drag, key, centre as restingCentre } from './input.mjs';

const golden = fs.readFileSync(path.join(FOOLISH, 'e2e/fixtures/ui_dom/bout_2p_attacker.html'), 'utf8');
const server = await serve(8720, FOOLISH);
const b = await launch();
const q = (sel) => b.evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? e.outerHTML : null; })()`);
const all = (sel, expr) => b.evaluate(`[...document.querySelectorAll(${JSON.stringify(sel)})].map((e) => ${expr})`);
const turn = () => b.evaluate(`document.querySelector('[data-game-container]').dataset.turn`);
const pills = () => all('[data-pill]', 'e.dataset.pill');
// The animations have landed (the run and any turning mark): the board is the game's.
const idle = () => b.until('window.app.then((x) => !x.app_anim_busy())', 30000);
const settle = async () => { await b.until(`document.querySelector('[data-game-container]').dataset.turn !== 'bot'`, 15000); await idle(); };
// The test build (?debug): the bot's steps held for the test to release one by one, or its waits
// made 30 ms (app_debug_fast) for the games played out.
const hold = () => b.evaluate(`window.app.then((x) => x.app_debug_hold(1))`);
// The run's clock 8 times as fast for games played out (the kernel's plan untouched).
const speed = () => b.evaluate(`window.app.then((x) => x.app_debug_anim_speed(8))`);
const fast = () => b.evaluate(`window.app.then((x) => x.app_debug_fast(1))`);
const releaseBot = async () => {
  await b.until(`window.app.then((x) => x.app_debug_due())`, 10000);
  const n = await b.evaluate(`window.app.then((x) => { const n = x.app_bot_steps(); x.app_debug_release(); return n; })`);
  await b.until(`window.app.then((x) => x.app_bot_steps() > ${n})`);
  await idle();
};
const ready = async () => { await b.until(`window.appReady`); await b.until(`window.app.then((x) => x.app_game_store(1) > 0)`); };
let failed = 0;
// ONLY=<regex>: run the checks whose name matches (tools/mutate.py, for a mutant's own check); the
// checks that set up the first page are cheap and run either way when they come first.
const ONLY = process.env.ONLY ? new RegExp(process.env.ONLY) : null;
async function check(name, fn) {
  if (ONLY && !ONLY.test(name)) return;
  try { await fn(); console.log(`ok   ${name}`); } catch (e) { failed++; console.log(`FAIL ${name}\n     ${e.message.split('\n').join('\n     ')}`); }
}

try {
  await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug');
  await ready(); // the board is drawn when the store answers (nothing kept: the golden's)
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
    await idle();
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
    await b.until('window.appReady');
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

  await check('the bot waits as the kernel says (bot_wait_ms: 3000 ms after the last move shown has finished playing), and a move of mine in between puts its step back', async () => {
    await b.evaluate(`indexedDB.deleteDatabase('cdom'), 1`);
    await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug');
    await ready();
    // The golden's board: the bot defends against 6d, and I may throw in my 6s meanwhile.
    assert.equal(await turn(), 'bot');
    assert.equal(await b.evaluate(`window.app.then((x) => x.app_bot_wait())`), 3000, 'the kernel\'s wait with a human in the game');
    await new Promise((r) => setTimeout(r, 1000));
    await b.click('[data-hand-plane] [data-card="0-5"]');
    const n0 = await b.evaluate('window.app.then((x) => x.app_bot_steps())');
    await b.click('[data-pill="Attack"]');
    const t1 = await b.evaluate('performance.now()');
    // my throw-in's run: one flight of the kernel's 500 ms; the bot's wait counts from its landing
    const run = await b.evaluate('window.app.then((x) => [x.app_anim_steps(), x.app_anim_step(0, 2)])');
    assert.deepEqual(run, [1, 500], 'my throw-in is one step of 500 ms');
    await b.until(`window.app.then((x) => x.app_bot_steps() > ${n0})`, 8000);
    const t2 = await b.evaluate('performance.now()');
    console.log(`     (the bot stepped ${Math.round(t2 - t1)} ms after my throw-in, ${Math.round(t2)} ms after the load)`);
    assert.ok(t2 - t1 >= 3450 && t2 - t1 < 4500, `the bot's step 3000 ms after my throw-in has landed (500 + 3000), not after the click or the load: ${Math.round(t2 - t1)} ms`);
  });

  // ---- the animations (README, Animations) -----------------------------------------------------
  // A recorder in the page: every flight node as the overlay gets it (its card, its start), when it
  // was armed and with what (its landing, transition, shadow), and when it went; and every width a
  // role coin was drawn at. Read back as plain values.
  const RECORD = `window.app.then((x) => { const rec = window.$rec = { flights: [], coins: [], ghosts: [], marks: [] };
    const root = document.getElementById('root');
    const isOverlay = (n) => n.nodeType === 1 && n.style && n.style.zIndex === '10000';
    const armed = new MutationObserver((ms) => { for (const m of ms) { const f = rec.flights.find((q) => q.node === m.target);
      if (f && !f.armedAt && m.target.style.transition !== 'none') { f.armedAt = performance.now();
        f.end = { x: parseFloat(m.target.style.left), y: parseFloat(m.target.style.top), transform: m.target.style.transform,
          transition: m.target.style.transition, shadow: m.target.firstChild.style.boxShadow }; } } });
    new MutationObserver((ms) => { const t = performance.now(); for (const m of ms) {
      for (const n of m.addedNodes) if (isOverlay(n)) [...n.children].forEach((c, i) => { const card = x.app_anim_flight_card(i);
        const slot = document.querySelector('[data-location="flipped"]')?.getBoundingClientRect();
        rec.flights.push({ node: c, at: t, card, layers: card < 0 ? c.firstChild.children.length : 0, slot: slot ? [slot.left + slot.width / 2, slot.top + slot.height / 2] : null,
          start: { x: parseFloat(c.style.left), y: parseFloat(c.style.top), transform: c.style.transform } });
        armed.observe(c, { attributes: true, attributeFilter: ['style'] }); });
      for (const n of m.removedNodes) if (isOverlay(n)) for (const f of rec.flights) if (!f.goneAt && n.contains(f.node)) f.goneAt = t;
      // the role ghosts (z 10001): their kind and where each starts, and when they go
      for (const n of m.addedNodes) if (n.nodeType === 1 && n.style && n.style.zIndex === '10001')
        for (const g of n.querySelectorAll('[data-role-flight]')) rec.ghosts.push({ kind: g.dataset.roleFlight, at: t, start: [parseFloat(g.style.left), parseFloat(g.style.top)], landed: x.app_anim_landed() });
      for (const n of m.removedNodes) if (n.nodeType === 1 && n.style && n.style.zIndex === '10001') for (const g of rec.ghosts) if (!g.goneAt) { g.goneAt = t; g.goneLanded = x.app_anim_landed(); g.goneBusyRun = x.app_anim_steps(); } } })
      .observe(root, { childList: true, subtree: true });
    new MutationObserver((ms) => { for (const m of ms) { const e = m.target; if (e.dataset.roleSeat === undefined) continue;
      if (m.attributeName === 'data-role-mark') rec.marks.push({ t: performance.now(), seat: +e.dataset.roleSeat, mark: e.dataset.roleMark, landed: x.app_anim_landed(), steps: x.app_anim_steps() });
      const w = /scaleX\\(([^)]*)\\)/.exec(e.style.transform); if (w) rec.coins.push([+e.dataset.roleSeat, +w[1], e.dataset.roleMark]); } })
      .observe(root, { attributes: true, subtree: true, attributeFilter: ['style', 'data-role-mark'] });
    return 1; })`;
  const flights = () => b.evaluate(`window.$rec.flights.map(({ node, ...f }) => f)`);
  const pad = (seat) => centre(`[data-role-seat="${seat}"]`);
  const centre = (sel) => b.evaluate(`(() => { const r = document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
  const near = (a, b2, tol, what) => assert.ok(Math.abs(a[0] - b2[0]) <= tol && Math.abs(a[1] - b2[1]) <= tol, `${what}: ${a.map(Math.round)} against ${b2.map(Math.round)}`);
  const id = (card) => { const [suit, value] = card.split('-').map(Number); return suit * 13 + value - 1; };
  const TRANSITION = 'left 500ms cubic-bezier(0.25, 0.46, 0.45, 0.94), top 500ms cubic-bezier(0.25, 0.46, 0.45, 0.94), transform 500ms cubic-bezier(0.25, 0.46, 0.45, 0.94)';
  const vis = (sel) => b.evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? getComputedStyle(e).visibility : 'none'; })()`);
  const seatCount = (seat) => b.evaluate(`document.querySelector('[data-role-seat="${seat}"]').parentElement.lastElementChild.lastElementChild?.textContent ?? null`);
  const ANIM_PAGE = 'http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug';

  await check('a played card flies from its place in my hand to the cell the row made for it, hidden at both ends, and the board commits at the landing', async () => {
    await openTable(b, ANIM_PAGE, { hold: true });
    await b.evaluate(RECORD);
    const from = await centre('[data-hand-plane] [data-card="0-5"]');
    await b.click('[data-hand-plane] [data-card="0-5"]');
    await b.click('[data-pill="Attack"]');
    // mid-flight: the card shows only in its flight; the cell is there (the row made room) and empty;
    // the row glides over the flight's own 500 ms; my count is the board before the landing
    const mid = await b.evaluate(`[getComputedStyle(document.querySelector('[data-hand-plane] [data-card="0-5"]')).visibility,
      getComputedStyle(document.querySelector('[data-table-container] [data-card="0-5"]')).visibility,
      [...document.querySelectorAll('[data-table-container] > div')].map((c) => c.style.transition).filter((t) => t.startsWith('transform 500ms')).length,
      document.querySelector('[data-role-seat="0"]').parentElement.lastElementChild.lastElementChild.textContent,
      window.$rec.flights.length]`);
    assert.deepEqual(mid, ['hidden', 'hidden', 2, '4', 1], 'mid-flight: hand card hidden, cell hidden, the two cells already down gliding, my count 4, one flight');
    await idle();
    const to = await centre('[data-table-container] [data-card="0-5"]');
    const [f] = await flights();
    assert.equal(f.card, id('0-5'));
    assert.ok(f.end, 'armed: given its landing');
    near([f.start.x, f.start.y], from, 0.5, 'it leaves from the hand card\'s centre');
    near([f.end.x, f.end.y], to, 0.5, 'it lands on the cell\'s centre');
    assert.equal(f.start.transform, 'translate(-50%, -50%) translate(0px, 17.5px) scale(1.5) rotate(0rad)');
    assert.equal(f.end.transform, 'translate(-50%, -50%) translate(0px, 28px) scale(1.8) rotate(0rad)');
    assert.equal(f.end.transition, TRANSITION);
    assert.equal(f.end.shadow, 'rgba(0, 0, 0, 0.4) 0px 10px 20px');
    console.log(`     (armed ${Math.round(f.armedAt - f.at)} ms after it was drawn, gone ${Math.round(f.goneAt - f.at)} ms after)`);
    assert.ok(f.armedAt - f.at >= 15 && f.armedAt - f.at < 150, `armed one beat (25 ms) after: ${f.armedAt - f.at}`);
    assert.ok(f.goneAt - f.at >= 480 && f.goneAt - f.at < 800, `gone at its landing (500 ms): ${f.goneAt - f.at}`);
    assert.deepEqual([await vis('[data-table-container] [data-card="0-5"]'), await vis('[data-hand-plane] [data-card="0-5"]'), await seatCount(0)],
      ['visible', 'none', '3'], 'after the landing: on the table, out of my hand, my count 3');
  });

  await check('the bot\'s cover flies from its seat and lands laid across its attack, which turns with it; a take flies to the seat; the refill from the stock into my hand as it makes room', async () => {
    await openTable(b, ANIM_PAGE, { hold: true });
    await b.evaluate(RECORD);
    const seat = await centre('[data-player-id="u-anna-0001"]');
    await b.until(`window.app.then((x) => x.app_debug_due())`, 10000);
    await b.evaluate(`window.app.then((x) => x.app_debug_release())`);
    await b.until(`window.$rec.flights.length > 0`);
    const turning = await b.evaluate(`(() => { const a = document.querySelector('[data-table-container] [data-card="3-5"]'); return [a.style.transform, a.style.transition]; })()`);
    assert.deepEqual(turning, ['translateX(-50%) rotate(-0.19635rad)', 'transform 500ms cubic-bezier(0.25, 0.46, 0.45, 0.94) 25ms, box-shadow 0.2s ease-in-out'],
      'the attack under the cover turns over the flight, on its curve, after its arm');
    await idle();
    let [f] = await flights();
    assert.equal(f.card, id('2-5'), 'the bot covers 6d with the trump 6c');
    near([f.start.x, f.start.y], seat, 0.5, 'from the bot\'s mini hand');
    assert.match(f.end.transform, /scale\(1\.8\) rotate\(0\.1963\d*rad\)$/, 'it lands tilted at the cover angle');
    const d = await centre('[data-table-container] [data-card="2-5"]');
    near([f.end.x, f.end.y], d, 0.5, 'it lands where the cover is drawn');
    assert.equal(await b.evaluate(`document.querySelector('[data-table-container] [data-card="3-5"]').style.transition`),
      'transform cubic-bezier(0.25, 0.46, 0.45, 0.94), box-shadow 0.2s ease-in-out', 'the turn\'s transition back at rest (0 ms, as Chrome writes it back)');
    // my throw-in, then the bot takes all five: they fly from where they lie to its seat, the covered
    // pair still at their tilts
    await b.click('[data-hand-plane] [data-card="0-5"]');
    await b.click('[data-pill="Attack"]');
    await idle();
    const lie = await b.evaluate(`Object.fromEntries([...document.querySelectorAll('[data-table-container] [data-card]')].map((e) => { const r = e.getBoundingClientRect(); return [e.dataset.card, [r.left + r.width / 2, r.top + r.height / 2]]; }))`);
    const handBefore = await all('[data-hand-plane] [data-card]', 'e.style.left');
    const n0 = (await flights()).length;
    await b.until(`window.app.then((x) => x.app_debug_due())`, 10000);
    await b.evaluate(`window.app.then((x) => x.app_debug_release())`);
    // the refill's flight: my hand has made room for it (its cards slid), the stock counts down as they leave
    const taken = JSON.stringify(Object.keys(lie).map(id));
    await b.until(`window.app.then((x) => x.app_anim_flying() > 0 && !${taken}.includes(x.app_anim_flight_card(0)))`, 8000);
    const handMid = await all('[data-hand-plane] [data-card]', 'e.style.left');
    const badgeMid = await b.evaluate(`document.querySelector('[data-location="deck"] p').textContent`);
    await idle();
    const fs2 = (await flights()).slice(n0);
    const take = fs2.filter((x) => x.end && Math.abs(x.end.y - seat[1]) < 20);
    assert.deepEqual(take.map((x) => x.card).sort((p, q) => p - q), ['1-5', '1-7', '3-5', '2-5', '0-5'].map(id).sort((p, q) => p - q), 'the take: the five table cards to the bot\'s seat');
    for (const x of take) {
      const card = Object.keys(lie).find((c) => id(c) === x.card);
      near([x.start.x, x.start.y], lie[card], 0.5, `${card} leaves from where it lay`);
    }
    const tilt = (c) => /rotate\(([^)]*)rad\)/.exec(take.find((x) => x.card === id(c)).start.transform)[1];
    assert.deepEqual(['1-5', '1-7', '3-5', '2-5', '0-5'].map(tilt).map(Number).map((v) => Math.round(v * 1e4) / 1e4), [-0.1963, 0.1963, -0.1963, 0.1963, 0], 'each lifts off at the tilt it lay at');
    const refill = fs2.filter((x) => !take.includes(x));
    assert.equal(refill.length, 3, 'my refill: three cards, the bot\'s none (it holds eight)');
    const deck = await centre('[data-location="deck"] > div');
    near([refill[0].start.x, refill[0].start.y], deck, 1, 'from the stock');
    assert.notDeepEqual(handMid, handBefore, 'my hand made room while the cards were in the air');
    assert.equal(badgeMid, String(Math.max(0, 21 - 3)), 'the stock counts down as its cards leave');
    const mine = await b.evaluate(`Object.fromEntries([...document.querySelectorAll('[data-hand-plane] [data-card]')].map((e) => { const r = e.getBoundingClientRect(); return [e.dataset.card, [r.left + r.width / 2, r.top + r.height / 2]]; }))`);
    for (const x of refill) {
      const card = Object.keys(mine).find((c) => id(c) === x.card);
      near([x.end.x, x.end.y], mine[card], 0.5, `${card} lands on its slot in my hand`);
    }
  });
  await check('a sweep flies the table to the discard pile, whose count shows at its landing while the refills still fly; the bot\'s draw is one stack of backs; the marks turn over as coins', async () => {
    await openTable(b, ANIM_PAGE, { hold: true });
    await b.evaluate(RECORD);
    await b.until(`window.app.then((x) => x.app_debug_due())`, 10000);
    await b.evaluate(`window.app.then((x) => x.app_debug_release())`);
    await b.until('window.app.then((x) => x.app_bot_steps() > 0)');
    await idle();
    const lie = await b.evaluate(`Object.fromEntries([...document.querySelectorAll('[data-table-container] [data-card]')].map((e) => { const r = e.getBoundingClientRect(); return [e.dataset.card, [r.left + r.width / 2, r.top + r.height / 2]]; }))`);
    const pile = await centre('[data-location="discard"]');
    const deck = await centre('[data-location="deck"] > div');
    const seat = await centre('[data-player-id="u-anna-0001"]');
    const n0 = (await flights()).length;
    await b.click('[data-pill="Good"]');
    // once the sweep has landed and the refills are in the air: the pile's count is the landed step's
    await b.until(`window.app.then((x) => x.app_anim_landed() >= 1 && x.app_anim_flying() > 0 && x.app_anim_flight_card(0) !== ${id('1-5')})`, 5000);
    const mid = await b.evaluate(`[document.querySelector('[data-location="discard"] p')?.textContent ?? null, window.app.then((x) => x.app_anim_busy())]`);
    assert.deepEqual([mid[0], await b.evaluate('window.app.then((x) => x.app_anim_busy())')], ['4', 1], 'the discard pile shows its four cards while the run still plays');
    // the badges mid-run: my good leads the stream (a check at once), the rest waits for the closing beat
    assert.deepEqual(await all('[data-role-mark]', 'e.dataset.roleMark'), ['check', 'shield'], 'mid-run: my check, Anna\'s shield, frozen');
    await idle();
    const fs2 = (await flights()).slice(n0);
    const sweep = fs2.filter((x) => Object.keys(lie).some((c) => id(c) === x.card) && x.end && Math.abs(x.end.y - pile[1]) < 20);
    assert.equal(sweep.length, 4, 'the four table cards to the pile');
    sweep.forEach((x, i) => {
      const card = Object.keys(lie).find((c) => id(c) === x.card);
      near([x.start.x, x.start.y], lie[card], 0.5, `${card} leaves from where it lay`);
      near([x.end.x, x.end.y], [pile[0] + 3 * i, pile[1] + 3 * i], 0.5, `${card} lands on the pile, 3 px a card apart`);
    });
    const backs = fs2.filter((x) => x.card === -1);
    assert.equal(backs.length, 1, 'the bot\'s draw is one flight');
    assert.equal(backs[0].layers, 3, 'of three backs (it had 3 cards)');
    near([backs[0].start.x, backs[0].start.y], deck, 1, 'from the stock');
    near([backs[0].end.x, backs[0].end.y], seat, 1, 'to the bot\'s seat');
    // the bout ended: the shield and the swords moved at the closing beat, each turning over where it stands
    await b.until(`window.app.then((x) => !x.app_anim_busy())`);
    const coins = await b.evaluate('window.$rec.coins');
    const narrow = coins.filter(([, w]) => w < 0.5);
    assert.ok(narrow.length >= 2 && coins.at(-1)[1] === 1, `the marks turned as coins (widths drawn: ${coins.length}, under 0.5: ${narrow.length})`);
    assert.deepEqual(await all('[data-role-mark]', 'e.dataset.roleMark'), ['shield', 'leadSword'], 'the bout over: Anna leads, I defend');
    const ghosts = await b.evaluate('window.$rec.ghosts');
    assert.deepEqual(ghosts.map((g) => g.kind).sort(), ['shield', 'sword'], 'the closing beat throws the shield and the opener\'s sword');
    near(ghosts.find((g) => g.kind === 'shield').start, await pad(1), 0.5, 'the shield leaves Anna\'s badge');
    near(ghosts.find((g) => g.kind === 'sword').start, await pad(0), 0.5, 'the sword leaves mine');
    const fin = ghosts[0].goneAt - ghosts[0].at;
    assert.ok(fin >= 400 && fin < 900, `the ghosts fly ROLE_FLIGHT_MS (400) after their 25 ms arm: ${Math.round(fin)} ms`);
  });

  await check('a transfer hands the shield over as its card leaves (anim_pass_hand_off), not when it lands', async () => {
    await openTable(b, ANIM_PAGE, { hold: true });
    await b.evaluate('window.app.then((x) => x.app_debug_transfer())');
    await b.evaluate(RECORD);
    assert.deepEqual(await all('[data-role-mark]', 'e.dataset.roleMark'), ['leadSword', 'shield']);
    await b.until(`window.app.then((x) => x.app_debug_due())`, 10000);
    await b.evaluate(`window.app.then((x) => x.app_debug_release())`);
    const anna = await pad(1);
    await idle();
    const { ghosts, marks } = await b.evaluate('({ ghosts: window.$rec.ghosts, marks: window.$rec.marks })');
    const [cardFlight] = await flights();
    assert.equal(cardFlight.card, id('1-11'), 'the bot transfers Qh');
    assert.equal(ghosts.length, 1, 'one mark flies: the shield');
    assert.equal(ghosts[0].kind, 'shield');
    near(ghosts[0].start, anna, 0.5, 'it leaves Anna\'s badge');
    assert.ok(ghosts[0].at - cardFlight.at < 20, `it takes off with the card (${Math.round(ghosts[0].at - cardFlight.at)} ms after it)`);
    assert.equal(ghosts[0].goneLanded, 0, 'it lands while the transferred card is still in the air');
    const mine = marks.filter((m) => m.seat === 0);
    assert.deepEqual(mine.at(-1) && [mine.at(-1).mark, mine.at(-1).landed], ['shield', 0], 'my badge shows the shield as the ghost lands, before the card does');
    const hers = marks.filter((m) => m.seat === 1).map((m) => m.mark);
    assert.deepEqual(hers, ['', 'sword'], 'Anna\'s badge blanks as the shield leaves it, then turns a sword in (the hand-off moves the shield alone)');
    const turned = await b.evaluate('window.$rec.coins.filter(([seat, w]) => seat === 0 && w < 0.5).length');
    assert.ok(turned > 0, 'my badge turns its sword away to be edge-on as the shield lands (the make-way)');
    await idle();
    assert.deepEqual(await all('[data-table-container] [data-battle-index]', 'e.dataset.card'), ['3-11', '1-11']);
  });

  await check('a deal is one flight per card, 350 ms each as the kernel paces it, mine face up and the bot\'s a back, then the trump turned out of the stock', async () => {
    await openTable(b, ANIM_PAGE, { hold: true });
    await b.evaluate(RECORD);
    await b.evaluate('window.app.then((x) => x.app_debug_deal())');
    const plan = await b.evaluate('window.app.then((x) => { const o = []; for (let k = 0; k < x.app_anim_steps(); k++) o.push([x.app_anim_step(k, 0), x.app_anim_step(k, 1), x.app_anim_step(k, 2)]); return o; })');
    const deals = plan.filter(([t]) => t === 1);
    assert.equal(deals.length, 12);
    assert.deepEqual(deals.map(([, start, dur]) => [start, dur]), deals.map((_, k) => [525 + 375 * k, 350]), 'the kernel\'s plan: 350 ms a card, 25 ms apart, after the 500 ms opening');
    await idle();
    const fs2 = await flights();
    assert.equal(fs2.length, 13, 'twelve cards and the trump');
    const dur = (x) => /left (\d+)ms/.exec(x.end.transition)[1];
    assert.deepEqual(fs2.slice(0, 12).map(dur), Array(12).fill('350'), 'each dealt card flies 350 ms');
    assert.equal(dur(fs2[12]), '500', 'the trump turns in 500');
    assert.deepEqual(fs2.slice(0, 12).map((x) => x.card >= 0 ? 'face' : 'back'), Array(6).fill(['face', 'back']).flat(), 'mine face up, the bot\'s a back, a card at a time round the table');
    const hand = await all('[data-hand-plane] [data-card]', 'e.dataset.card');
    assert.deepEqual(fs2.slice(0, 12).filter((x) => x.card >= 0).map((x) => x.card), hand.map(id), 'my six, in the order the hand holds them');
    const slot = await centre('[data-location="flipped"]');
    near([fs2[12].end.x, fs2[12].end.y], slot, 0.5, 'the trump lands in its slot');
  });

  await check('a move made while a run plays joins it: one run, its steps after the ones already there (start_ms from the kernel)', async () => {
    await openTable(b, ANIM_PAGE, { hold: true });
    await b.evaluate(RECORD);
    const runs0 = await b.evaluate('window.app.then((x) => x.app_anim_runs())');
    await b.until(`window.app.then((x) => x.app_debug_due())`, 10000);
    await b.evaluate(`window.app.then((x) => x.app_debug_release())`);
    await b.until(`window.$rec.flights.length > 0`); // the cover is in the air
    await b.click('[data-hand-plane] [data-card="0-5"]');
    await b.click('[data-pill="Attack"]');
    const plan = await b.evaluate('window.app.then((x) => [x.app_anim_runs(), x.app_anim_steps(), x.app_anim_step(0, 0), x.app_anim_step(1, 0), x.app_anim_step(1, 1)])');
    assert.deepEqual(plan, [runs0 + 1, 2, 5, 4, 525], 'one run: the cover (0 ms), then my attack at 500 + 25');
    await idle();
    const [cover, mine] = await flights();
    assert.ok(mine.at >= cover.goneAt - 1, `my card took off once the cover had landed (${Math.round(mine.at - cover.at)} ms after it)`);
    assert.deepEqual(await all('[data-table-container] [data-battle-index]', 'e.dataset.card'), ['1-5', '3-5', '0-5']);
  });

  await check('a reload in the middle of a flight lands on the committed board, with no flight', async () => {
    await openTable(b, ANIM_PAGE, { hold: true });
    await b.click('[data-hand-plane] [data-card="0-5"]');
    await b.click('[data-pill="Attack"]');
    assert.equal(await b.evaluate('window.app.then((x) => x.app_anim_flying())'), 1, 'in the air');
    await new Promise((r) => setTimeout(r, 200)); // the save is a put on its way
    await b.goto(ANIM_PAGE);
    await ready();
    const k = await kernel(b);
    assert.deepEqual(await b.evaluate('window.app.then((x) => [x.app_game_store(2), x.app_anim_busy(), x.app_anim_runs()])'), [1, 0, 0], 'the kept game, nothing playing');
    assert.deepEqual(await all('[data-table-container] [data-battle-index]', 'e.dataset.card'), ['1-5', '3-5', '0-5']);
    assert.deepEqual(k.battles, ['1-5/1-7', '3-5/-', '0-5/-']);
    assert.equal(await b.evaluate(`[...document.querySelectorAll('div')].some((d) => d.style.zIndex === '10000')`), false, 'no overlay');
  });

  // ---- drag and drop, and keyboard play (README, Drag and drop; Keyboard) ------------------------
  // A board of app_debug_board (k: 0 the golden's, 1 to 3 below), the bot held, textures off.
  const board = async (k) => {
    await openTable(b, 'http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug', { hold: true });
    if (k) await b.evaluate(`window.app.then((x) => x.app_debug_board(${k}))`);
    await idle();
  };
  const hand = () => all('[data-hand-plane] [data-card]', 'e.dataset.card');
  const reds = () => all('[data-hand-plane] [data-card]', 'e.style.borderColor === "red" ? e.dataset.card : ""').then((x) => x.filter(Boolean));
  const battles = () => all('[data-table-container] > [data-location="table"]', '[...e.querySelectorAll("[data-card]")].map((c) => c.dataset.card).join("/")');
  const cardAt = (c) => restingCentre(b, `[data-hand-plane] [data-card="${c}"]`);
  const battleAt = (c) => restingCentre(b, `[data-table-container] [data-battle-index][data-card="${c}"]`);
  // What shows mid-drag: the shadow's badge and cards, the drop cells, the amber cells, the dragged card's look.
  const midDrag = () => b.evaluate(`(() => { const s = document.querySelector('.drag-shadow');
    return { badge: s?.querySelector('.drag-shadow__indicator')?.textContent ?? null,
      cards: s ? [...s.querySelectorAll('.drag-shadow__cards > div')].map((e) => e.firstElementChild.textContent.replace(/\\s/g, '')) : null,
      at: s ? [s.style.left, s.style.top] : null,
      cells: [...document.querySelectorAll('[data-table-container] > [data-location="table"]')].filter((e) => e.style.borderStyle === 'dashed').map((e) => e.dataset.battleIndex),
      amber: [...document.querySelectorAll('[data-table-container] > [data-location="table"]')].filter((e) => e.style.borderStyle === 'solid' && e.style.borderColor === 'rgb(210, 144, 2)').map((e) => e.querySelector('[data-card]').dataset.card),
      look: [...document.querySelectorAll('[data-hand-plane] [data-card]')].map((e) => e.dataset.card + ':' + e.style.borderColor + ':' + e.style.opacity) }; })()`);
  const TABLE_SPOT = { x: 512, y: 300 };   // open table, away from the battles and the hand's area

  for (const touch of [false, true]) {
    const by = touch ? ' (touch)' : '';
    await check(`a card dragged onto the table attacks; the shadow names the move over the drop cells while it is held${by}`, async () => {
      await board(0);   // the golden's: 6s may be thrown in
      let seen = null;
      // let go 40 px above the hand: still the hand's area (the plane and a card's height above it), no move
      const top = await b.evaluate(`document.querySelector('[data-hand-plane]').getBoundingClientRect().top`);
      await drag(b, await cardAt('0-5'), [{ x: 700, y: top - 40 }], { touch });
      await idle();
      assert.deepEqual(await battles(), ['1-5/1-7', '3-5'], 'a drop in the hand\'s area plays nothing');
      assert.deepEqual(await hand(), ['0-5', '1-6', '3-11', '3-13']);
      await drag(b, await cardAt('0-5'), [TABLE_SPOT], { touch, atEach: async () => { seen = await midDrag(); } });
      assert.deepEqual(seen.badge, '⚔️ Attack', 'the badge names the kernel\'s move');
      assert.deepEqual(seen.at, ['512px', '260px'], 'over the pointer, 40 px up');
      assert.deepEqual(seen.cells, ['2'], 'one drop cell, past the two battles');
      assert.ok(seen.look.includes('0-5:red:1'), `the dragged card shows the selection's border: ${seen.look}`);
      await idle();
      assert.deepEqual(await battles(), ['1-5/1-7', '3-5', '0-5'], 'the attack is played');
      assert.deepEqual(await hand(), ['1-6', '3-11', '3-13']);
      const after = await midDrag();
      assert.equal(after.badge, null, 'the shadow is gone');
      assert.deepEqual(after.cells, [], 'and the drop cells');
    });

    await check(`a card dragged onto a battle it beats covers it (the kernel's pairing), the cell in amber while it is held; onto one it cannot beat, nothing${by}`, async () => {
      await board(1);   // I defend 7h and Qd with 9h Kd 6c 6s
      await b.click('[data-hand-plane] [data-card="3-12"]');   // Kd selected
      let seen = null;
      // 6s onto 7h: no move. The card comes back, the selection is untouched.
      await drag(b, await cardAt('0-5'), [await battleAt('1-6')], { touch, atEach: async () => { seen = await midDrag(); } });
      assert.equal(seen.badge, null, 'no badge for a drop that is no move');
      assert.deepEqual(seen.amber, [], 'no cell in amber');
      await idle();
      assert.deepEqual(await battles(), ['1-6', '3-11'], 'nothing played');
      assert.deepEqual(await hand(), ['1-8', '3-12', '2-5', '0-5'], 'the card is back in its place');
      assert.deepEqual(await reds(), ['3-12'], 'the selection untouched');
      // 9h onto 7h: a cover, amber while held.
      await drag(b, await cardAt('1-8'), [await battleAt('1-6')], { touch, atEach: async () => { seen = await midDrag(); } });
      assert.equal(seen.badge, '🛡️ Cover');
      assert.deepEqual(seen.amber, ['1-6'], 'the cover\'s cell in amber');
      assert.deepEqual(seen.cards, ['9\u2665'], 'the shadow carries the dragged card alone (it is not selected)');
      await idle();
      assert.deepEqual(await battles(), ['1-6/1-8', '3-11'], 'covered as the kernel paired it');
      assert.deepEqual(await reds(), [], 'a drop that is a move spends the selection');
      assert.deepEqual((await midDrag()).amber, [], 'the amber is gone');
    });

    await check(`a press released within 150 ms after a 5 px wiggle is a tap and toggles the card; a 200 ms hold selects nothing${by}`, async () => {
      await board(0);
      const at = await cardAt('1-6');
      await drag(b, at, [{ x: at.x + 3, y: at.y + 4 }], { touch, steps: 2, stepMs: 5 });
      assert.deepEqual(await reds(), ['1-6'], 'a tap selects');
      await drag(b, at, [{ x: at.x + 3, y: at.y + 4 }], { touch, steps: 2, stepMs: 5 });
      assert.deepEqual(await reds(), [], 'a second tap takes it back');
      await drag(b, at, [], { touch, holdMs: 200 });
      assert.deepEqual(await reds(), [], 'a long press selects nothing');
      // a quick tap whose release waits 250 ms behind a busy page is still a tap: the two events'
      // own times are 0 to 150 ms apart, whatever the handler's clock says (DragContext.tsx endCardDrag)
      const T = touch ? 'Input.dispatchTouchEvent' : 'Input.dispatchMouseEvent';
      const ev = (type) => touch ? { type: type ? 'touchEnd' : 'touchStart', touchPoints: type ? [] : [{ x: at.x, y: at.y, id: 1 }] }
        : { type: type ? 'mouseReleased' : 'mousePressed', x: at.x, y: at.y, button: 'left', buttons: type ? 0 : 1, clickCount: 1 };
      await b.send(T, ev(0));
      const busy = b.evaluate('(() => { const t = performance.now() + 250; while (performance.now() < t); return 1; })()');
      await b.send(T, ev(1));
      await busy;
      assert.deepEqual(await reds(), ['1-6'], 'a tap released behind a busy page selects');
      assert.equal(await b.evaluate('window.app.then((x) => x.app_drag(0))'), 0, 'and the drag is over');
    });

    await check(`a card dragged along the hand changes places with the cards it passes, dimmed meanwhile, and the game keeps the order${by}`, async () => {
      await board(0);
      let seen = null;
      const from = await cardAt('0-5'), last = await cardAt('3-13');
      await drag(b, from, [{ x: from.x, y: from.y - 30 }, { x: last.x, y: last.y - 30 }], { touch, steps: 8, atEach: async (i) => { if (i === 1) seen = await midDrag(); } });
      assert.ok(seen.look.some((l) => l === '0-5:black:0.3'), `the dragged card is dimmed: ${seen.look}`);
      assert.equal(seen.badge, null, 'no shadow within the hand');
      await idle();
      assert.deepEqual(await hand(), ['1-6', '3-11', '3-13', '0-5'], 'the six went to the end, the others moved left one by one');
      assert.deepEqual(await all('[data-hand-plane] [data-card]', 'e.dataset.cardIndex'), ['0', '1', '2', '3']);
      assert.deepEqual(await reds(), [], 'a drag is no tap');
      assert.deepEqual((await kernel(b)).hand, ['1-6', '3-11', '3-13', '0-5'], 'the game holds the new order');
      await b.until('window.app.then((x) => x.app_saves_pending() === 0)');
      await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug');
      await ready();
      assert.deepEqual(await hand(), ['1-6', '3-11', '3-13', '0-5'], 'a reload draws it');
    });
  }

  await check('a transfer by drag: the shadow says Pass over a drop cell; dragging a selected card carries the selection', async () => {
    await board(2);   // I defend 6h with 6c 6s 9h Kd
    await b.click('[data-hand-plane] [data-card="0-5"]');
    await b.click('[data-hand-plane] [data-card="2-5"]');   // 6s and 6c selected
    let seen = null;
    await drag(b, await cardAt('0-5'), [TABLE_SPOT], { atEach: async () => { seen = await midDrag(); } });
    assert.equal(seen.badge, '🔄 Pass');
    assert.deepEqual(seen.cards.length, 2, 'the shadow carries both selected cards');
    assert.deepEqual(seen.cells, ['1', '2'], 'two drop cells, one a card');
    await idle();
    assert.deepEqual(await battles(), ['1-5', '2-5', '0-5'], 'both transferred, in the kernel\'s order for the move');
  });

  await check('keys: 3 toggles the third card, 1 then A attacks with the first; letters do nothing with Ctrl, Alt or Meta held', async () => {
    await board(0);
    // typed into a field: no key is the table's
    await b.evaluate(`(() => { const i = document.createElement('input'); i.id = 'typing'; document.body.append(i); i.focus(); })()`);
    await key(b, '1');
    await key(b, 'ArrowRight');
    assert.deepEqual([await reds(), await b.evaluate(`document.querySelector('[data-testid="kbd-state"]').dataset.mode`)], [[], 'none'], 'keys typed into a field do nothing');
    await b.evaluate(`document.getElementById('typing').remove()`);
    await key(b, '3');
    assert.deepEqual(await reds(), ['3-11']);
    await key(b, '3');
    assert.deepEqual(await reds(), []);
    await key(b, '1', { meta: true });
    assert.deepEqual(await reds(), [], 'Meta+1 is the browser\'s');
    await key(b, '1');
    assert.deepEqual(await pills(), ['Attack']);
    await key(b, 'a', { ctrl: true });
    assert.deepEqual(await battles(), ['1-5/1-7', '3-5'], 'Ctrl+A plays nothing');
    await key(b, 'a');
    await idle();
    assert.deepEqual(await battles(), ['1-5/1-7', '3-5', '0-5'], 'A attacked with the six');
    assert.deepEqual(await reds(), [], 'and spent the selection');
  });

  await check('arrows: the cursor sits under its card as React draws it, Shift toggles, Meta+Right moves the card, and the hidden state says so', async () => {
    await board(0);
    const st = () => b.evaluate(`(() => { const e = document.querySelector('[data-testid="kbd-state"]'); return [e.dataset.mode, e.dataset.sel, e.dataset.target]; })()`);
    assert.deepEqual(await st(), ['none', '', '']);
    await key(b, 'ArrowRight');
    await key(b, 'ArrowLeft');
    assert.deepEqual(await st(), ['cards', '3-13', ''], 'Left from the first card wraps to the last');
    await key(b, 'ArrowRight');
    await key(b, 'ArrowRight');
    assert.deepEqual(await st(), ['cards', '1-6', '']);
    const geo = await b.evaluate(`(() => { const c = document.querySelector('[data-hand-plane] [data-card="1-6"]').getBoundingClientRect();
      const u = [...document.querySelectorAll('[data-game-container] > div')].find((e) => e.style.zIndex === '1500');
      const s = u.style; return [parseFloat(s.left) - (c.x + 0.15 * c.width), parseFloat(s.top) - (c.y + c.height + 4), parseFloat(s.width) - 0.7 * c.width, s.height, s.position, s.background]; })()`);
    for (const d of geo.slice(0, 3)) assert.ok(Math.abs(d) < 0.01, `the underline at rect.x + 0.15 w, rect.y + h + 4, 0.7 w: ${geo}`);
    assert.deepEqual(geo.slice(3), ['3px', 'fixed', 'rgb(17, 17, 17)']);
    await key(b, 'Shift');
    assert.deepEqual(await reds(), ['1-6'], 'Shift toggles the card under the cursor');
    await key(b, 'ArrowRight', { shift: true });
    assert.deepEqual(await reds(), ['1-6', '3-11'], 'Shift+Right sweeps on and toggles');
    await key(b, 'ArrowLeft', { meta: true });
    assert.deepEqual(await hand(), ['0-5', '3-11', '1-6', '3-13'], 'Meta+Left moved the card under the cursor');
    assert.deepEqual(await st(), ['cards', '3-11', ''], 'and the cursor with it');
    assert.deepEqual((await kernel(b)).hand, ['0-5', '3-11', '1-6', '3-13'], 'kept in the game');
  });

  await check('arrows: Up on a card that covers two battles aims (the arrow and the box on the target), Right cycles, Up covers the one shown', async () => {
    await board(1);   // 6c, the trump, covers 7h or Qd
    const st = () => b.evaluate(`(() => { const e = document.querySelector('[data-testid="kbd-state"]'); return [e.dataset.mode, e.dataset.sel, e.dataset.target]; })()`);
    for (let i = 0; i < 3; i++) await key(b, 'ArrowRight');   // 9h, Kd, 6c
    assert.deepEqual(await st(), ['cards', '2-5', '']);
    await key(b, 'ArrowUp');
    assert.deepEqual(await st(), ['target', '2-5', '1-6'], 'aiming at the first battle the kernel lists');
    const arrow = () => b.evaluate(`(() => { const s = document.querySelector('svg marker#kbd-arrow')?.closest('svg'); if (!s) return null;
      const l = s.querySelector('line'), r = s.querySelector(':scope > rect'), t = document.querySelector('[data-battle-index][data-card="' + document.querySelector('[data-testid="kbd-state"]').dataset.target + '"]').getBoundingClientRect();
      const c = document.querySelector('[data-hand-plane] [data-card="2-5"]').getBoundingClientRect();
      return { from: [+l.getAttribute('x1') - (c.x + c.width / 2), +l.getAttribute('y1') - c.y], to: [+l.getAttribute('x2') - (t.x + t.width / 2), +l.getAttribute('y2') - (t.y + t.height / 2)],
        box: [+r.getAttribute('x') - (t.x - 3), +r.getAttribute('width') - (t.width + 6)], stroke: l.getAttribute('stroke'), z: s.style.zIndex }; })()`);
    const a = await arrow();
    for (const d of [...a.from, ...a.to, ...a.box]) assert.ok(Math.abs(d) < 0.01, `the arrow from the card's top to the target's centre, the box 3 px out: ${JSON.stringify(a)}`);
    assert.equal(a.stroke, '#ff2d2d');
    await key(b, 'ArrowRight');
    assert.deepEqual(await st(), ['target', '2-5', '3-11'], 'Right cycles to the next');
    await key(b, 'ArrowUp');
    await idle();
    assert.deepEqual(await battles(), ['1-6', '3-11/2-5'], 'Up covered the Qd with the trump');
    assert.deepEqual((await st())[0], 'cards', 'the aim is over, the cursor stays');
    assert.equal(await arrow(), null, 'the arrow is gone');
  });

  await check('arrows: a card that may cover or transfer aims at the battle, then the PASS place right of the row; Down takes when Take shows, and says Good when Good shows', async () => {
    await board(2);   // 6c covers 6h or transfers
    await key(b, 'ArrowRight');   // 6c
    await key(b, 'ArrowUp');
    await key(b, 'ArrowRight');
    const pass = await b.evaluate(`(() => { const s = document.querySelector('svg marker#kbd-arrow').closest('svg'), l = s.querySelector('line'), t = s.querySelector('text');
      const cells = [...document.querySelectorAll('[data-location="table"]')].map((e) => e.getBoundingClientRect());
      const right = Math.max(...cells.map((r) => r.right)), mid = (Math.min(...cells.map((r) => r.top)) + Math.max(...cells.map((r) => r.bottom))) / 2;
      return [+l.getAttribute('x2') - (right + 46), +l.getAttribute('y2') - mid, l.getAttribute('stroke-dasharray'), t.textContent, document.querySelector('[data-testid="kbd-state"]').dataset.target]; })()`);
    assert.ok(Math.abs(pass[0]) < 0.01 && Math.abs(pass[1]) < 0.01, `the pass point is 46 px right of the row, at its middle: ${pass}`);
    assert.deepEqual(pass.slice(2), ['7 5', 'PASS', 'pass']);
    await key(b, 'ArrowDown');
    assert.equal((await b.evaluate(`document.querySelector('[data-testid="kbd-state"]').dataset.mode`)), 'cards', 'Down gives up the aim');
    assert.ok((await pills()).includes('Pickup'), 'Take shows');
    await key(b, 'ArrowDown');
    await idle();
    assert.deepEqual(await battles(), [], 'Down took');
    assert.ok((await hand()).includes('1-5'), 'the 6h is in my hand');
    await board(3);   // all covered: Good shows
    assert.deepEqual(await pills(), ['Good']);
    await key(b, 'ArrowDown');
    await idle();
    assert.deepEqual(await b.evaluate('window.app.then((x) => x.app_last_reject())'), 0);
    assert.deepEqual(await pills(), [], 'Down said Good');
  });

  await check('the table is the kernel\'s game and keeps React\'s rules at every click and bot step of played games (tools/consist.mjs)', async () => {
    await openTable(b, 'http://127.0.0.1:8720/c/cdom_poc/out/index.html?debug'); // the golden's board, nothing kept
    const r = await consistRun(b, { games: 3, seed: 1 });
    console.log(`     (${r.games} games, ${r.steps} steps; ${r.presses} pill presses, ${r.stillOffered} with the pill still offered after its move; ${r.flightsSeen} flights checked mid-air)`);
    const lines = [...r.kinds].map(([kind, { n, first }]) => `${kind} x${n}: at step ${first.step} after ${first.after} (before: ${first.recent.join(', ')}): ${first.detail}`);
    assert.deepEqual(lines, [], 'no disagreement');
    assert.equal(r.games, 3, 'three games played out');
  });

  await check('a reload lands in the same game, which plays on to its end; after Play again a reload lands in the new one', async () => {
    const page = 'http://127.0.0.1:8720/c/cdom_poc/out/index.html?debug';
    const board = async () => { const d = await b.evaluate(READ_DOM); d.hand = d.hand.map((h) => [h.card, h.left, h.bottom]); return JSON.stringify(d); };
    const reload = async () => { await new Promise((r) => setTimeout(r, 200)); await b.goto(page); await ready(); await hold(); await speed(); };
    await b.evaluate(`indexedDB.deleteDatabase('cdom'), 1`);
    await reload();
    assert.equal(await b.evaluate(`window.app.then((x) => x.app_game_store(2))`), 0, 'nothing stored: the seeded deal');
    // Seed 3 plays the moves below to 6h, 6d and 6s attacking me, 6h uncovered where the golden's
    // board covers it (drawn over that board, the kept game once kept the golden's 8h on 6h).
    await b.evaluate(`window.app.then((x) => x.app_debug_reseed(3))`);
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
    await idle();
    const kept = await board();
    const k = await kernel(b);
    assert.equal(JSON.stringify(JSON.parse(kept).battles), JSON.stringify(k.battles), 'the battles before the reload are the kernel\'s');
    assert.deepEqual(k.battles, ['1-5/-', '3-5/-', '0-5/-'], 'seed 3\'s line: 6h uncovered, which the golden board covers');
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
    await idle(); // the deal's run
    const fresh = await board();
    assert.equal(await all('[data-hand-plane] [data-card]', '1').then((x) => x.length), 6, 'a new deal');
    await reload();
    assert.equal(await board(), fresh, 'the new game after a reload');
  });

  await check('a save that fails is not silent: a notice until a later save is kept, and the save is tried again', async () => {
    const page = 'http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug';
    await openTable(b, page, { hold: true });
    // IndexedDB refuses the game's key from now on (the put throws), as a full quota would.
    await b.evaluate(`(() => { const put = IDBObjectStore.prototype.put; window.$put = put;
      IDBObjectStore.prototype.put = function (v, k) { if (k === 'foolish-game-v1') throw new DOMException('full', 'QuotaExceededError'); return put.call(this, v, k); };
      return 1; })()`);
    assert.equal(await b.evaluate(`document.querySelector('[data-save-failed]')`), null, 'no notice before a save');
    await b.click('[data-hand-plane] [data-card="0-5"]'); await b.click('[data-pill="Attack"]');
    await b.until(`document.querySelector('[data-save-failed]')`, 3000);
    assert.match(await b.evaluate(`document.querySelector('[data-save-failed]').textContent`), /^Not saved/);
    // The store takes it again: the retry keeps the game, the notice goes, and a reload lands in it.
    await b.evaluate(`(IDBObjectStore.prototype.put = window.$put, 1)`);
    await b.until(`!document.querySelector('[data-save-failed]')`, 5000);
    const battles = await all('[data-table-container] [data-battle-index]', 'e.dataset.card');
    await b.goto(page); await ready();
    assert.equal(await b.evaluate(`window.app.then((x) => x.app_game_store(2))`), 1, 'the retried save adopted');
    assert.deepEqual(await all('[data-table-container] [data-battle-index]', 'e.dataset.card'), battles);
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
    await speed();
    await b.evaluate(RECORD);
    // the trump, read off its slot: rank and suit glyph (spades, hearts, clubs, diamonds are suits 0-3; ace is 13)
    const face = await b.evaluate(`document.querySelector('[data-location="flipped"]').textContent.replace(/\\s/g, '')`);
    const glyph = [...face].at(-1), rank = face.slice(0, face.indexOf(glyph));
    const trumpId = ['♠', '♥', '♣', '♦'].indexOf(glyph) * 13 + ({ J: 10, Q: 11, K: 12, A: 13 }[rank] ?? Number(rank) - 1) - 1;
    const moves = await playToEnd(b, 400, woodCovered);
    await woodCovered();
    console.log(`     (${moves} moves of mine to the end)`);
    const out = (await flights()).filter((x) => x.card === trumpId);
    assert.ok(out.length >= 1, `the trump (${face}) is drawn, as the last card of the stock`);
    assert.ok(out[0].slot, 'the trump\'s slot is on screen as its first flight leaves');
    near([out[0].start.x, out[0].start.y], out[0].slot, 1, 'its first flight leaves from its own slot, face up');
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
  for (const mode of ['drag', 'touch', 'keys']) {
    await check(`a game played out by ${mode === 'keys' ? 'the arrow keys alone' : mode === 'touch' ? 'touch drags' : 'mouse drags'} reaches the end screen (tools/play.mjs ${mode})`, async () => {
      await openTable(b, 'http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug', { fast: true });
      await speed();
      const moves = await playToEnd(b, 400, null, mode);
      console.log(`     (${moves} moves of mine to the end)`);
      assert.equal(await turn(), 'over');
      assert.equal(await b.evaluate(`document.querySelector('[data-end] h1').textContent`), 'done - Two of us');
    });
  }
} finally {
  await b.close();
  server.close();
}
if (failed) { console.log(`${failed} failed`); process.exit(1); }
console.log('all passed');
