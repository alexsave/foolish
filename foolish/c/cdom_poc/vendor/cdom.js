"use strict";
// cdom shim: C calls one of these functions per DOM operation, and the shim posts events back.
// Fixed per app. Wasm passes i32, so ids and numbers that may pass 2^31 are made unsigned (>>> 0).
var cdom = (() => {
  const EVENTS = ["click", "dblclick", "input", "change", "keydown", "keyup", "focusin",
    "focusout", "submit", "pointerdown", "pointerup", "pointermove"];
  const TRANSIENT = 0x40000000, DETACHED = 0xffffffff, SMALL = 64;
  const doc = document, td = new TextDecoder(), te = new TextEncoder(), SVGNS = "http://www.w3.org/2000/svg";
  const nodes = [], strs = [], tmp = [], tpl = [], stack = [], on = [], tslot = [];
  let slots = []; // slot text nodes of the template being built
  let lmask = new Uint16Array(1024); // listened event types per node id
  let x, mem, buf, u8, u32, i32, SB, SM, INBOX;
  let sp = 0, last = null, root = null, base = DETACHED, ref = 0;

  function views() {
    buf = mem.buffer;
    u8 = new Uint8Array(buf); u32 = new Uint32Array(buf); i32 = new Int32Array(buf);
  }
  const S = (a) => (a < TRANSIENT ? strs[a] : tmp[a - TRANSIENT]);
  function put(dst, s) { if (dst < TRANSIENT) strs[dst] = s; else tmp[dst - TRANSIENT] = s; }

  // Strings arrive together: one TextDecoder call per batch, and a char loop for tiny batches.
  function decode(n, nbytes) {
    let m = SM >> 2, o = 0;
    if (nbytes > SMALL) {
      const all = td.decode(u8.subarray(SB, SB + nbytes));
      for (let i = 0; i < n; i++, m += 3) { const len = u32[m + 2]; put(u32[m], all.substring(o, o + len)); o += len; }
      return;
    }
    for (let i = 0, p = SB; i < n; i++, m += 3) {
      const nb = u32[m + 1];
      let s;
      if (nb === u32[m + 2]) { s = ""; for (let k = 0; k < nb; k++) s += String.fromCharCode(u8[p + k]); }
      else s = td.decode(u8.subarray(p, p + nb));
      put(u32[m], s); p += nb;
    }
  }

  function place(n) {
    if (sp) stack[sp - 1].appendChild(n);
    else if (base !== DETACHED) nodes[base].insertBefore(n, ref ? nodes[ref] : null);
  }

  function delegate(type) {
    on[type] = 1;
    const bit = 1 << type, top = nodes[0];
    top.addEventListener(EVENTS[type], (e) => {
      let path = 0, depth = 0;
      for (let n = e.target; n && n !== top; n = n.parentNode) {
        if (n.$i !== undefined && lmask[n.$i] & bit) {
          if (buf !== mem.buffer) views();
          const k = INBOX >> 2;
          i32[k] = type; i32[k + 1] = n.$i; i32[k + 2] = e.clientX | 0; i32[k + 3] = e.clientY | 0;
          i32[k + 4] = e.button | 0; i32[k + 5] = e.keyCode | 0;
          i32[k + 6] = e.shiftKey | (e.ctrlKey << 1) | (e.altKey << 2) | (e.metaKey << 3);
          i32[k + 7] = path; i32[k + 8] = depth;
          x.cd_event();
          return;
        }
        let i = 0;
        for (let s = n.previousElementSibling; s; s = s.previousElementSibling) i++;
        path = (path << 8) | (i & 255); depth++;
      }
    });
  }

  function strings(n, nbytes) {
    if (buf !== mem.buffer) views();
    decode(n, nbytes);
  }
  function at(p, b) { base = p >>> 0; ref = b; }
  // An element inside an <svg> (and the <svg> itself) is made in the SVG namespace, as the HTML parser does. [poc]
  function open(t) {
    const tag = S(t), up = sp ? stack[sp - 1] : null;
    const el = tag === "svg" || (up && up.namespaceURI === SVGNS) ? doc.createElementNS(SVGNS, tag) : doc.createElement(tag);
    if (sp) up.appendChild(el); stack[sp++] = el; last = el;
  }
  function close() { const el = stack[--sp]; if (!sp) { root = el; place(el); } }
  function text(s) { last = doc.createTextNode(S(s)); place(last); }
  function text_num(n) { last = doc.createTextNode(n >>> 0); place(last); }
  function attr(a, v) { last.setAttribute(S(a), S(v)); }
  function cls(v) { last.className = S(v); }
  function name(n) { nodes[n] = last; }
  function listen(n, ev) {
    nodes[n] = last; last.$i = n;
    if (n >= lmask.length) { const g = new Uint16Array(n * 2); g.set(lmask); lmask = g; }
    lmask[n] |= 1 << ev; if (!on[ev]) delegate(ev);
  }
  function value(s) { last.nodeValue = S(s); }
  function value_num(n) { last.nodeValue = n >>> 0; }
  function tpl_save(t) { tpl[t] = root; tslot[t] = slots; slots = []; root.remove(); }
  // A clone opened inside an open element is that element's child, as an opened element is. [poc]
  function clone(t) { const el = tpl[t].cloneNode(true); if (sp) stack[sp - 1].appendChild(el); stack[sp++] = el; last = el; }
  function slot(k) { slots[k] = last; }
  function tpl_text(t, k, s) { tslot[t][k].nodeValue = S(s); }
  function tpl_text_num(t, k, n) { tslot[t][k].nodeValue = n >>> 0; }
  // A walk: n steps of 2 bits (1 firstChild, 2 nextSibling, 3 parentNode) from a named node, or
  // from the last node when from is -1.
  function walk(from, steps, n) {
    let el = from === -1 ? last : nodes[from];
    for (; n; n--, steps >>>= 2) el = (steps & 3) === 1 ? el.firstChild : (steps & 3) === 2 ? el.nextSibling : el.parentNode;
    last = el;
  }
  function walk_value(from, steps, n, s) { walk(from, steps, n); last.nodeValue = S(s); }
  function walk_value_num(from, steps, n, v) { walk(from, steps, n); last.nodeValue = v >>> 0; }
  function set_text(id, s) { nodes[id].nodeValue = S(s); }
  function set_text_num(id, n) { nodes[id].nodeValue = n >>> 0; }
  function set_attr(id, a, v) { nodes[id].setAttribute(S(a), S(v)); }
  function set_class(id, v) { nodes[id].className = S(v); }
  function remove(id) { nodes[id].remove(); nodes[id] = undefined; lmask[id] = 0; }
  function free(id) { nodes[id] = undefined; lmask[id] = 0; }
  function move(id, b) { const el = nodes[id]; el.parentNode.insertBefore(el, b ? nodes[b] : null); }
  function clear(id) { nodes[id].textContent = ""; }
  function detach(id) { const el = nodes[id]; el.$p = el.parentNode; el.$s = el.nextSibling; el.remove(); }
  function attach(id) { const el = nodes[id]; el.$p.insertBefore(el, el.$s); el.$p = el.$s = null; }

  function input(id, dst, cap) {
    const r = te.encodeInto(String(nodes[id].value ?? ""), new Uint8Array(mem.buffer, dst, cap));
    return r.written;
  }

  // [poc] A timer: C names a token, and cd_timer(token) is called when it fires.
  function timer(ms, token) { setTimeout(() => x.cd_timer(token), ms); }

  // [poc] The network, bytes straight into C memory. C reserves room with cd_net_buf(n) and is handed
  // (ptr, n) with the message; it never sees a string it did not write.
  const sockets = [];
  const str = (p, n) => td.decode(new Uint8Array(mem.buffer, p, n));
  function deliver(f, a, data) {
    const b = new Uint8Array(data), p = x.cd_net_buf(b.length);
    new Uint8Array(mem.buffer, p, b.length).set(b); f(a, p, b.length);
  }
  const net = {
    ws_open(p, n) {
      const id = sockets.length, ws = new WebSocket(str(p, n)); sockets.push(ws); ws.binaryType = "arraybuffer";
      ws.onopen = () => x.cd_ws_open(id);
      ws.onmessage = (e) => deliver(x.cd_ws_msg, id, typeof e.data === "string" ? te.encode(e.data) : e.data);
      ws.onclose = (e) => x.cd_ws_close(id, e.code);
      return id;
    },
    ws_send(id, p, n) { sockets[id].send(new Uint8Array(mem.buffer, p, n).slice()); },
    ws_close(id) { sockets[id].close(); },
    http(token, mp, mn, up, un, bp, bn) {
      const m = str(mp, mn), init = { method: m, body: bn ? new Uint8Array(mem.buffer, bp, bn).slice() : undefined };
      fetch(str(up, un), init).then(async (r) => deliver((t, p, n) => x.cd_http_done(t, r.status, p, n), token, await r.arrayBuffer()),
        () => x.cd_http_done(token, 0, 0, 0));
    },
  };

  // mount is an element, or the id of one that may not be parsed yet: then the wasm loads
  // while the parser and the stylesheets run, and the app starts once the DOM is ready.
  async function start(url, mount, env) {
    const imports = { cdom: { strings, at, open, close, text, text_num, attr, class: cls, name, listen, value,
      value_num, tpl_save, clone, slot, tpl_text, tpl_text_num, walk, walk_value, walk_value_num, set_text,
      set_text_num, set_attr, set_class, remove, free, move, clear, detach, attach, input, timer }, net, env: env || {} };
    const res = fetch(url);
    const { instance } = WebAssembly.instantiateStreaming
      ? await WebAssembly.instantiateStreaming(res, imports)
      : await WebAssembly.instantiate(await (await res).arrayBuffer(), imports);
    if (typeof mount === "string") {
      if (doc.readyState === "loading") await new Promise((r) => doc.addEventListener("DOMContentLoaded", r, { once: true }));
      mount = doc.getElementById(mount);
    }
    nodes[0] = mount;
    x = instance.exports; mem = x.memory; views();
    const L = x.cd_layout() >> 2;
    [SB, SM, INBOX] = [u32[L], u32[L + 1], u32[L + 2]];
    x.cd_start((Math.random() * 4294967296) >>> 0);
    return x;
  }
  return { start };
})();
