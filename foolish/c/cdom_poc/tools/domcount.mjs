// DOM calls per action: every DOM method and setter the shim reaches is counted for one
// select, one play (the Attack pill) and one bot step, on the golden board (textures off, the test build),
// each until its animations have landed: the handler's calls, then the frames' and timers' after it.
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
    for (const n of ['setAttribute', 'remove']) wrap(Element.prototype, n);
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
    return {
      select: await run(() => document.querySelector('[data-hand-plane] [data-card="0-5"]').click()),
      play: await run(() => document.querySelector('[data-pill="Attack"]').click()),
      bot: await run(() => x.cd_timer(3)),
    };
  })()`);
  for (const [k, v] of Object.entries(r)) console.log(k.padEnd(7), `${v.handler + v.after} (handler ${v.handler}, frames and timers after it ${v.after})`, JSON.stringify(v.all));
} finally { await b.close(); server.close(); }
