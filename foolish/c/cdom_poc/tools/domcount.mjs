// DOM calls per action: every DOM method and setter the shim reaches is counted for one
// select (a tap: the press and the release), one play (the Attack pill) and one bot step, then a
// drag of the 6s (the move that makes it a game action, a move whose answer is the same, the move
// onto 6d where the answer changes, the drop on the table) and a key (the cursor on, then a number),
// on the golden board (textures off, the test build), each until its animations have landed: the
// handler's calls, then the frames' and timers' after it.
//   PAGE=/c/cdom_poc/out_before/index.html node tools/domcount.mjs   for another build
import { launch, serve, FOOLISH, openTable } from './cdp.mjs';
const server = await serve(8720, FOOLISH);
const b = await launch();
try {
  // The test build, the bot held: its step is run where its timer would run it (the release path).
  await openTable(b, 'http://127.0.0.1:8720' + (process.env.PAGE || '/c/cdom_poc/out/index.html?notex&debug'), { hold: true });
  const r = await b.evaluate(`(async () => {
    const x = await window.app, counts = {};
    let on = false;
    const wrap = (proto, name) => { const f = proto[name]; proto[name] = function (...a) { if (on) counts[name] = (counts[name] || 0) + 1; return f.apply(this, a); }; };
    const wrapSet = (proto, name) => { const d = Object.getOwnPropertyDescriptor(proto, name);
      Object.defineProperty(proto, name, { ...d, set(v) { if (on) counts[name + '='] = (counts[name + '='] || 0) + 1; d.set.call(this, v); } }); };
    for (const n of ['appendChild', 'insertBefore', 'removeChild', 'cloneNode']) wrap(Node.prototype, n);
    for (const n of ['setAttribute', 'remove', 'removeAttribute']) wrap(Element.prototype, n);
    for (const n of ['createElement', 'createElementNS', 'createTextNode']) wrap(Document.prototype, n);
    for (const n of ['nodeValue', 'textContent']) wrapSet(Node.prototype, n);
    wrapSet(Element.prototype, 'className');
    wrap(CSSStyleDeclaration.prototype, 'setProperty');
    // An action is counted until its animations have landed (app_anim_busy; a build without them is
    // idle at once): the handler's own calls, and those of the frames and timers after it.
    const busy = () => (x.app_anim_busy ? x.app_anim_busy() : 0);
    const run = async (f) => {
      for (const k in counts) delete counts[k];
      on = true; f(); on = false;
      const now = { ...counts };
      on = true;
      while (busy()) await new Promise((r) => setTimeout(r, 5));
      await new Promise((r) => requestAnimationFrame(() => setTimeout(r, 0)));
      on = false;
      const all = Object.values(counts).reduce((a, c) => a + c, 0), first = Object.values(now).reduce((a, c) => a + c, 0);
      return { handler: first, after: all - first, all: { ...counts } };
    };
    const at = (sel) => { const r = document.querySelector(sel).getBoundingClientRect(); return { clientX: r.x + r.width / 2, clientY: r.y + r.height / 2 }; };
    const mouse = (target, type, p) => target.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, ...p }));
    const key = (k, code) => document.body.dispatchEvent(new KeyboardEvent('keydown', { bubbles: true, cancelable: true, key: k, keyCode: code }));
    const six = '[data-hand-plane] [data-card="0-5"]';
    const out = {
      select: await run(() => { const p = at(six); mouse(document.querySelector(six), 'mousedown', p); mouse(document, 'mouseup', p); }),
      play: await run(() => document.querySelector('[data-pill="Attack"]').click()),
      bot: await run(() => x.cd_timer(3)),
    };
    // a drag on the golden board, freshly put back (app_debug_board 0)
    x.app_debug_board(0);
    while (busy()) await new Promise((r) => setTimeout(r, 5));
    const from = at(six), six6d = at('[data-table-container] [data-battle-index][data-card="3-5"]');
    mouse(document.querySelector(six), 'mousedown', from);
    out['drag out'] = await run(() => mouse(document, 'mousemove', { clientX: 512, clientY: 300 }));
    out['drag same'] = await run(() => mouse(document, 'mousemove', { clientX: 520, clientY: 290 }));
    out['drag onto'] = await run(() => mouse(document, 'mousemove', six6d));
    mouse(document, 'mousemove', { clientX: 512, clientY: 300 });
    out.drop = await run(() => mouse(document, 'mouseup', { clientX: 512, clientY: 300 }));
    x.app_debug_board(0);
    while (busy()) await new Promise((r) => setTimeout(r, 5));
    out['key arrow'] = await run(() => key('ArrowRight', 39));
    out['key 2'] = await run(() => key('2', 50));
    return out;
  })()`);
  for (const [k, v] of Object.entries(r)) console.log(k.padEnd(7), `${v.handler + v.after} (handler ${v.handler}, frames and timers after it ${v.after})`, JSON.stringify(v.all));
} finally { await b.close(); server.close(); }
