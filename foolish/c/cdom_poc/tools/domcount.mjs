// DOM calls per action: every DOM method and setter the shim reaches is counted for one
// select, one play (the Attack pill) and one bot step, on the golden board (textures off).
//   PAGE=/c/cdom_poc/out_before/index.html node tools/domcount.mjs   for another build
import { launch, serve, FOOLISH } from './cdp.mjs';
const server = await serve(8720, FOOLISH);
const b = await launch();
try {
  await b.goto('http://127.0.0.1:8720' + (process.env.PAGE || '/c/cdom_poc/out/index.html?notex'));
  await b.until(`document.querySelector('[data-game-container]')?.dataset.turn === 'human'`);
  const r = await b.evaluate(`(async () => {
    const x = await window.app, counts = {};
    let on = false;
    const wrap = (proto, name) => { const f = proto[name]; proto[name] = function (...a) { if (on) counts[name] = (counts[name] || 0) + 1; return f.apply(this, a); }; };
    const wrapSet = (proto, name) => { const d = Object.getOwnPropertyDescriptor(proto, name);
      Object.defineProperty(proto, name, { ...d, set(v) { if (on) counts[name + '='] = (counts[name + '='] || 0) + 1; d.set.call(this, v); } }); };
    for (const n of ['appendChild', 'insertBefore', 'removeChild', 'cloneNode']) wrap(Node.prototype, n);
    for (const n of ['setAttribute', 'remove']) wrap(Element.prototype, n);
    for (const n of ['createElement', 'createElementNS', 'createTextNode']) wrap(Document.prototype, n);
    for (const n of ['nodeValue', 'textContent']) wrapSet(Node.prototype, n);
    wrapSet(Element.prototype, 'className');
    wrap(CSSStyleDeclaration.prototype, 'setProperty');
    const run = (f) => { for (const k in counts) delete counts[k]; on = true; f(); on = false; return { ...counts }; };
    return {
      select: run(() => document.querySelector('[data-hand-plane] [data-card="0-5"]').click()),
      play: run(() => document.querySelector('[data-pill="Attack"]').click()),
      bot: run(() => x.cd_timer(1)),
    };
  })()`);
  for (const [k, v] of Object.entries(r)) console.log(k.padEnd(7), Object.values(v).reduce((a, c) => a + c, 0), JSON.stringify(v));
} finally { await b.close(); server.close(); }
