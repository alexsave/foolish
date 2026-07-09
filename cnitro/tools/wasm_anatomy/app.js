(function () {
  'use strict';
  const D = document;
  const el = (t, c, h) => { const e = D.createElement(t); if (c) e.className = c; if (h != null) e.innerHTML = h; return e; };
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const commas = n => n.toLocaleString('en-US');
  const kib = n => (n / 1024).toFixed(n < 10240 ? 2 : 1) + ' KiB';
  const hx = (n, p = 0) => '0x' + (n >>> 0).toString(16).padStart(p, '0');
  const pct = (a, b) => b ? (100 * a / b) : 0;
  const pad = (s, n) => { s = String(s); return s + ' '.repeat(Math.max(1, n - s.length)); };

  const SUBS = {
    engine: 'engine core', codec: 'codec (replay/awire)', bridge: 'wasm bridge',
    strategy: 'bot strategies', runtime: 'runtime / compiler', other: 'other',
  };
  const OPCLASSES = {
    control: 'control flow', call: 'calls', local: 'locals', global: 'globals',
    memory: 'load / store', const: 'constants', i32: 'i32 arithmetic', i64: 'i64 arithmetic',
    float: 'float', param: 'stack (drop/select)', other: 'other',
  };
  const MEMKIND = {
    stack: 'stack', data: 'static data', bss: 'BSS', io: 'IO buffer', game: 'game state',
    moves: 'move buffer', cards: 'card buffers', replay: 'replay scratch', heap: 'heap',
  };
  const SECMEANING = {
    type: 'function signatures', import: 'imported items', function: 'func → type map',
    table: 'indirect-call table', memory: 'linear-memory limits', global: 'mutable/immutable globals',
    export: 'exported entry points', elem: 'table initializer', code: 'all function bodies',
    data: 'initialized static data', custom: 'name / metadata',
  };

  let MODULES = [], META = { title: 'WASM Anatomy', subtitle: '' }, mi = 0, view = 'overview';
  const VIEWS = [
    ['overview', 'Overview'], ['size', 'Size layout'], ['memory', 'Memory layout'],
    ['assembly', 'Annotated assembly'], ['opcodes', 'Opcode census'],
    ['interface', 'Imports · exports'], ['data', 'Data segments'],
  ];

  async function boot() {
    const b64 = D.getElementById('payload').textContent.trim();
    const bin = Uint8Array.from(atob(b64), c => c.charCodeAt(0));
    let text;
    try {
      const ds = new DecompressionStream('gzip');
      text = await new Response(new Blob([bin]).stream().pipeThrough(ds)).text();
    } catch (e) {
      D.getElementById('app').innerHTML = '<div id="boot">This page needs a browser with DecompressionStream (any current Chrome/Safari/Firefox).</div>';
      return;
    }
    const data = JSON.parse(text);
    MODULES = data.modules; META = { title: data.title || 'WASM Anatomy', subtitle: data.subtitle || '' };
    D.title = META.title;
    // restore theme
    const t = localStorage.getItem('wasmviz-theme');
    if (t) D.documentElement.setAttribute('data-theme', t);
    buildShell();
    render();
  }

  function buildShell() {
    const app = D.getElementById('app');
    app.removeAttribute('aria-busy');
    app.innerHTML = '';
    const rail = el('div'); rail.id = 'rail';
    rail.appendChild(el('div', 'brand',
      '<h1>' + esc(META.title) + '</h1>' + (META.subtitle ? '<div class="sub">' + esc(META.subtitle) + '</div>' : '')));
    const pick = el('div', 'modpick');
    MODULES.forEach((m, i) => {
      const b = el('button', 'modbtn' + (i === mi ? ' on' : ''));
      b.innerHTML = '<span class="nm">' + esc(m.human) + '</span><span class="mt">' +
        commas(m.total) + ' B · ' + m.numFuncs + ' fn · ' + kib(m.gz) + ' gz</span>';
      b.onclick = () => { mi = i; view = view; buildShell(); render(); };
      pick.appendChild(b);
    });
    rail.appendChild(pick);
    const nav = el('div', 'nav');
    nav.appendChild(el('div', 'lbl', 'Views'));
    const m = MODULES[mi];
    const counts = {
      overview: '', size: m.sections.length + ' sec', memory: m.memmap.regions.length + ' reg',
      assembly: m.numFuncs + ' fn', opcodes: m.ops.length + ' op',
      interface: (m.numExports) + ' ex', data: m.data.length + ' seg',
    };
    VIEWS.forEach(([k, label]) => {
      const b = el('button', 'navbtn' + (k === view ? ' on' : ''));
      b.innerHTML = '<span>' + label + '</span><span class="ct">' + (counts[k] || '') + '</span>';
      b.onclick = () => { view = k; buildShell(); render(); D.getElementById('main').scrollTop = 0; };
      nav.appendChild(b);
    });
    rail.appendChild(nav);
    const foot = el('div', 'railfoot');
    const tg = el('button', 'themetog', 'Theme');
    tg.onclick = () => {
      const cur = D.documentElement.getAttribute('data-theme');
      const nx = cur === 'dark' ? 'light' : (cur === 'light' ? 'dark' : (matchMedia('(prefers-color-scheme: dark)').matches ? 'light' : 'dark'));
      D.documentElement.setAttribute('data-theme', nx); localStorage.setItem('wasmviz-theme', nx);
    };
    const footText = MODULES.some(x => x.attributed) ? '-Oz · --strip-all' : MODULES.length + (MODULES.length === 1 ? ' module' : ' modules');
    foot.appendChild(el('span', null, '<span style="font-family:ui-monospace,monospace;font-size:10.5px;color:var(--faint)">' + footText + '</span>'));
    foot.appendChild(tg);
    rail.appendChild(foot);
    app.appendChild(rail);
    const main = el('div'); main.id = 'main';
    app.appendChild(main);
  }

  function render() {
    const m = MODULES[mi];
    const main = D.getElementById('main');
    main.innerHTML = '';
    const head = el('div', 'vhead');
    const vlabel = VIEWS.find(v => v[0] === view)[1];
    const note = m.attributed ? 'byte-identical to the shipped artifact · names from a name-preserving companion build'
      : (m.named ? 'symbols recovered from the module’s own name section' : 'stripped module · functions shown by index');
    head.innerHTML = '<h2>' + vlabel + '</h2><span class="crumb">' + esc(m.human) + '</span>' +
      '<span class="note">' + note + '</span>';
    main.appendChild(head);
    const wrap = el('div', 'wrap'); main.appendChild(wrap);
    ({ overview: vOverview, size: vSize, memory: vMemory, assembly: vAssembly, opcodes: vOpcodes, interface: vInterface, data: vData }[view])(m, wrap);
  }

  // ---- readout helper -------------------------------------------------------
  function readout(items) {
    const r = el('div', 'readout');
    items.forEach(([k, v, u]) => r.appendChild(el('div', 'metric',
      '<div class="k">' + esc(k) + '</div><div class="v">' + v + (u ? ' <span class="u">' + esc(u) + '</span>' : '') + '</div>')));
    return r;
  }
  function card(title, hint, desc) {
    const c = el('div', 'card');
    c.appendChild(el('h3', null, esc(title) + (hint ? ' <span class="hint">' + esc(hint) + '</span>' : '')));
    if (desc) c.appendChild(el('p', 'desc', desc));
    return c;
  }
  function legend(entries) { // [ [colorValue, label] ] — colorValue is any CSS color (hex or var(...))
    const l = el('div', 'legend');
    entries.forEach(([cv, lb]) => l.appendChild(el('span', 'lg',
      '<span class="sw" style="background:' + cv + '"></span><b>' + esc(lb) + '</b>')));
    return l;
  }

  // ---- OVERVIEW -------------------------------------------------------------
  function vOverview(m, w) {
    w.appendChild(readout([
      ['binary size', commas(m.total), 'bytes'],
      ['gzipped', kib(m.gz), '(' + pct(m.gz, m.total).toFixed(0) + '%)'],
      ['code section', pct(m.codeSize, m.total).toFixed(0) + '%', kib(m.codeSize)],
      ['functions', String(m.numFuncs), ''],
      ['exports', String(m.numExports), 'entry pts'],
      ['linear memory', (m.memTop / 1048576).toFixed(m.memTop < 1048576 ? 2 : 0) + ' MiB', m.memPages + ' pages'],
    ]));
    if (m.blurb) {
      const c = card('What this module is', null, esc(m.blurb));
      if (m.attributed) {
        const facts = el('div'); facts.style.cssText = 'display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px;margin-top:6px';
        const F = [
          ['Provenance', 'Compiled from the cnitro C sources with clang <code class="inl">-Oz</code>, stripped. This page’s bytes were verified <b>identical</b> to the shipped ' + (m.key === 'bots' ? 'bots.wasm.gz' : m.key + '_wasm.ts') + '.'],
          ['Optimization', 'Size-first (<code class="inl">-Oz</code>), <code class="inl">-mbulk-memory</code>, no LTO, no fast-math. Imports: <b>' + (m.numImports || 'none') + '</b> (freestanding, no libc).'],
          ['Shipping', m.key === 'bots' ? 'A gzip static asset (server-only; bots never run in the browser).' : 'Base64-embedded in a generated .ts, decoded at load — works unchanged in Deno edge, Node and browsers.'],
        ];
        F.forEach(([t, b]) => { const d = el('div', 'card'); d.style.margin = '0'; d.innerHTML = '<h3>' + t + '</h3><p class="desc" style="margin-bottom:0">' + b + '</p>'; facts.appendChild(d); });
        c.appendChild(facts);
      }
      w.appendChild(c);
    }

    // code weight by group (subsystem when a source map exists, else name prefix)
    const groups = m.groups;
    const total = groups.reduce((a, g) => a + g.bytes, 0) || 1;
    const glabel = m.attributed ? 'code bytes by subsystem' : (m.named ? 'code bytes by name prefix' : 'code bytes');
    const gdesc = m.attributed ? 'Every function body attributed to its source file, grouped into subsystems.'
      : (m.named ? 'Functions grouped by the leading token of their symbol name — a plain wasm carries no source map, so this is the best grouping available.'
        : 'This module has no name section, so functions can’t be grouped or named — shown as one bucket.');
    const sc = card('Where the code weight lives', glabel, gdesc);
    const bl = el('div', 'barlist');
    groups.slice(0, 24).forEach(g => bl.appendChild(bar('<span class="sw" style="background:' + g.color + '"></span>' + esc(g.label),
      pct(g.bytes, total), g.color, kib(g.bytes))));
    sc.appendChild(bl);
    w.appendChild(sc);
  }
  function bar(nameHTML, percent, color, valueText) {
    const r = el('div', 'brow');
    r.innerHTML = '<div class="bn">' + nameHTML + '</div><div class="bt"><div class="bf" style="width:' +
      Math.max(1.2, percent) + '%;background:' + color + '"></div></div><div class="bv">' + valueText + '</div>';
    return r;
  }

  // ---- SIZE -----------------------------------------------------------------
  function vSize(m, w) {
    const secSorted = m.sections.slice().sort((a, b) => b.size - a.size);
    const other = m.sections.filter(s => s.name !== 'code' && s.name !== 'data').reduce((a, s) => a + s.size, 0);
    const dataSz = m.sections.filter(s => s.name === 'data').reduce((a, s) => a + s.size, 0);
    w.appendChild(readout([
      ['total', commas(m.total), 'B'],
      ['code', kib(m.codeSize), pct(m.codeSize, m.total).toFixed(0) + '%'],
      ['data', commas(dataSz), 'B'],
      ['metadata & tables', commas(other), 'B'],
      ['sections', String(m.sections.length), ''],
    ]));

    // section treemap
    const tc = card('Binary section map', 'proportional · hover for detail', 'The WASM module is a flat sequence of sections. Width ∝ byte size. <code class="inl">CODE</code> dominates; the <code class="inl">EXPORT</code> and name sections carry the rest.');
    const tm = el('div', 'treemap');
    const row = el('div', 'tmrow'); row.style.setProperty('--h', '132px');
    secSorted.forEach(s => {
      const cell = el('div', 'tmcell');
      cell.style.flex = s.size + ' 1 0'; cell.style.background = 'var(--sec-' + s.name + ', var(--sec-custom))';
      const nm = (s.customName ? s.name + ' “' + s.customName + '”' : s.name).toUpperCase();
      cell.style.minWidth = s.size / m.total > 0.02 ? '' : '3px';
      cell.innerHTML = '<span class="lab">' + esc(s.name.toUpperCase()) + '<small>' + commas(s.size) + ' B · ' + pct(s.size, m.total).toFixed(1) + '%</small></span>';
      cell.title = nm + ' — ' + (SECMEANING[s.name] || '') + ' — ' + commas(s.size) + ' bytes (' + pct(s.size, m.total).toFixed(2) + '%)';
      row.appendChild(cell);
    });
    tm.appendChild(row); tc.appendChild(tm);
    const t = el('table', 't');
    t.innerHTML = '<thead><tr><th>section</th><th>meaning</th><th class="n">bytes</th><th class="n">share</th></tr></thead>';
    const tb = el('tbody');
    secSorted.forEach(s => {
      tb.appendChild(el('tr', null,
        '<td class="m"><span class="sw" style="display:inline-block;width:9px;height:9px;border-radius:2px;margin-right:8px;background:var(--sec-' + s.name + ',var(--sec-custom))"></span>' +
        esc(s.name.toUpperCase()) + (s.customName ? ' <span style="color:var(--faint)">“' + esc(s.customName) + '”</span>' : '') + '</td>' +
        '<td style="color:var(--dim)">' + esc(SECMEANING[s.name] || '—') + '</td>' +
        '<td class="n">' + commas(s.size) + '</td><td class="n">' + pct(s.size, m.total).toFixed(2) + '%</td>'));
    });
    t.appendChild(tb); tc.appendChild(el('div', null, '')); tc.lastChild.style.marginTop = '16px'; tc.lastChild.appendChild(t);
    w.appendChild(tc);

    // code by source file / name group
    if (m.named) {
      const fc = card(m.attributed ? 'Code by source file' : 'Code by name group', 'within the CODE section',
        m.attributed ? 'Each function’s body size summed by the .c file it came from (symbol→file via the object symbol tables), colored by subsystem.'
          : 'Each function’s body size summed by the leading token of its symbol name.');
      fc.appendChild(legend(m.groups.map(g => [g.color, g.label])));
      const bl = el('div', 'barlist');
      m.fileList.forEach(f => {
        bl.appendChild(bar('<span class="sw" style="background:' + f.color + '"></span>' + esc(f.file) + (m.attributed ? '.c' : ''),
          pct(f.bytes, m.codeSize), f.color, kib(f.bytes)));
      });
      fc.appendChild(bl);
      w.appendChild(fc);
    }

    // top functions
    const topc = card('Largest functions', 'top 40 by body size', 'Click a bar to jump to its annotated disassembly.');
    const top = m.funcs.slice().sort((a, b) => b.size - a.size).slice(0, 40);
    const maxSz = top[0].size;
    const bl2 = el('div', 'barlist');
    top.forEach(f => {
      const r = bar('<span class="sw" style="background:' + f.color + '"></span>' + esc(f.name),
        pct(f.size, maxSz), f.color, commas(f.size) + ' B');
      r.style.cursor = 'pointer';
      r.onclick = () => { view = 'assembly'; buildShell(); render(); setTimeout(() => openFn(f.index), 30); };
      bl2.appendChild(r);
    });
    topc.appendChild(bl2);
    w.appendChild(topc);
  }

  // ---- MEMORY ---------------------------------------------------------------
  let memMode = 'linear';
  function vMemory(m, w) {
    const mm = m.memmap;
    const staticEnd = mm.regions.filter(r => r.kind !== 'heap').reduce((mx, r) => Math.max(mx, r.end), 0);
    w.appendChild(readout([
      ['linear memory', (mm.memTop / 1048576).toFixed(2) + ' MiB', m.memPages + ' pages'],
      ['stack', kib(mm.stackSize), '@ 0'],
      ['static footprint', (staticEnd / 1048576).toFixed(2) + ' MiB', 'to __heap_base'],
      ['growable', m.memGrowable ? 'yes' : 'no', 'max = ∞'],
    ]));
    const hasStack = mm.regions.some(r => r.kind === 'stack' && (r.end - r.start) > 0);
    const c = card('Linear memory at module init', 'address 0 → top · scroll down', m.attributed
      ? 'One flat <code class="inl">ArrayBuffer</code>. With <code class="inl">--stack-first</code> the C shadow stack sits at the very bottom and grows <b>down</b>; static data, then the big zero-init engine/bot buffers, then a 2 MiB replay scratch fill the rest. Region sizes are exact; the named buffers are anchored by their <code class="inl">wasm_*_ptr</code> getter constants.'
      : 'One flat <code class="inl">ArrayBuffer</code>' + (hasStack ? '. A mutable i32 global (the LLVM <code class="inl">__stack_pointer</code>) marks the shadow stack at the bottom, growing <b>down</b>; ' : ', with ') + 'the initialized data segments and the buffers above them make up the static footprint. Region sizes are exact.');
    const tools = el('div', 'memtools');
    const seg = el('div', 'seg');
    ['linear', 'log'].forEach(mode => {
      const b = el('button', memMode === mode ? 'on' : '', mode === 'linear' ? 'proportional' : 'log scale');
      b.onclick = () => { memMode = mode; render(); };
      seg.appendChild(b);
    });
    tools.appendChild(seg);
    tools.appendChild(el('span', null, '<span style="font-size:11.5px;color:var(--faint)">' +
      (memMode === 'linear' ? 'heights are true byte proportions — the 2 MiB replay buffer really is that big' : 'heights ∝ log₂(size) so tiny regions stay readable') + '</span>'));
    c.appendChild(tools);
    c.appendChild(legend(Object.keys(MEMKIND).map(k => ['var(--mem-' + k + ')', MEMKIND[k]])));

    const map = el('div', 'memmap');
    const scale = mm.memTop / 2400;
    mm.regions.forEach(r => {
      const size = r.end - r.start;
      let h;
      if (memMode === 'linear') h = Math.max(34, size / scale);
      else h = Math.max(34, 26 * Math.log2(Math.max(2, size)));
      const seg = el('div', 'mseg');
      seg.style.setProperty('--mh', h + 'px');
      seg.style.setProperty('--c', 'var(--mem-' + r.kind + ')');
      seg.innerHTML =
        '<div class="addr"><span class="hi">' + hx(r.start, 6) + '</span><span>' + hx(r.end, 6) + '</span></div>' +
        '<div class="body"><div><div class="ml">' + esc(r.label) + '</div><div class="md">' + r.detail + '</div></div>' +
        '<div class="msz">' + (size >= 1024 ? kib(size) : commas(size) + ' B') + '<br><span style="color:var(--faint)">' + pct(size, mm.memTop).toFixed(1) + '%</span></div></div>';
      map.appendChild(seg);
    });
    c.appendChild(map);
    c.appendChild(el('div', 'mnote', 'Addresses are byte offsets into the module’s single linear memory. The stack pointer global initializes to ' + commas(mm.stackSize) + ' (' + hx(mm.stackSize) + '); everything above __heap_base is reserved but untouched until the module grows memory.'));
    w.appendChild(c);
  }

  // ---- ASSEMBLY -------------------------------------------------------------
  let asmSort = 'index', asmFilter = '', asmSub = 'all';
  function vAssembly(m, w) {
    const c = card('Full annotated disassembly', m.numFuncs + ' functions · expand any', 'Every function body, decoded instruction-by-instruction from the shipped bytes. Columns are fixed: <b>offset</b> (function-relative) │ <b>raw bytes</b> │ <b>instruction</b>. Mnemonics are colored by class; <span style="color:var(--accent)">call targets</span> and memory addresses are resolved in the trailing annotation.');
    const tools = el('div', 'asmtools');
    const search = el('input'); search.placeholder = 'filter functions…'; search.value = asmFilter;
    search.oninput = () => { asmFilter = search.value.toLowerCase(); renderFnList(m, listWrap); };
    tools.appendChild(search);
    const subsel = el('select', 'btn');
    const grpWord = m.attributed ? 'subsystems' : 'name groups';
    subsel.innerHTML = '<option value="all">all ' + grpWord + '</option>' +
      m.groups.map(g => '<option value="' + esc(g.key) + '">' + esc(g.label) + '</option>').join('');
    subsel.value = asmSub; subsel.onchange = () => { asmSub = subsel.value; renderFnList(m, listWrap); };
    if (m.groups.length > 1) tools.appendChild(subsel);
    const sortsel = el('select', 'btn');
    sortsel.innerHTML = '<option value="index">order: file/index</option><option value="size">order: size ↓</option><option value="name">order: name</option>';
    sortsel.value = asmSort; sortsel.onchange = () => { asmSort = sortsel.value; renderFnList(m, listWrap); };
    tools.appendChild(sortsel);
    const exp = el('button', 'btn warn', 'expand all');
    const col = el('button', 'btn', 'collapse all');
    exp.onclick = () => expandAll(listWrap, true);
    col.onclick = () => expandAll(listWrap, false);
    tools.appendChild(exp); tools.appendChild(col);
    c.appendChild(tools);
    const listWrap = el('div', 'fnlist');
    c.appendChild(listWrap);
    w.appendChild(c);
    renderFnList(m, listWrap);
  }
  function currentFns(m) {
    let fns = m.funcs.slice();
    if (asmSub !== 'all') fns = fns.filter(f => f.grp === asmSub);
    if (asmFilter) fns = fns.filter(f => f.name.toLowerCase().includes(asmFilter) || f.file.includes(asmFilter));
    if (asmSort === 'size') fns.sort((a, b) => b.size - a.size);
    else if (asmSort === 'name') fns.sort((a, b) => a.name.localeCompare(b.name));
    else fns.sort((a, b) => a.index - b.index);
    return fns;
  }
  function renderFnList(m, wrap) {
    wrap.innerHTML = '';
    const fns = currentFns(m);
    if (!fns.length) { wrap.appendChild(el('div', null, '<p class="desc">No functions match.</p>')); return; }
    const frag = D.createDocumentFragment();
    fns.forEach(f => frag.appendChild(fnNode(f)));
    wrap.appendChild(frag);
  }
  function sigText(f) {
    const p = (f.sig.params || []).join(', ');
    const r = (f.sig.results || []).join(', ');
    return '(' + p + ')' + (r ? ' → ' + r : '');
  }
  function fnNode(f) {
    const m = MODULES[mi];
    const d = el('details', 'fn'); d.dataset.idx = f.index;
    const s = el('summary');
    s.innerHTML =
      '<span class="idx">#' + f.index + '</span>' +
      '<span class="fname">' + esc(f.name) + ' <span class="fsig">' + esc(sigText(f)) + '</span></span>' +
      (m.attributed || f.file !== 'code'
        ? '<span class="fsub" style="color:' + f.color + ';border-color:' + f.color + '">' + esc(f.file) + (m.attributed ? '.c' : '') + '</span>'
        : '') +
      '<span class="fsz">' + commas(f.size) + ' B · ' + f.ins.length + ' ins</span>';
    d.appendChild(s);
    d.addEventListener('toggle', () => {
      if (d.open && !d.dataset.built) { d.dataset.built = '1'; buildBody(d, f); }
    });
    return d;
  }
  function buildBody(d, f) {
    const locals = (f.locals || []).map(l => l.count + '×' + l.type).join(', ') || 'none';
    d.appendChild(el('div', 'fmeta', 'locals: ' + locals + '  │  ' + f.localCount + ' slots  │  body ' + commas(f.size) + ' B'));
    const asm = el('div', 'asm');
    asm.innerHTML = renderAsm(f);
    d.appendChild(asm);
  }
  function renderAsm(f) {
    let out = '';
    for (const ins of f.ins) {
      const ind = '  '.repeat(ins.d || 0);
      const mn = '<span class="ic ' + ins.k + '">' + esc(ins.t) + '</span>';
      let rest = ind + mn;
      if (ins.a) rest += ' '.repeat(Math.max(1, 15 - ins.t.length - ins.d * 2)) + '<span class="oa">' + esc(ins.a) + '</span>';
      if (ins.c) {
        const cc = ins.c.startsWith('→') ? 'cm callann' : 'cm';
        rest += '   <span class="' + cc + '">; ' + esc(ins.c) + '</span>';
      }
      const bytesSp = ins.b.replace(/(..)/g, '$1 ').trim();
      out += '<div class="ln"><span class="o">' + (ins.o).toString(16).padStart(4, '0') + '</span>' +
        '<span class="hx" title="' + bytesSp + '">' + bytesSp + '</span>' +
        '<span class="ins">' + rest + '</span></div>';
    }
    return out;
  }
  function expandAll(wrap, open) {
    const items = [...wrap.querySelectorAll('details.fn')];
    if (open && items.length > 120 && !confirm('Expand all ' + items.length + ' functions? This renders every instruction at once and may pause briefly.')) return;
    let i = 0;
    (function step() {
      const t0 = performance.now();
      while (i < items.length && performance.now() - t0 < 40) { items[i].open = open; i++; }
      if (i < items.length) requestAnimationFrame(step);
    })();
  }
  function openFn(idx) {
    const node = D.querySelector('details.fn[data-idx="' + idx + '"]');
    if (node) { node.open = true; node.scrollIntoView({ block: 'center' }); node.querySelector('summary').style.outline = '2px solid var(--accent)'; setTimeout(() => node.querySelector('summary').style.outline = '', 1400); }
  }

  // ---- OPCODES --------------------------------------------------------------
  function vOpcodes(m, w) {
    w.appendChild(readout([
      ['distinct opcodes', String(m.ops.length), 'of ~180'],
      ['total instructions', commas(m.opTotal), ''],
      ['most common', m.ops[0].t, commas(m.ops[0].n)],
      ['bytes / instr', (m.codeSize / m.opTotal).toFixed(2), 'avg'],
    ]));
    // by class
    const classTot = {};
    m.ops.forEach(o => classTot[o.k] = (classTot[o.k] || 0) + o.n);
    const cc = card('Instruction mix by class', 'share of all ' + commas(m.opTotal) + ' instructions', 'What the compiler actually emitted. Load/store and local access dominate — the fingerprint of struct-heavy engine code lowered at ' + '-Oz.');
    cc.appendChild(legend(Object.keys(OPCLASSES).filter(k => classTot[k]).map(k => ['var(--op-' + k + ')', OPCLASSES[k]])));
    const bl = el('div', 'barlist');
    Object.entries(classTot).sort((a, b) => b[1] - a[1]).forEach(([k, n]) => {
      bl.appendChild(bar('<span class="sw" style="background:var(--op-' + k + ')"></span>' + OPCLASSES[k],
        pct(n, m.opTotal), 'var(--op-' + k + ')', commas(n)));
    });
    cc.appendChild(bl);
    w.appendChild(cc);

    const gc = card('Every opcode, by frequency', m.ops.length + ' distinct', null);
    const grid = el('div', 'opgrid');
    const max = m.ops[0].n;
    m.ops.forEach(o => {
      const cell = el('div', 'opcell');
      cell.innerHTML = '<span class="sw" style="background:var(--op-' + o.k + ')"></span>' +
        '<span class="on2">' + esc(o.t) + '</span><span class="oc">' + commas(o.n) + '</span>' +
        '<span class="opbar"><i style="width:' + pct(o.n, max) + '%;background:var(--op-' + o.k + ')"></i></span>';
      grid.appendChild(cell);
    });
    gc.appendChild(grid);
    w.appendChild(gc);
  }

  // ---- INTERFACE ------------------------------------------------------------
  function vInterface(m, w) {
    w.appendChild(readout([
      ['exports', String(m.numExports), ''],
      ['imports', String(m.numImports), m.numImports ? '' : 'none'],
      ['globals', String(m.globals.length), ''],
      ['indirect table', m.table ? commas(m.table[0].min) + ' slots' : '—', ''],
    ]));
    const ic = card('Imports', 'the host surface', 'What the module needs from the host at instantiation.');
    if (!m.imports.length) {
      ic.appendChild(el('p', 'lead', m.attributed
        ? 'None. The kernel is compiled <code class="inl">-nostdlib -ffreestanding</code> — it imports nothing, not even memory (it exports its own). Every dependency, down to <code class="inl">memcpy</code>/<code class="inl">memset</code>, is compiled in.'
        : 'None — this module is fully self-contained and imports nothing from the host.'));
    } else {
      const it = el('table', 't');
      it.innerHTML = '<thead><tr><th>module</th><th>name</th><th>kind</th><th>type</th></tr></thead>';
      const itb = el('tbody');
      m.imports.forEach(im => {
        let ty = '';
        if (im.kind === 'func') ty = 'type #' + im.typeIndex;
        else if (im.kind === 'mem') ty = 'min ' + im.limits?.min + (im.limits?.max != null ? ' / max ' + im.limits.max : '') + ' pages';
        else if (im.kind === 'global') ty = (im.mut ? 'mut ' : '') + im.valtype;
        else if (im.kind === 'table') ty = im.et + ' × ' + im.limits?.min;
        itb.appendChild(el('tr', null,
          '<td class="m" style="color:var(--dim)">' + esc(im.module) + '</td>' +
          '<td class="m" style="color:var(--accent)">' + esc(im.field) + '</td>' +
          '<td><span class="tag" style="color:var(--sec-import)">' + esc(im.kind) + '</span></td>' +
          '<td class="m" style="color:var(--dim)">' + esc(ty) + '</td>'));
      });
      it.appendChild(itb);
      const isc = el('div'); isc.style.cssText = 'max-height:360px;overflow:auto;border:1px solid var(--line);border-radius:8px'; isc.appendChild(it);
      ic.appendChild(isc);
    }
    w.appendChild(ic);

    const byIdx = {}; m.funcs.forEach(f => byIdx[f.index] = f);
    // a pure constant getter: body is just `<const>; end` with no side effects
    const constGetter = f => f && f.ins.length === 2 && f.ins[1].t === 'end' && /\.const$/.test(f.ins[0].t) ? f.ins[0].a : null;
    const ec = card('Exports', m.numExports + ' entry points', m.attributed
      ? 'The explicit allow-list the Makefile passes to <code class="inl">wasm-ld</code> — only the memory plus the <code class="inl">wasm_*</code> API the TS bridge calls. Pure address getters resolve to a fixed linear-memory constant.'
      : 'Everything this module exposes to the host. Zero-argument functions that just return a constant are resolved to that value.');
    const t = el('table', 't');
    t.innerHTML = '<thead><tr><th>export</th><th>kind</th><th class="n">index</th><th>resolves to</th></tr></thead>';
    const tb = el('tbody');
    m.exports.forEach(x => {
      let resolve = '';
      if (x.kind === 'mem') resolve = 'linear memory — ' + m.memPages + ' pages';
      else if (x.kind === 'global') { const g = m.globals[x.index - m.imports.filter(i => i.kind === 'global').length]; if (g && g.initConst != null) resolve = hx(g.initConst); }
      else if (x.kind === 'func') {
        const cv = constGetter(byIdx[x.index]);
        if (cv != null) { const v = parseInt(cv, 10); resolve = /_cap$|_size$|_len$/.test(x.name) ? commas(v) + ' bytes' : hx(v) + ' <span style="color:var(--faint)">(' + commas(v) + ')</span>'; }
      }
      tb.appendChild(el('tr', null,
        '<td class="m" style="color:var(--accent)">' + esc(x.name) + '</td>' +
        '<td><span class="tag" style="color:var(--sec-export)">' + esc(x.kind) + '</span></td>' +
        '<td class="n">' + x.index + '</td>' +
        '<td class="m" style="color:var(--dim)">' + resolve + '</td>'));
    });
    t.appendChild(tb);
    const sc = el('div'); sc.style.cssText = 'max-height:520px;overflow:auto;border:1px solid var(--line);border-radius:8px'; sc.appendChild(t);
    ec.appendChild(sc);
    w.appendChild(ec);

    const gc = card('Globals & indirect table', null, null);
    const gt = el('table', 't');
    gt.innerHTML = '<thead><tr><th>global</th><th>type</th><th>mutable</th><th class="n">init</th></tr></thead>';
    const gtb = el('tbody');
    m.globals.forEach((g, i) => gtb.appendChild(el('tr', null,
      '<td class="m">' + (i === 0 ? '__stack_pointer' : '#' + g.index) + '</td>' +
      '<td class="m">' + g.valtype + '</td><td style="color:var(--dim)">' + (g.mut ? 'yes' : 'no') + '</td>' +
      '<td class="n">' + (g.initConst != null ? hx(g.initConst) + ' · ' + commas(g.initConst) : '—') + '</td>')));
    gt.appendChild(gtb); gc.appendChild(gt);
    if (m.table) gc.appendChild(el('p', 'desc', '<br>Indirect-call table (<code class="inl">funcref</code>): ' + commas(m.table[0].min) + ' slots — the address-taken strategy/hook function pointers dispatched via <code class="inl">call_indirect</code>.'));
    w.appendChild(gc);
  }

  // ---- DATA -----------------------------------------------------------------
  function vData(m, w) {
    w.appendChild(card('Static data segments', m.data.length + ' segment' + (m.data.length > 1 ? 's' : ''), 'The only bytes materialized into linear memory at load (everything else is zero-init BSS). Address │ hex │ ASCII, 16 bytes per row. Printable runs are the module’s only string/rodata.'));
    m.data.forEach((seg, si) => {
      const bytes = Uint8Array.from(atob(seg.b64), ch => ch.charCodeAt(0));
      const h = el('div', 'card'); h.style.margin = '0 0 16px';
      h.appendChild(el('h3', null, 'segment ' + si + ' <span class="hint">@ ' + hx(seg.memOffset) + ' · ' + commas(seg.size) + ' bytes</span>'));
      const hex = el('div', 'hex');
      hex.appendChild(el('div', 'hexhdr', '<span>offset</span><span>bytes</span><span>ascii</span>'));
      const inner = el('div');
      let html = '';
      for (let off = 0; off < bytes.length; off += 16) {
        const slice = bytes.slice(off, off + 16);
        let hexs = '', asc = '';
        for (let j = 0; j < 16; j++) {
          if (j < slice.length) {
            const b = slice[j];
            hexs += (b === 0 ? '<span class="z">00</span>' : b.toString(16).padStart(2, '0')) + (j === 7 ? '  ' : ' ');
            asc += (b >= 32 && b < 127) ? '<span class="hc">' + esc(String.fromCharCode(b)) + '</span>' : '<span class="z">.</span>';
          } else { hexs += '   '; }
        }
        html += '<div class="hl"><span class="ha">' + hx(seg.memOffset + off, 6) + '</span><span class="hb">' + hexs + '</span><span class="hb">' + asc + '</span></div>';
      }
      inner.innerHTML = html; hex.appendChild(inner); h.appendChild(hex);
      if (m.key === 'bots' && si === 0) h.appendChild(el('p', 'desc', '<br>These NUL-terminated strings are the bot env-flag keys (<code class="inl">getenv</code> lookups): <code class="inl">prod</code>, <code class="inl">OG_REPLY</code>, <code class="inl">SX_VERIFY</code>, tuning knobs read by octogen/semtex/cordite at choose-move time.'));
      w.appendChild(h);
    });
  }

  boot();
})();
