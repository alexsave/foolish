// Build the interactive WASM anatomy presentation from the analyzed module JSON.
// Derives source attribution + linear-memory map in Node, then emits a single
// self-contained HTML file with the module data embedded as gzip+base64.
import fs from 'node:fs';
import zlib from 'node:zlib';

const DIR = process.argv[2] || '.';
const OUT = process.argv[3] || `${DIR}/wasm_anatomy.html`;
// Config drives everything (module list, labels, optional enrichment). This is
// what makes the tool generic: `analyze.mjs` + this script work on ANY wasm; the
// cnitro-specific richness (source-file attribution, module blurbs) is just
// extra fields in the config, absent for a plain wasm.
//   CONFIG = { title, subtitle, symfile?, modules:[{key, human, blurb?, wasm}] }
const CONFIG = JSON.parse(fs.readFileSync(process.argv[4] || `${DIR}/config.json`, 'utf8'));

// Node-side copy of the categorical palette so attribution colors can be baked
// into the payload (keeps the client CSS free of module-specific classes).
const SUB_HEX = { engine:'#4fb3cf', codec:'#e07bb0', bridge:'#6f97f0', strategy:'#b58ae6', runtime:'#8b98ac', other:'#6b7686' };
const SUB_LABEL = { engine:'engine core', codec:'codec (replay/awire)', bridge:'wasm bridge', strategy:'bot strategies', runtime:'runtime / compiler', other:'other' };
const PALETTE = ['#4fb3cf','#e07bb0','#6f97f0','#b58ae6','#8fce7a','#e0b23b','#e08a5a','#6fd0b0','#d0d060','#d17a6a','#9aa7b8','#7fb069','#c98bdb','#5aa9e0','#d99a4e','#78c2a4'];

// ---- source attribution (optional; only when the config points at a symfile) -
let sym2file = null;
if (CONFIG.symfile && fs.existsSync(CONFIG.symfile)) {
  sym2file = {};
  for (const line of fs.readFileSync(CONFIG.symfile, 'utf8').split('\n')) {
    if (!line.trim()) continue;
    const [sym, file] = line.split('\t');
    if (!(sym in sym2file) || (sym2file[sym].startsWith('wasm_') && !file.startsWith('wasm_'))) sym2file[sym] = file;
  }
}
const FILE_SUB = {
  game:'engine', legal:'engine', deal_rng:'engine', view:'engine', replay:'codec',
  awire:'codec', evwire:'codec',
  wasm_api:'bridge', wasm_bots_api:'bridge', wasm_guards_api:'bridge',
};
const STRAT_FILES = ['random_strategy','espresso_strategy','espresso_prod_strategy','handwritten_strategy','handwritten_prod_strategy','simple_heuristic_strategy','champion_strategy','ultimate_champion_strategy','hacker_strategy','fulminate_strategy','cordite_strategy','cordite_sim','semtex_strategy','octogen_strategy'];
for (const f of STRAT_FILES) FILE_SUB[f] = 'strategy';

function attributeSym(name) {
  let file = sym2file[name];
  if (!file && name.startsWith('wasm_')) file = 'wasm_api';
  if (!file) {
    if (/^__|memcpy|memset|memmove|memcmp/.test(name)) return { file: 'runtime', sub: 'runtime' };
    return { file: 'other', sub: 'runtime' };
  }
  return { file, sub: FILE_SUB[file] || 'other' };
}

// ---- memory map -------------------------------------------------------------
const PAGE = 65536;
function u(n){ return n; }
function buildMemMap(m) {
  // __stack_pointer is conventionally global 0 for LLVM output, but only when
  // it is a mutable i32; on a wasm without it we simply have no stack region.
  const sp = m.globals.find(g => g.mut && g.valtype === 'i32' && g.initConst != null);
  const stackSize = (m.globals[0]?.index === 0 && m.globals[0]?.mut ? m.globals[0].initConst : sp?.initConst) ?? 0;
  const memMin = m.memory?.[0]?.min ?? m.imports.find(i => i.kind === 'mem')?.limits?.min ?? 0;
  const memTop = memMin * PAGE;
  const p = m.ptrConsts || {};
  const dataEnd = m.data.reduce((mx,d)=>Math.max(mx, d.memOffset + d.size), stackSize);
  // authoritative anchored buffers (pure getter exports only)
  const anchors = [];
  const add = (start, size, label, detail, kind) => { if (start>0) anchors.push({ start, size, label, detail, kind }); };
  if (p.wasm_io_ptr) add(p.wasm_io_ptr, p.wasm_io_cap||0, 'g_io', 'TS ⇄ kernel IO buffer · IO_CAP = 72 KiB', 'io');
  if (p.wasm_game_ptr_internal) {
    const gEnd = p.wasm_moves_ptr_internal || (p.wasm_game_ptr_internal);
    add(p.wasm_game_ptr_internal, (gEnd>p.wasm_game_ptr_internal?gEnd:p.wasm_game_ptr_internal+0) - p.wasm_game_ptr_internal, 'g_game + g_snaps[48]', 'live Game struct + snapshot ring (MAX_SNAPS = 48)', 'game');
  }
  if (p.wasm_moves_ptr_internal) {
    const mEnd = p.wasm_cards_a_ptr || p.wasm_moves_ptr_internal;
    add(p.wasm_moves_ptr_internal, Math.max(0, mEnd - p.wasm_moves_ptr_internal), 'g_moves', `LegalMoves menu buffer (MAX_LEGAL_MOVES)`, 'moves');
  }
  if (p.wasm_cards_a_ptr) {
    const cStart = p.wasm_cards_a_ptr;
    const cEnd = p.wasm_replay_io_ptr && p.wasm_replay_io_ptr>cStart ? Math.min(p.wasm_cards_b_ptr+128+512, p.wasm_replay_io_ptr) : (p.wasm_cards_b_ptr? p.wasm_cards_b_ptr+128 : cStart+256);
    add(cStart, Math.max(256, (p.wasm_cards_b_ptr? p.wasm_cards_b_ptr+128:cStart+256)-cStart), 'g_in_raw_a/b · g_in_a/b', 'action-card decode buffers (MAX_IN_CARDS = 128)', 'cards');
  }
  if (p.wasm_replay_io_ptr) add(p.wasm_replay_io_ptr, p.wasm_replay_io_cap||0, 'replay_io', 'replay-codec scratch buffer (2 MiB cap)', 'replay');

  anchors.sort((a,b)=>a.start-b.start);
  // assemble ordered regions with gap-fill
  const regions = [];
  regions.push({ start:0, end:stackSize, label:'shadow stack', detail:'C call stack — grows downward from '+stackSize.toLocaleString()+' (--stack-first)', kind:'stack' });
  // static data
  for (const d of m.data) regions.push({ start:d.memOffset, end:d.memOffset+d.size, label:'static data / rodata', detail:`initialized data segment · ${d.size.toLocaleString()} B`, kind:'data' });
  let cursor = dataEnd;
  for (const a of anchors) {
    if (a.start > cursor + 8) regions.push({ start:cursor, end:a.start, label:'BSS — static scratch', detail:'zero-initialized engine/bot buffers (env, logs, belief & sim scratch)', kind:'bss' });
    regions.push({ start:a.start, end:a.start + Math.max(a.size,1), label:a.label, detail:a.detail, kind:a.kind });
    cursor = a.start + Math.max(a.size,1);
  }
  if (memTop > cursor + 8) regions.push({ start:cursor, end:memTop, label:'heap / headroom', detail:'above __heap_base — unused at init; memory is growable (max = ∞)', kind:'heap' });
  // clip & sort
  regions.sort((a,b)=>a.start-b.start || a.end-b.end);
  return { stackSize, memTop, dataEnd, regions };
}

// ---- opcode classes ---------------------------------------------------------
function opClass(mn){
  if (['block','loop','if','else','end','br','br_if','br_table','return','unreachable','nop'].includes(mn)) return 'control';
  if (mn==='call'||mn==='call_indirect') return 'call';
  if (mn.startsWith('local.')) return 'local';
  if (mn.startsWith('global.')) return 'global';
  if (mn.startsWith('memory.')||mn.includes('.load')||mn.includes('.store')) return 'memory';
  if (mn.endsWith('.const')) return 'const';
  if (mn.startsWith('i32.')) return 'i32';
  if (mn.startsWith('i64.')) return 'i64';
  if (mn.startsWith('f32.')||mn.startsWith('f64.')) return 'float';
  if (mn==='drop'||mn==='select') return 'param';
  return 'other';
}

// ---- assemble per-module view model ----------------------------------------
function buildModule(spec) {
  const m = JSON.parse(fs.readFileSync(`${DIR}/${spec.key}.json`, 'utf8'));
  const attributed = !!sym2file;                                    // source-file map present
  const named = m.funcs.some(f => !/^func\[\d+\]$/.test(f.name));   // wasm carries a name section

  // Assign each function a group + a baked-in color. With a symfile we use the
  // cnitro subsystem taxonomy; otherwise we group by the leading token of the
  // (name-section) symbol, or fall back to a single "code" bucket for a wasm
  // with no names at all.
  const fileTotals = {}, grpTotals = {}, grpColor = {}, fileColor = {};
  let nextColor = 0;
  const colorForKey = k => (grpColor[k] || (grpColor[k] = PALETTE[nextColor++ % PALETTE.length]));
  for (const f of m.funcs) {
    let file, grp, color;
    if (attributed) {
      const a = attributeSym(f.name); file = a.file; grp = a.sub; color = SUB_HEX[a.sub] || SUB_HEX.other; grpColor[grp] = color;
    } else if (named) {
      file = (f.name.split(/[_.<[(]/)[0] || 'anon'); grp = file; color = colorForKey(grp);
    } else {
      file = 'code'; grp = 'code'; color = colorForKey('code');
    }
    f.file = file; f.grp = grp; f.color = color;
    fileTotals[file] = (fileTotals[file] || 0) + f.size;
    grpTotals[grp] = (grpTotals[grp] || 0) + f.size;
    fileColor[file] = color;
    for (const ins of f.ins) ins.k = opClass(ins.t);
  }
  const groups = Object.entries(grpTotals).sort((a, b) => b[1] - a[1])
    .map(([g, bytes]) => ({ key: g, label: attributed ? (SUB_LABEL[g] || g) : g, color: grpColor[g], bytes }));
  const fileList = Object.entries(fileTotals).sort((a, b) => b[1] - a[1])
    .map(([file, bytes]) => ({ file, bytes, color: fileColor[file], grp: (m.funcs.find(f => f.file === file) || {}).grp }));

  const codeSize = m.sections.find(s => s.name === 'code')?.size || 0;
  const memmap = buildMemMap(m);
  const ops = Object.entries(m.opcodes).map(([t, n]) => ({ t, n, k: opClass(t) })).sort((a, b) => b.n - a.n);
  const opTotal = ops.reduce((a, o) => a + o.n, 0);
  const raw = fs.readFileSync(spec.wasm);
  const gz = zlib.gzipSync(raw, { level: 9 }).length;
  // memory limits may come from an imported memory, not the memory section
  const impMem = m.imports.find(i => i.kind === 'mem');
  const memDef = m.memory?.[0] || impMem?.limits || null;
  return {
    key: spec.key, human: spec.human || spec.key, blurb: spec.blurb || '',
    attributed, named, attrLabel: attributed ? 'source file' : (named ? 'name prefix' : 'unnamed'),
    total: m.total, gz, codeSize,
    numFuncs: m.funcs.length, numImports: m.imports.length, numExports: m.exports.length,
    memPages: memmap.memTop / 65536, memTop: memmap.memTop, memGrowable: memDef ? (memDef.max == null) : true, memImported: !!impMem,
    sections: m.sections, funcs: m.funcs, ops, opTotal,
    imports: m.imports, exports: m.exports, globals: m.globals, table: m.table,
    data: m.data, ptrConsts: m.ptrConsts, customs: m.customs,
    fileTotals, groups, fileList, memmap,
  };
}

const MODULES = CONFIG.modules.map(buildModule);
const payloadObj = { title: CONFIG.title || 'WASM Anatomy', subtitle: CONFIG.subtitle || '', modules: MODULES };
const payload = zlib.gzipSync(Buffer.from(JSON.stringify(payloadObj)), { level: 9 }).toString('base64');
console.log('payload gz+b64:', (payload.length / 1e6).toFixed(2), 'MB');

// ============================================================================
function renderHTML(b64, title) {
  // NOTE: charset must be declared for the standalone file:// case — without it
  // the browser falls back to latin-1 and mangles every → │ ⇄ × ∞ · ’ in the UI.
  return String.raw`<meta charset="utf-8">
<title>${title.replace(/[<&]/g, c => ({ '<': '&lt;', '&': '&amp;' }[c]))}</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>${CSS}</style>
<div id="app" aria-busy="true">
  <div id="boot">decompressing module data…</div>
</div>
<script id="payload" type="application/octet-stream">${b64}</script>
<script>${JS}</script>`;
}

const CSS = String.raw`
:root{
  --bg:#0b0e13; --panel:#11151d; --panel2:#161b25; --line:#232b38; --line2:#2e3746;
  --text:#c9d3e0; --dim:#8391a6; --faint:#5b6678; --bright:#eef3fa;
  --accent:#f2a63b; --accent-dim:#a9752a;
  --sub-engine:#4fb3cf; --sub-codec:#e07bb0; --sub-bridge:#6f97f0; --sub-strategy:#b58ae6; --sub-runtime:#8b98ac; --sub-other:#6b7686;
  --mem-stack:#d17a6a; --mem-data:#e0b23b; --mem-bss:#4a5566; --mem-io:#4fb3cf; --mem-game:#8fce7a; --mem-moves:#6f97f0; --mem-cards:#e08a5a; --mem-replay:#b58ae6; --mem-heap:#333c49;
  --sec-code:#4fb3cf; --sec-data:#e0b23b; --sec-type:#b58ae6; --sec-export:#8fce7a; --sec-function:#6f97f0; --sec-elem:#e07bb0; --sec-table:#e08a5a; --sec-memory:#6fd0b0; --sec-global:#d0d060; --sec-custom:#8b98ac; --sec-import:#d17a6a;
  --op-control:#b58ae6; --op-call:#e07bb0; --op-local:#93a1b5; --op-global:#d0d060; --op-memory:#6f97f0; --op-const:#e0b23b; --op-i32:#8fce7a; --op-i64:#6fd0b0; --op-float:#e08a5a; --op-param:#a7b2c2; --op-other:#6b7686;
  --shadow:0 6px 24px rgba(0,0,0,.4);
}
:root[data-theme="light"], :root:not([data-theme="dark"]){}
@media (prefers-color-scheme: light){
  :root{
    --bg:#f3f1ea; --panel:#faf9f5; --panel2:#eeece3; --line:#dcd8cc; --line2:#c9c4b4;
    --text:#2b2f38; --dim:#5f6675; --faint:#8a8f9c; --bright:#12151b;
    --accent:#b9741a; --accent-dim:#8a5714;
    --sub-engine:#2b7f97; --sub-codec:#b0447f; --sub-bridge:#3a5fc0; --sub-strategy:#7b4fbf; --sub-runtime:#5f6b7d; --sub-other:#7a8494;
    --mem-stack:#b8503c; --mem-data:#9a7415; --mem-bss:#b7b3a4; --mem-io:#2b7f97; --mem-game:#4f8f3a; --mem-moves:#3a5fc0; --mem-cards:#b05e2a; --mem-replay:#7b4fbf; --mem-heap:#cfcabb;
    --sec-code:#2b7f97; --sec-data:#9a7415; --sec-type:#7b4fbf; --sec-export:#4f8f3a; --sec-function:#3a5fc0; --sec-elem:#b0447f; --sec-table:#b05e2a; --sec-memory:#2b8f74; --sec-global:#8a8a20; --sec-custom:#5f6b7d; --sec-import:#b8503c;
    --op-control:#7b4fbf; --op-call:#b0447f; --op-local:#5f6b7d; --op-global:#8a8a20; --op-memory:#3a5fc0; --op-const:#9a7415; --op-i32:#4f8f3a; --op-i64:#2b8f74; --op-float:#b05e2a; --op-param:#6b7686; --op-other:#8a8f9c;
    --shadow:0 6px 20px rgba(80,70,40,.14);
  }
}
:root[data-theme="dark"]{
  --bg:#0b0e13; --panel:#11151d; --panel2:#161b25; --line:#232b38; --line2:#2e3746;
  --text:#c9d3e0; --dim:#8391a6; --faint:#5b6678; --bright:#eef3fa; --accent:#f2a63b; --accent-dim:#a9752a;
  --mem-bss:#4a5566; --mem-heap:#333c49;
}
:root[data-theme="light"]{
  --bg:#f3f1ea; --panel:#faf9f5; --panel2:#eeece3; --line:#dcd8cc; --line2:#c9c4b4;
  --text:#2b2f38; --dim:#5f6675; --faint:#8a8f9c; --bright:#12151b; --accent:#b9741a; --accent-dim:#8a5714;
  --sub-engine:#2b7f97; --sub-codec:#b0447f; --sub-bridge:#3a5fc0; --sub-strategy:#7b4fbf; --sub-runtime:#5f6b7d; --sub-other:#7a8494;
  --mem-stack:#b8503c; --mem-data:#9a7415; --mem-bss:#b7b3a4; --mem-io:#2b7f97; --mem-game:#4f8f3a; --mem-moves:#3a5fc0; --mem-cards:#b05e2a; --mem-replay:#7b4fbf; --mem-heap:#cfcabb;
  --sec-code:#2b7f97; --sec-data:#9a7415; --sec-type:#7b4fbf; --sec-export:#4f8f3a; --sec-function:#3a5fc0; --sec-elem:#b0447f; --sec-table:#b05e2a; --sec-memory:#2b8f74; --sec-global:#8a8a20; --sec-custom:#5f6b7d; --sec-import:#b8503c;
  --op-control:#7b4fbf; --op-call:#b0447f; --op-local:#5f6b7d; --op-global:#8a8a20; --op-memory:#3a5fc0; --op-const:#9a7415; --op-i32:#4f8f3a; --op-i64:#2b8f74; --op-float:#b05e2a; --op-param:#6b7686; --op-other:#8a8f9c;
}
*{box-sizing:border-box}
html,body{margin:0;height:100%}
body{background:var(--bg);color:var(--text);font:14px/1.5 ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-font-smoothing:antialiased}
.mono{font-family:ui-monospace,"SF Mono","JetBrains Mono","Menlo","Consolas",monospace}
#boot{padding:40px;color:var(--dim);font-family:ui-monospace,monospace}
#app{height:100vh;display:grid;grid-template-columns:236px 1fr;overflow:hidden}
a{color:inherit}
::selection{background:color-mix(in srgb, var(--accent) 40%, transparent)}

/* sidebar */
#rail{background:var(--panel);border-right:1px solid var(--line);display:flex;flex-direction:column;overflow-y:auto;min-height:0}
.brand{padding:16px 16px 12px;border-bottom:1px solid var(--line)}
.brand h1{margin:0;font-size:15px;letter-spacing:.02em;color:var(--bright);font-weight:650}
.brand .sub{font-size:11px;color:var(--faint);margin-top:3px;font-family:ui-monospace,monospace}
.modpick{display:flex;flex-direction:column;gap:6px;padding:12px}
.modbtn{text-align:left;background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:9px 11px;cursor:pointer;color:var(--text);display:flex;flex-direction:column;gap:2px;transition:border-color .12s,background .12s}
.modbtn:hover{border-color:var(--line2)}
.modbtn.on{border-color:var(--accent);background:color-mix(in srgb,var(--accent) 10%,var(--panel2))}
.modbtn .nm{font-family:ui-monospace,monospace;font-size:13px;color:var(--bright);font-weight:600}
.modbtn .mt{font-size:10.5px;color:var(--dim);font-variant-numeric:tabular-nums}
.modbtn.on .mt{color:var(--accent)}
.nav{display:flex;flex-direction:column;padding:8px;gap:1px;border-top:1px solid var(--line);margin-top:4px}
.nav .lbl{font-size:10px;text-transform:uppercase;letter-spacing:.12em;color:var(--faint);padding:8px 10px 4px}
.navbtn{text-align:left;background:none;border:0;border-radius:6px;padding:7px 10px;cursor:pointer;color:var(--dim);font-size:13px;display:flex;justify-content:space-between;align-items:center;gap:8px}
.navbtn:hover{background:var(--panel2);color:var(--text)}
.navbtn.on{background:var(--panel2);color:var(--bright);box-shadow:inset 2px 0 0 var(--accent)}
.navbtn .ct{font-family:ui-monospace,monospace;font-size:10.5px;color:var(--faint);font-variant-numeric:tabular-nums}
.railfoot{margin-top:auto;padding:12px;border-top:1px solid var(--line);display:flex;align-items:center;gap:8px;justify-content:space-between}
.themetog{background:var(--panel2);border:1px solid var(--line);border-radius:6px;color:var(--dim);padding:5px 9px;cursor:pointer;font-size:12px}
.themetog:hover{color:var(--text);border-color:var(--line2)}

/* main */
#main{overflow-y:auto;min-height:0;position:relative;scroll-behavior:smooth}
.wrap{max-width:1180px;margin:0 auto;padding:28px 32px 120px}
.vhead{position:sticky;top:0;z-index:20;background:linear-gradient(var(--bg) 78%,transparent);backdrop-filter:blur(3px);padding:18px 32px 12px;border-bottom:1px solid var(--line);display:flex;align-items:baseline;gap:14px;flex-wrap:wrap}
.vhead h2{margin:0;font-size:18px;color:var(--bright);letter-spacing:.01em}
.vhead .crumb{font-family:ui-monospace,monospace;font-size:12px;color:var(--accent)}
.vhead .note{font-size:12px;color:var(--dim);margin-left:auto}

/* readout metric strip */
.readout{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:1px;background:var(--line);border:1px solid var(--line);border-radius:10px;overflow:hidden;margin-bottom:24px}
.metric{background:var(--panel);padding:13px 15px}
.metric .k{font-size:10.5px;text-transform:uppercase;letter-spacing:.08em;color:var(--faint)}
.metric .v{font-family:ui-monospace,monospace;font-size:20px;color:var(--bright);font-variant-numeric:tabular-nums;margin-top:3px}
.metric .u{font-size:11px;color:var(--dim)}

.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:20px 22px;margin-bottom:22px}
.card h3{margin:0 0 4px;font-size:14px;color:var(--bright);letter-spacing:.01em}
.card .desc{font-size:12.5px;color:var(--dim);margin:0 0 16px;max-width:70ch}
h3 .hint{font-weight:400;color:var(--faint);font-size:11.5px;margin-left:8px}
p.lead{font-size:13.5px;color:var(--text);max-width:74ch;line-height:1.65}
code.inl{font-family:ui-monospace,monospace;background:var(--panel2);border:1px solid var(--line);border-radius:4px;padding:1px 5px;font-size:12px;color:var(--accent)}

/* treemap */
.treemap{display:flex;flex-wrap:wrap;gap:3px;border-radius:8px;overflow:hidden}
.tmrow{display:flex;width:100%;gap:3px;height:var(--h)}
.tmcell{border-radius:5px;position:relative;overflow:hidden;cursor:default;min-width:2px;display:flex;align-items:flex-end;padding:6px 8px;color:#0b0e13;font-family:ui-monospace,monospace;transition:filter .12s}
.tmcell:hover{filter:brightness(1.12)}
.tmcell .lab{font-size:11px;font-weight:600;line-height:1.25;text-shadow:0 1px 0 rgba(255,255,255,.15)}
.tmcell .lab small{display:block;font-weight:400;opacity:.8;font-size:10px}

/* generic bars */
.barlist{display:flex;flex-direction:column;gap:5px}
.brow{display:grid;grid-template-columns:200px 1fr 78px;gap:12px;align-items:center;font-size:12px}
.brow .bn{font-family:ui-monospace,monospace;color:var(--text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.brow .bt{background:var(--panel2);border-radius:4px;height:16px;overflow:hidden;position:relative}
.brow .bf{height:100%;border-radius:4px}
.brow .bv{font-family:ui-monospace,monospace;text-align:right;color:var(--dim);font-variant-numeric:tabular-nums}
.brow .bn .sw{display:inline-block;width:8px;height:8px;border-radius:2px;margin-right:7px;vertical-align:middle}

/* legend */
.legend{display:flex;flex-wrap:wrap;gap:6px 16px;margin:2px 0 16px}
.lg{display:flex;align-items:center;gap:6px;font-size:11.5px;color:var(--dim)}
.lg .sw{width:11px;height:11px;border-radius:3px}
.lg b{color:var(--text);font-weight:500}

/* tables */
table.t{width:100%;border-collapse:collapse;font-size:12.5px}
table.t th{text-align:left;font-size:10.5px;text-transform:uppercase;letter-spacing:.07em;color:var(--faint);font-weight:500;padding:7px 10px;border-bottom:1px solid var(--line2);position:sticky;top:0;background:var(--panel)}
table.t td{padding:6px 10px;border-bottom:1px solid var(--line)}
table.t tr:hover td{background:var(--panel2)}
table.t td.n{font-family:ui-monospace,monospace;font-variant-numeric:tabular-nums;text-align:right;color:var(--dim)}
table.t td.m{font-family:ui-monospace,monospace;color:var(--text)}
.tag{display:inline-block;font-family:ui-monospace,monospace;font-size:10.5px;padding:1px 7px;border-radius:20px;border:1px solid currentColor}

/* memory map */
.memtools{display:flex;gap:8px;align-items:center;margin-bottom:14px;flex-wrap:wrap}
.seg{display:inline-flex;background:var(--panel2);border:1px solid var(--line);border-radius:8px;overflow:hidden}
.seg button{background:none;border:0;color:var(--dim);padding:6px 12px;cursor:pointer;font-size:12px;font-family:ui-monospace,monospace}
.seg button.on{background:var(--accent);color:#0b0e13;font-weight:600}
.memmap{display:flex;flex-direction:column;border:1px solid var(--line);border-radius:10px;overflow:hidden}
.mseg{display:grid;grid-template-columns:118px 1fr;border-bottom:1px solid var(--line);position:relative;min-height:var(--mh)}
.mseg:last-child{border-bottom:0}
.mseg .addr{font-family:ui-monospace,monospace;font-size:10.5px;color:var(--faint);padding:8px 10px;border-right:1px solid var(--line);background:var(--panel);display:flex;flex-direction:column;justify-content:space-between;white-space:nowrap}
.mseg .addr .hi{color:var(--dim)}
.mseg .body{padding:10px 14px;position:relative;display:flex;flex-direction:column;justify-content:center;background:linear-gradient(90deg,color-mix(in srgb,var(--c) 26%,var(--panel)),color-mix(in srgb,var(--c) 7%,var(--panel)));box-shadow:inset 4px 0 0 var(--c)}
.mseg .body .ml{font-family:ui-monospace,monospace;font-size:13px;color:var(--bright);font-weight:600}
.mseg .body .md{font-size:11.5px;color:var(--dim);margin-top:2px}
.mseg .body .msz{position:absolute;right:14px;top:50%;transform:translateY(-50%);font-family:ui-monospace,monospace;font-size:11px;color:var(--dim);text-align:right;font-variant-numeric:tabular-nums}
.mnote{font-size:11.5px;color:var(--faint);margin-top:10px;max-width:80ch}

/* opcode grid */
.opgrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:8px}
.opcell{display:grid;grid-template-columns:16px 1fr auto;gap:8px;align-items:center;background:var(--panel2);border:1px solid var(--line);border-radius:7px;padding:7px 10px}
.opcell .sw{width:10px;height:10px;border-radius:3px}
.opcell .on2{font-family:ui-monospace,monospace;font-size:12px;color:var(--text)}
.opcell .oc{font-family:ui-monospace,monospace;font-size:12px;color:var(--dim);font-variant-numeric:tabular-nums;text-align:right}
.opbar{height:3px;grid-column:1/-1;background:var(--panel);border-radius:2px;overflow:hidden;margin-top:2px}
.opbar i{display:block;height:100%}

/* assembly */
.asmtools{display:flex;gap:10px;align-items:center;margin-bottom:14px;flex-wrap:wrap;position:sticky;top:64px;z-index:15;background:var(--bg);padding:8px 0}
.asmtools input{background:var(--panel);border:1px solid var(--line);border-radius:8px;color:var(--text);padding:8px 12px;font-family:ui-monospace,monospace;font-size:12.5px;width:260px}
.asmtools input:focus{outline:2px solid var(--accent);border-color:var(--accent)}
.btn{background:var(--panel2);border:1px solid var(--line);border-radius:8px;color:var(--text);padding:7px 13px;cursor:pointer;font-size:12.5px}
.btn:hover{border-color:var(--line2)}
.btn.warn{color:var(--accent);border-color:var(--accent-dim)}
.fnlist{display:flex;flex-direction:column;gap:6px}
.fn{border:1px solid var(--line);border-radius:9px;overflow:hidden;background:var(--panel)}
.fn>summary{list-style:none;cursor:pointer;display:grid;grid-template-columns:30px 1fr auto auto;gap:12px;align-items:center;padding:9px 13px}
.fn>summary::-webkit-details-marker{display:none}
.fn>summary:hover{background:var(--panel2)}
.fn .idx{font-family:ui-monospace,monospace;font-size:11px;color:var(--faint);text-align:right}
.fn .fname{font-family:ui-monospace,monospace;font-size:13px;color:var(--bright);display:flex;align-items:center;gap:9px;min-width:0}
.fn .fname .fsig{color:var(--faint);font-size:11px;font-weight:400;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.fn .fsub{font-size:10px;font-family:ui-monospace,monospace;padding:1px 7px;border-radius:20px;border:1px solid currentColor;white-space:nowrap}
.fn .fsz{font-family:ui-monospace,monospace;font-size:11.5px;color:var(--dim);font-variant-numeric:tabular-nums;white-space:nowrap}
.fn[open]>summary{border-bottom:1px solid var(--line);background:var(--panel2)}
.fn .fmeta{padding:6px 13px;font-family:ui-monospace,monospace;font-size:11px;color:var(--faint);background:var(--panel2);border-bottom:1px solid var(--line)}
.asm{overflow-x:auto;padding:6px 0}
.asm .ln{display:grid;grid-template-columns:56px 196px 1fr;gap:0;font-family:ui-monospace,monospace;font-size:12px;line-height:1.55;white-space:nowrap;padding:0 13px}
.asm .ln:hover{background:var(--panel2)}
.asm .o{color:var(--faint);text-align:right;padding-right:14px;font-variant-numeric:tabular-nums}
.asm .hx{color:var(--dim);padding-right:14px;overflow:hidden;text-overflow:ellipsis}
.asm .ins{white-space:pre}
.asm .ic{font-weight:600}
.asm .oa{color:var(--text)}
.asm .cm{color:var(--faint);font-style:italic}
.asm .cm.callann{color:var(--accent);font-style:normal}
.ic.control{color:var(--op-control)} .ic.call{color:var(--op-call)} .ic.local{color:var(--op-local)} .ic.global{color:var(--op-global)} .ic.memory{color:var(--op-memory)} .ic.const{color:var(--op-const)} .ic.i32{color:var(--op-i32)} .ic.i64{color:var(--op-i64)} .ic.float{color:var(--op-float)} .ic.param{color:var(--op-param)} .ic.other{color:var(--op-other)}

/* subsystem text colors */
.s-engine{color:var(--sub-engine)} .s-codec{color:var(--sub-codec)} .s-bridge{color:var(--sub-bridge)} .s-strategy{color:var(--sub-strategy)} .s-runtime{color:var(--sub-runtime)} .s-other{color:var(--sub-other)}

/* data hex */
.hex{overflow-x:auto;font-family:ui-monospace,monospace;font-size:12px;line-height:1.6;border:1px solid var(--line);border-radius:8px;background:var(--panel)}
.hex .hl{display:grid;grid-template-columns:92px 1fr 150px;gap:16px;padding:1px 12px;white-space:nowrap}
.hex .hl:hover{background:var(--panel2)}
.hex .ha{color:var(--faint)}
.hex .hb{color:var(--text);letter-spacing:.02em}
.hex .hb .z{color:var(--faint)}
.hex .hc{color:var(--sub-strategy)}
.hexhdr{display:grid;grid-template-columns:92px 1fr 150px;gap:16px;padding:6px 12px;font-size:10.5px;text-transform:uppercase;letter-spacing:.06em;color:var(--faint);border-bottom:1px solid var(--line)}

@media (max-width:820px){#app{grid-template-columns:1fr;grid-template-rows:auto 1fr}#rail{flex-direction:row;overflow-x:auto;align-items:stretch}.brand{border-bottom:0;border-right:1px solid var(--line)}.nav{display:none}.brow{grid-template-columns:130px 1fr 64px}}
@media (prefers-reduced-motion:reduce){*{scroll-behavior:auto!important;transition:none!important}}
`;

const JS = fs.readFileSync(`${DIR}/app.js`, 'utf8');
fs.writeFileSync(OUT, renderHTML(payload, payloadObj.title));
console.log('wrote', OUT, (fs.statSync(OUT).size/1e6).toFixed(2), 'MB');
