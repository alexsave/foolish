// Is the table on screen the kernel's game, and does it behave as the React table does?
// Plays games by real input with a seeded random hand (cards picked and put down by clicks, taps,
// number keys and the arrow cursor; pills; the table and a battle clicked; cards dragged onto the
// table or a battle by mouse and by touch, and along the hand; letter keys; Up and Down), the bot's
// steps held and released one at a time (?debug: table_debug.wasm),
// and after EVERY click and every bot step reads the DOM and compares it with
//   - the kernel's state, worked out afresh from the game round-tripped through its blob
//     (app_debug_state: the hand, the battles, the stock and the trump, the discard pile, each
//     seat's mark and count, the end), and
//   - a model of React's own rules for what the kernel does not hold (spec_state.md section 2):
//     the selection (a tap toggles; Attack, Pass and Cover spend it when pressed, a table or
//     battle drop when it is a move; Pickup and Good leave it), the pressed pills (hidden until the
//     pill falls and rises again, ActionButtons.tsx rearm), the end screen (WinScreen.tsx), the
//     hand's order (clientReconcile.ts: a sticky memory that drags and Meta+arrows rearrange), and
//     the keyboard's cursor (KeyboardPlayMode.tsx: the card it is on, kept across moves, clamped).
// The board is judged once the animations have landed (app_anim_busy); while a flight is up, right
// after the action that started it, every card in the air must be hidden where it is drawn on the
// table or in my hand (it shows once, in its flight). The run's clock goes 4 times as fast here
// (app_debug_anim_speed: the kernel's plan untouched), so a game takes seconds, not minutes.
// Every disagreement is printed once per kind with the step that showed it, and counted.
//
//   node tools/consist.mjs [games] [seed]        (after ./build.sh)
// As a module: consistRun(b, { games, seed }) returns { steps, games, kinds: Map(kind -> {n, first}) }.
import { sleep } from './cdp.mjs';
import { drag, key, centre } from './input.mjs';

const COLUMN = ['Attack', 'Cover', 'Pass', 'Pickup', 'Good'];

function mulberry32(a) {
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// The kernel's lines (app_debug_state) as an object of word lists.
export async function kernel(b) {
  const text = await b.evaluate(`window.app.then((x) => { const p = x.app_debug_state();
    return new TextDecoder().decode(new Uint8Array(x.memory.buffer, p, x.app_debug_len())); })`);
  const k = { seats: [] };
  for (const line of text.trim().split('\n')) {
    const [key, ...rest] = line.split(' ');
    if (key === 'seat') k.seats.push({ mark: rest[1], count: Number(rest[2]) });
    else k[key] = rest;
  }
  return k;
}

// The DOM, read the same way.
export const READ_DOM = `(() => {
  const all = (sel, f) => [...document.querySelectorAll(sel)].map(f);
  const hand = all('[data-hand-plane] [data-card]', (e) => ({ card: e.dataset.card, left: parseFloat(e.style.left), bottom: parseFloat(e.style.bottom), red: e.style.borderColor === 'red',
    box: [e.style.left, e.style.bottom, e.style.width, e.style.height].map((v) => String(parseFloat(v))).join(',') }));
  const deck = document.querySelector('[data-location="deck"]');
  const flipped = document.querySelector('[data-location="flipped"]');
  const discard = document.querySelector('[data-location="discard"]');
  const end = document.querySelector('[data-end]');
  return {
    turn: document.querySelector('[data-game-container]').dataset.turn,
    hand,
    pills: all('[data-pill]', (e) => e.dataset.pill),
    battles: all('[data-table-container] > [data-location="table"]', (e) => {
      const c = [...e.querySelectorAll('[data-card]')].map((x) => x.dataset.card);
      return c[0] + '/' + (c[1] || '-'); }),
    deckBadge: deck?.querySelector('p')?.textContent ?? null,
    deckLayers: deck ? deck.querySelector('div').children.length : null,
    flipped: flipped ? flipped.textContent.replace(/\\s/g, '') : null,
    hasFlippedSlot: !!flipped,
    trumpIcon: [...document.querySelectorAll('span')].find((s) => s.style.fontSize === '64px')?.textContent ?? null,
    kbd: { mode: document.querySelector('[data-testid="kbd-state"]').dataset.mode, sel: document.querySelector('[data-testid="kbd-state"]').dataset.sel },
    discardBadge: discard.querySelector('p')?.textContent ?? null,
    discardLayers: discard.querySelector('div')?.children.length ?? 0,
    seats: all('[data-role-seat]', (e) => {
      const h = e.parentElement.lastElementChild;
      const badge = [...h.children].find((c) => c.style.fontSize === '10px');
      return { mark: e.dataset.roleMark || 'none', minis: h.children.length - (badge ? 1 : 0), badge: badge ? badge.textContent : null };
    }),
    end: end ? { title: end.querySelector('h1').textContent,
      rows: all('[data-end] .result-card', (r) => r.textContent),
      button: end.querySelector('button').textContent } : null,
  };
})()`;

// A real click on a hand card where it shows: a long hand overlaps, and a card's centre can be
// under the next one (or, in two rows, the row in front), so the point is the first along its
// middle row, then rows above and below, that hits the card itself.
export async function clickCard(b, card) {
  const sel = `[data-hand-plane] [data-card="${card}"]`;
  // the whole hand at rest: every card's transition over (its computed box is the one it was given,
  // to Chrome's 1/64 px layout unit;
  // a card slides for 0.1 s when the hand changes, over its neighbours)
  await b.until(`[...document.querySelectorAll('[data-hand-plane] [data-card]')].every((e) => { const c = getComputedStyle(e);
    return ['left', 'bottom', 'width', 'height'].every((p) => Math.abs(parseFloat(c[p]) - parseFloat(e.style[p])) < 0.05); })`, 2000)
    .catch(async (err) => { console.log(await b.evaluate(`[...document.querySelectorAll('[data-hand-plane] [data-card]')].map((e) => { const c = getComputedStyle(e);
    return e.dataset.card + ' ' + ['left', 'bottom', 'width', 'height'].map((p) => c[p] + '|' + e.style[p]).join(' '); })`)); throw err; });
  const at = await b.evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}), r = e.getBoundingClientRect();
    for (const f of [0.5, 0.25, 0.75, 0.1, 0.9]) for (let x = r.left + 2; x < r.right; x += 2) { const y = r.top + r.height * f;
      if (document.elementFromPoint(x, y)?.closest('[data-card]') === e) return { x, y }; }
    return null; })()`);
  if (!at) {
    const why = await b.evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}), r = e.getBoundingClientRect();
      const top = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
      return JSON.stringify({ rect: [r.left, r.top, r.width, r.height].map(Math.round), vw: innerWidth, vh: innerHeight, cards: document.querySelectorAll('[data-hand-plane] [data-card]').length,
        top: top ? [top === e, top.closest('[data-card]')?.dataset.card, top.closest('[data-card]') === e, top.parentElement?.tagName, top.closest('[data-hand-plane]') ? 'in plane' : 'outside', top.outerHTML.slice(0, 60)] : null, same: document.querySelectorAll('[data-card="' + e.dataset.card + '"]').length style: e.getAttribute('style').slice(0, 200) }); })()`);
    throw new Error(`card ${card} shows nowhere: ${why}`);
  }
  for (const type of ['mousePressed', 'mouseReleased'])
    await b.send('Input.dispatchMouseEvent', { type, x: at.x, y: at.y, button: 'left', clickCount: 1 });
}

export async function consistRun(b, { games = 3, seed = 1, maxSteps = 4000, log = () => {} } = {}) {
  const settledHand = () => b.until(`[...document.querySelectorAll('[data-hand-plane] [data-card]')].every((e) => { const c = getComputedStyle(e);
    return ['left', 'bottom', 'width', 'height'].every((p) => Math.abs(parseFloat(c[p]) - parseFloat(e.style[p])) < 0.05); })`, 2000);
  const rnd = mulberry32(seed);
  const kinds = new Map();
  let step = 0, done = 0, what = 'load', presses = 0, stillOffered = 0, justPressed = null;
  const history = [];
  const flag = (kind, detail) => {
    const k = kinds.get(kind);
    if (k) { k.n++; return; }
    kinds.set(kind, { n: 1, first: { step, after: what, detail, recent: history.slice(-6) } });
    log(`DISAGREE ${kind} (step ${step}, after ${what}): ${detail}`);
  };
  // React's state, which the kernel does not hold: the selection, the pressed pills, the hand's
  // remembered order, and the keyboard's cursor (its index, and whether it is aiming).
  let selection = [], pressed = new Set(), prevRearm = new Set(), memory = [], cursor = -1, aiming = false;
  const remember = (m, hand) => { const out = [...new Set(m)]; for (const c of hand) if (!out.includes(c)) out.push(c); return out; };
  const shown = (m, hand) => [...m.filter((c) => hand.includes(c)), ...hand.filter((c) => !m.includes(c))];
  const toggle = (c) => { selection = selection.includes(c) ? selection.filter((x) => x !== c) : [...selection, c]; };

  // While a flight is up: each card in the air hidden wherever the hand or the table draws it.
  let flightsSeen = 0;
  const midFlight = async () => {
    const r = await b.evaluate(`window.app.then((x) => { const out = [], n = x.app_anim_flying();
      for (let i = 0; i < n; i++) { const id = x.app_anim_flight_card(i); if (id < 0) continue;
        const card = Math.floor(id / 13) + '-' + (id % 13 + 1);
        for (const e of document.querySelectorAll('[data-hand-plane] [data-card="' + card + '"], [data-table-container] [data-card="' + card + '"]'))
          if (getComputedStyle(e).visibility !== 'hidden') out.push(card + ' shows in ' + (e.closest('[data-hand-plane]') ? 'my hand' : 'the table')); }
      return [n, out]; })`);
    flightsSeen += r[0];
    if (r[1].length) flag('a flying card shows where it is drawn', r[1].join(', '));
  };
  // modelBefore(k): React's model updated for what the kernel now says, before the DOM is judged.
  const compare = async (modelBefore = () => {}) => {
    await midFlight();
    await b.until('window.app.then((x) => !x.app_anim_busy())', 30000).catch(async (err) => {
      console.log('RAF', await b.evaluate(`new Promise((r) => { requestAnimationFrame(() => r('fires')); setTimeout(() => r('no frame in 1 s, hidden ' + document.hidden + ' ' + document.visibilityState), 1000); })`));
      console.log('STUCK', await b.evaluate(`window.app.then((x) => JSON.stringify({ busy: x.app_anim_busy(), steps: x.app_anim_steps(), landed: x.app_anim_landed(), origin: x.app_anim_origin(), now: performance.now(), flying: x.app_anim_flying(), plan: [...Array(x.app_anim_steps()).keys()].map((k) => [x.app_anim_step(k, 0), x.app_anim_step(k, 1), x.app_anim_step(k, 2)]) }))`));
      throw err; });
    const k = await kernel(b), d = await b.evaluate(READ_DOM);
    modelBefore(k);
    const eq = (a, x) => JSON.stringify(a) === JSON.stringify(x);
    // the hand in React's order: the kernel's cards, ordered by the remembered order
    memory = remember(memory, k.hand);
    const handK = shown(memory, k.hand);
    if (!eq(d.hand.map((h) => h.card), handK)) flag('hand order', `DOM ${d.hand.map((h) => h.card).join(' ')} React ${handK.join(' ')} (kernel ${k.hand.join(' ')})`);
    // the keyboard's cursor: kept on its index across moves, on the last card when the hand shrank
    if (cursor >= handK.length) cursor = handK.length ? handK.length - 1 : -1;
    const wantKb = [aiming ? 'target' : cursor >= 0 ? 'cards' : 'none', cursor >= 0 ? handK[cursor] : ''];
    if (!eq([d.kbd.mode, d.kbd.sel], wantKb)) flag('keyboard cursor', `DOM ${d.kbd.mode} ${d.kbd.sel} React ${wantKb.join(' ')}`);
    // each card where the kernel's hand_layout puts it at this width (left, bottom, width, height)
    // (within 0.001 px: the port writes a length to 4 decimals, cdom's cd_fmt_len, and Chrome keeps
    // 6 significant digits of it, as it does of React's full double)
    const near = (a, b) => a !== undefined && b !== undefined && a.split(',').every((v, j) => Math.abs(Number(v) - Number(b.split(',')[j])) < 0.001);
    const off = d.hand.findIndex((h, j) => !near(h.box, k.slots[j]));
    if (off >= 0 || d.hand.length !== k.slots.length)
      flag('hand card placed off its slot', `${d.hand[off]?.card} at ${d.hand[off]?.box}, kernel ${k.slots[off]} (${d.hand.length} cards)`);
    // in the kernel's order along each row (hand_layout: a long hand is two rows)
    const rows = new Map();
    for (const h of d.hand) rows.set(h.bottom, [...(rows.get(h.bottom) || []), h.left]);
    for (const [bottom, lefts] of rows) if (lefts.some((l, i) => i && l <= lefts[i - 1])) flag('hand laid out out of order', `row at bottom ${bottom}: ${lefts.join(' ')}`);
    // the selection: React's model, shown as red borders on the cards still in the hand
    const want = handK.filter((c) => selection.includes(c));
    const red = d.hand.filter((h) => h.red).map((h) => h.card);
    if (!eq(red, want)) flag('selection shown', `red ${red.join(' ') || '(none)'} React ${want.join(' ') || '(none)'} (port holds ${k.sel.join(' ') || 'nothing'})`);
    if (!eq([...k.sel].sort(), [...selection].sort())) flag('selection held', `port ${k.sel.join(' ') || '(none)'} React ${selection.join(' ') || '(none)'}`);
    // the pills: the kernel's for the selection, less the pressed ones (rearm on a rising edge)
    const rearm = new Set([...k.kernel_pills, ...k.kernel_pills_none]);
    // how often a press is visible at all: its pill still offered by the kernel once its move is in
    if (justPressed) { presses++; if (rearm.has(justPressed) && pressed.has(justPressed)) stillOffered++; justPressed = null; }
    for (const p of [...pressed]) if (rearm.has(p) && !prevRearm.has(p)) pressed.delete(p);
    prevRearm = rearm;
    const wantPills = COLUMN.filter((p) => k.kernel_pills.includes(p) && !pressed.has(p));
    if (!eq(d.pills, wantPills)) flag('pills', `DOM ${d.pills.join(' ') || '(none)'} React ${wantPills.join(' ') || '(none)'} (kernel ${k.kernel_pills.join(' ') || 'none'}, pressed ${[...pressed].join(' ') || 'none'})`);
    if (!eq(d.battles, k.battles)) flag('battles', `DOM ${d.battles.join(' ')} kernel ${k.battles.join(' ')}`);
    // the stock, the trump, the discard pile
    const kd = k.deck, at = (w) => kd.indexOf(w);
    const badge = at('badge') >= 0 ? kd[at('badge') + 1] : null, layers = at('layers') >= 0 ? Number(kd[at('layers') + 1]) : null;
    if (d.deckBadge !== badge || (badge !== null && d.deckLayers !== layers)) flag('stock', `DOM ${d.deckBadge}/${d.deckLayers} kernel ${badge}/${layers}`);
    const fl = at('flipped') >= 0 ? kd[at('flipped') + 1] : undefined;
    if (fl === undefined ? d.hasFlippedSlot : !d.hasFlippedSlot) flag('flipped slot', `DOM ${d.hasFlippedSlot} kernel ${fl}`);
    if (fl && fl !== '-') { const ch = [...fl], suit = ch.pop(), rank = ch.join(''); if (d.flipped !== rank + suit + rank + suit + suit) flag('trump card', `DOM ${d.flipped} kernel ${fl}`); }
    const tr = at('trump') >= 0 ? kd[at('trump') + 1] : null;
    if ((d.trumpIcon || null) !== tr) flag('trump icon', `DOM ${d.trumpIcon} kernel ${tr}`);
    const dis = k.discard[0] ?? null;
    if (d.discardBadge !== dis || (dis !== null && d.discardLayers !== Number(dis))) flag('discard', `DOM ${d.discardBadge}/${d.discardLayers} kernel ${dis}`);
    k.seats.forEach((s, i) => {
      const ds = d.seats[i];
      if (!ds) { flag('seat missing', `seat ${i}`); return; }
      if (ds.mark !== s.mark) flag('role mark', `seat ${i} DOM ${ds.mark} kernel ${s.mark}`);
      if (ds.minis !== s.count || (s.count > 0 ? ds.badge !== String(s.count) : ds.badge !== null)) flag('seat count', `seat ${i} DOM ${ds.minis}/${ds.badge} kernel ${s.count}`);
    });
    // whose step: the bot's while the kernel says it may act
    const over = k.status[0] === 'over';
    const wantTurn = over ? 'over' : Number(k.bot_eligible[0]) ? 'bot' : 'human';
    if (d.turn !== wantTurn) flag('turn', `DOM ${d.turn} kernel ${wantTurn}`);
    // the end screen, as WinScreen.tsx draws it: "done - <title>", the rows in the kernel's finish
    // order (anim_finish_rows: everyone out, then the fool), a medal by place and the fool's card
    // last, "(you)" after my name
    if (over) {
      if (!d.end) flag('end screen missing', '');
      else {
        const order = [...k.elimination.map(Number)];
        const fool = Number(k.fool[0]);
        if (fool >= 0 && !order.includes(fool)) order.push(fool);
        const names = ['Me', 'Anna'], icon = (place) => place === order.length ? '\u{1F0CF}' : ['\u{1F947}', '\u{1F948}', '\u{1F949}'][place - 1];
        const rows = order.map((seat, i) => `${icon(i + 1)}${names[seat]}${seat === 0 ? '(You)' : ''}`);
        if (d.end.title !== 'done - Two of us') flag('end title', `DOM "${d.end.title}" React "done - Two of us"`);
        if (!eq(d.end.rows, rows)) flag('end rows', `DOM ${JSON.stringify(d.end.rows)} React ${JSON.stringify(rows)}`);
      }
    } else if (d.end) flag('end screen while playing', d.end.title);
    return { k, d, over };
  };

  // the kernel seeded too (the bot's choices, the draws, the next deals): one seed, one run
  await b.evaluate(`window.app.then((x) => { x.app_debug_hold(1); x.app_debug_fast(1); x.app_debug_anim_speed(4); x.app_debug_reseed(${seed}); })`);
  let state = await compare();
  for (; step < maxSteps && done < games; step++) {
    const { k, d, over } = state;
    // the bot's turn by the board alone (not by whether its 30 ms are up), so a seed is one run
    const due = d.turn === 'bot';
    const before = JSON.stringify([k.hand, k.battles, k.seats, k.status]);
    if (over) {
      what = 'Play again';
      await b.click('[data-end] button');
      selection = []; pressed = new Set(); prevRearm = new Set(); memory = []; cursor = -1; aiming = false;
      done++;
    } else if (due && (rnd() < 0.8 || (!d.pills.length && !d.hand.length))) {
      what = 'a bot step';
      await b.until(`window.app.then((x) => x.app_debug_due())`, 5000);
      const n = await b.evaluate(`window.app.then((x) => { const n = x.app_bot_steps(); x.app_debug_release(); return n; })`);
      await b.until(`window.app.then((x) => x.app_bot_steps() > ${n})`, 2000);
    } else {
      const r = rnd();
      const mine = d.hand.filter((h) => h.red).map((h) => h.card);
      if (d.pills.length && r < 0.7) {
        const p = d.pills[Math.floor(rnd() * d.pills.length)];
        what = `the ${p} pill`;
        await b.click(`[data-pill="${p}"]`);
        const reject = await b.evaluate(`window.app.then((x) => x.app_last_reject())`);
        pressed.add(p);
        justPressed = p;
        if (p === 'Attack' || p === 'Pass' || p === 'Cover') selection = [];
        if (reject) pressed.delete(p);
      } else if (mine.length && rnd() < 0.6) {
        const c = mine[Math.floor(rnd() * mine.length)];
        what = `card ${c}`;
        await clickCard(b, c);
        selection = selection.filter((x) => x !== c);
      } else if (d.battles.length && mine.length && r < 0.6) {
        const i = Math.floor(rnd() * d.battles.length);
        what = `battle ${d.battles[i]}`;
        await b.click(`[data-table-container] > [data-location="table"]:nth-child(${i + 1}) [data-card]`);
      } else if (mine.length && r < 0.65) {
        what = 'the empty table';
        await b.click('[data-table-container]');
      } else if (d.hand.length && r < 0.85) {
        // a card dragged by mouse or by touch: out of the hand onto a battle or the open table, or
        // along the hand to another card's place. Within the hand's area it trades places with
        // every card it passes over, on the way out too; the model replays that on the kernel's
        // slots (the topmost card under each point, DragContext.tsx), the port draws it.
        const i = Math.floor(rnd() * d.hand.length), c = d.hand[i].card, touch = rnd() < 0.5;
        await settledHand();
        const from = await centre(b, `[data-hand-plane] [data-card="${c}"]`);
        let to, path;
        if (rnd() < 0.7) {
          const t = Math.floor(rnd() * (d.battles.length + 1));
          to = t < d.battles.length ? await centre(b, `[data-table-container] > [data-location="table"]:nth-child(${t + 1}) [data-battle-index]`) : { x: 512, y: 300 };
          what = `${touch ? 'touch' : 'mouse'} drag of ${c} onto ${t < d.battles.length ? 'battle ' + d.battles[t] : 'the table'}`;
          path = [to];
        } else {
          const j = Math.floor(rnd() * d.hand.length), at = await centre(b, `[data-hand-plane] [data-card="${d.hand[j].card}"]`);
          what = `${touch ? 'touch' : 'mouse'} rearrange of ${c} toward ${d.hand[j].card}`;
          path = [{ x: from.x, y: from.y - 12 }, at];
        }
        const pts = [];
        let at = from;
        for (const q of path) { for (let s = 1; s <= 8; s++) pts.push({ x: at.x + (q.x - at.x) * s / 8, y: at.y + (q.y - at.y) * s / 8 }); at = q; }
        const geo = await b.evaluate(`(() => { const r = document.querySelector('[data-hand-plane]').getBoundingClientRect(); return [r.left, r.top, r.bottom]; })()`);
        const kk = await kernel(b);
        const order = d.hand.map((h) => h.card), slots = kk.slots.map((v) => v.split(',').map(Number));
        let idx = i, moving = false, swapped = false;
        for (const q of pts) {
          if ((q.x - from.x) ** 2 + (q.y - from.y) ** 2 > 100) moving = true;
          if (!moving || q.y < geo[1] - 70) continue;   // still a press, or out of the hand's area
          let hit = -1;
          slots.forEach(([x, bottom, w, h], k) => { const left = geo[0] + x, top = geo[2] - bottom - h;
            if (q.x >= left && q.x < left + w && q.y >= top && q.y < top + h) hit = k; });   // the last drawn is on top
          if (hit >= 0 && hit !== idx) { [order[idx], order[hit]] = [order[hit], order[idx]]; idx = hit; swapped = true; }
        }
        await drag(b, from, pts, { touch, steps: 1 });
        if (swapped) memory = [...order, ...memory.filter((x) => !order.includes(x))];
      } else if (r < 0.95) {
        // a key: a number toggles a card, a letter plays as its button would, an arrow moves the cursor, Up and Down play
        const keys = ['1', '2', '3', '4', '5', '6', 'a', 'c', 'p', 'u', 'g', ' ', 'ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Shift'];
        const k = keys[Math.floor(rnd() * keys.length)], order = d.hand.map((h) => h.card);
        what = `key ${JSON.stringify(k)}`;
        await key(b, k);
        const n = order.length;
        if (/^[1-6]$/.test(k)) { if (Number(k) <= n) toggle(order[Number(k) - 1]); }
        else if (k === 'Shift') { if (cursor >= 0 && cursor < n && !aiming) toggle(order[cursor]); }
        else if (aiming) { if (k === 'ArrowUp' || k === 'ArrowDown') aiming = false; }   // the aim plays, or is given up
        else if (k === 'ArrowLeft' || k === 'ArrowRight') { if (cursor < 0) { if (n) cursor = 0; } else if (n) cursor = (cursor + (k === 'ArrowLeft' ? n - 1 : 1)) % n; }
        // whether Up aims is the kernel's (client_cursor_play, held in c/tests): read, then held from here on
        else if (k === 'ArrowUp' && cursor >= 0 && !selection.length) aiming = (await b.evaluate(`document.querySelector('[data-testid="kbd-state"]').dataset.mode`)) === 'target';
      } else if (d.hand.length) {
        const c = d.hand[Math.floor(rnd() * d.hand.length)].card;
        what = `card ${c}`;
        await clickCard(b, c);
        selection = selection.includes(c) ? selection.filter((x) => x !== c) : [...selection, c];
      } else { await sleep(20); continue; }
    }
    history.push(what); if (process.env.TRACE) console.log(step, what, due, d.turn, d.pills.join(","), d.hand.length, await b.evaluate("document.visibilityState"));
    // a table or battle drop that was a move spends the selection (DragContext.tsx endCardDrag), and
    // so do the keys that play the selection (KeyboardInputHandler, and Up with cards selected)
    const drop = what.startsWith('battle') || what === 'the empty table' || what.includes(' drag of ')
      || what === 'key "a"' || what === 'key "c"' || what === 'key "p"' || what === 'key " "' || (what === 'key "ArrowUp"' && selection.length);
    state = await compare((k2) => {
      if (drop && before !== JSON.stringify([k2.hand, k2.battles, k2.seats, k2.status])) selection = [];
    });
  }
  return { steps: step, games: done, kinds, presses, stillOffered, flightsSeen };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const { launch, serve, FOOLISH } = await import('./cdp.mjs');
  const games = Number(process.argv[2] || 3), seed = Number(process.argv[3] || 1);
  const server = await serve(8720, FOOLISH);
  const b = await launch();
  let r;
  try {
    // a trap or an exception in the page is printed (a run that never ends is often one)
    b.listen('Runtime.exceptionThrown', (e) => console.log('PAGE EXCEPTION', e.exceptionDetails?.exception?.description?.slice(0, 600)));
    await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html?debug');
    await b.until(`window.appReady && window.app.then((x) => x.app_game_store(1) > 0)`);
    r = await consistRun(b, { games, seed, log: console.log });
  } finally { await b.close(); server.close(); }
  console.log(`${r.games} games, ${r.steps} steps, seed ${seed}; ${r.presses} pill presses, ${r.stillOffered} with the pill still offered after the move (a press hidden); ${r.flightsSeen} flights checked mid-air`);
  for (const [kind, { n, first }] of r.kinds) console.log(`${String(n).padStart(5)}  ${kind}: first at step ${first.step} after ${first.after} (before that: ${first.recent.join(', ')})\n       ${first.detail}`);
  process.exit(r.kinds.size ? 1 : 0);
}
