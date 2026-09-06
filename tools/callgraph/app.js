"use strict";
const G = JSON.parse(document.getElementById("G").textContent);
const N = G.name.length, E = G.es.length;

/* ---------------------------------------------------------------- palette */
// Language sets the hue family; the group (category) shifts it and moves the
// lightness, so `c/src` rules and `c/wasm` bridge read as the same language in
// two shades rather than as two unrelated colours.
const LANG_BASE = {
  c: [352, 70, 50], ts: [206, 78, 50], swift: [30, 85, 50], rust: [270, 55, 58],
  sql: [92, 58, 44],
};
const CAT_TINT = {
  rules: [0, 6, 0], bots: [-16, 0, 10], anim: [14, 4, 12], wire: [-26, -6, -6],
  bridge: [24, -4, -10], ui: [8, 8, 14], state: [-8, -18, -8],
  network: [20, 2, -6], server: [-20, 4, -14], imessage: [32, 2, 6],
  test: [4, -30, -12], tools: [-32, -22, 2],
  // Library calls keep their caller's hue - C's `memcpy` is still C - but sit
  // far down the saturation scale, so the repo's own code is what carries
  // colour and the platform reads as background.
  platform: [0, -52, 6],
};
const CAT_LABEL = {
  rules: "Rules kernel", bots: "Bots & oracle", anim: "Animation",
  wire: "Wire & replay", bridge: "Bridges (wasm / iOS C)", ui: "Interface",
  state: "Client state", network: "Network & auth", server: "Server",
  imessage: "iMessage", test: "Tests", tools: "Tools & scripts",
  platform: "Platform & stdlib",
};
const LANG_LABEL = { c: "C", ts: "TypeScript", swift: "Swift", rust: "Rust",
                     sql: "SQL (Postgres)" };
const CONF_LABEL = {
  high: "Resolved", med: "Name-ambiguous", low: "Best guess", cross: "Crosses a language",
};

let dark = false;
function readTheme() {
  const t = document.documentElement.getAttribute("data-theme");
  dark = t === "dark" || (t !== "light" && matchMedia("(prefers-color-scheme: dark)").matches);
}
readTheme();

let COLOR = new Array(N), COLOR_DIM = new Array(N);
function mix(l, c) {
  const b = LANG_BASE[l] || LANG_BASE.c, t = CAT_TINT[c] || [0, 0, 0];
  const h = (b[0] + t[0] + 360) % 360;
  const s = Math.max(4, Math.min(96, b[1] + t[1]));
  const li = Math.max(16, Math.min(84, b[2] + t[2] + (dark ? 9 : 0)));
  return [h, s, li];
}
function css(hsl, a) {
  return "hsl(" + hsl[0] + " " + hsl[1] + "% " + hsl[2] + "%" + (a === undefined ? "" : " / " + a) + ")";
}
function buildColors() {
  for (let i = 0; i < N; i++) {
    const h = mix(G.langs[G.lang[i]], G.cats[G.cat[i]]);
    COLOR[i] = css(h);
    COLOR_DIM[i] = css([h[0], Math.max(3, h[1] - 46), dark ? h[2] - 18 : h[2] + 24], 0.85);
  }
}
buildColors();

let CSSV = {};
function readVars() {
  const s = getComputedStyle(document.documentElement);
  ["--ground", "--ink", "--ink-2", "--ink-3", "--panel", "--rule", "--edge", "--edge-cross", "--accent"]
    .forEach(k => CSSV[k] = s.getPropertyValue(k).trim());
}
readVars();

/* ------------------------------------------------------------- adjacency */
const outHead = new Int32Array(N).fill(-1), outNext = new Int32Array(E).fill(-1);
const inHead = new Int32Array(N).fill(-1), inNext = new Int32Array(E).fill(-1);
for (let e = E - 1; e >= 0; e--) {
  const s = G.es[e], t = G.et[e];
  outNext[e] = outHead[s]; outHead[s] = e;
  inNext[e] = inHead[t]; inHead[t] = e;
}
function callees(i) { const r = []; for (let e = outHead[i]; e !== -1; e = outNext[e]) r.push(e); return r; }
function callers(i) { const r = []; for (let e = inHead[i]; e !== -1; e = inNext[e]) r.push(e); return r; }

/* ----------------------------------------------------------------- state */
const st = {
  tests: false, std: false,
  view: "auto",
  lang: new Set(),      // hidden languages
  cat: new Set(),       // hidden groups
  conf: new Set(),      // hidden edge classes
  sel: -1, hov: -1, hovFile: -1,
  q: "", matches: new Set(),
};
const cam = { x: 0, y: 0, k: 1 };
const ABSENT = -32768;
// Tests are a quarter of the tree and the platform buckets another sixth, so
// each combination is a layout of its own rather than a filter over one map -
// hiding a third of the nodes from a shared layout would leave craters.
let X = new Int32Array(N), Y = new Int32Array(N), FXY = null, LAY = null;
function layoutKey() {
  return st.tests ? (st.std ? "full" : "noPlat") : (st.std ? "noTest" : "noBoth");
}
function inView(i) { return X[i] !== ABSENT; }
function visible(i) {
  return inView(i) && !st.lang.has(G.lang[i]) && !st.cat.has(G.cat[i]);
}
let VIS = new Uint8Array(N);
function recomputeVisible() {
  for (let i = 0; i < N; i++) VIS[i] = visible(i) ? 1 : 0;
}

/* ----------------------------------------------------------- spatial grid */
let grid = null, gridCell = 46, gx0 = 0, gy0 = 0, gw = 0, gh = 0;
function buildGrid() {
  let minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
  for (let i = 0; i < N; i++) {
    if (!inView(i)) continue;
    if (X[i] < minx) minx = X[i]; if (X[i] > maxx) maxx = X[i];
    if (Y[i] < miny) miny = Y[i]; if (Y[i] > maxy) maxy = Y[i];
  }
  gx0 = minx - 4; gy0 = miny - 4;
  gw = Math.max(1, Math.ceil((maxx - minx + 8) / gridCell));
  gh = Math.max(1, Math.ceil((maxy - miny + 8) / gridCell));
  grid = new Array(gw * gh);
  for (let i = 0; i < N; i++) {
    if (!inView(i)) continue;
    const cx = Math.min(gw - 1, Math.max(0, ((X[i] - gx0) / gridCell) | 0));
    const cy = Math.min(gh - 1, Math.max(0, ((Y[i] - gy0) / gridCell) | 0));
    const k = cy * gw + cx;
    (grid[k] || (grid[k] = [])).push(i);
  }
}
function pickNode(wx, wy, tol) {
  let best = -1, bd = tol * tol;
  const c0 = Math.max(0, ((wx - tol - gx0) / gridCell) | 0), c1 = Math.min(gw - 1, ((wx + tol - gx0) / gridCell) | 0);
  const r0 = Math.max(0, ((wy - tol - gy0) / gridCell) | 0), r1 = Math.min(gh - 1, ((wy + tol - gy0) / gridCell) | 0);
  for (let r = r0; r <= r1; r++) for (let c = c0; c <= c1; c++) {
    const cell = grid[r * gw + c]; if (!cell) continue;
    for (const i of cell) {
      if (!VIS[i]) continue;
      const dx = X[i] - wx, dy = Y[i] - wy, d = dx * dx + dy * dy;
      if (d < bd) { bd = d; best = i; }
    }
  }
  return best;
}

/* --------------------------------------------------------- file rollups */
let fileNodes = null, fileEdges = null, fileSig = "";
function fileState() {
  const sig = [st.tests, [...st.lang].sort(), [...st.cat].sort(), [...st.conf].sort()].join("|");
  if (sig === fileSig && fileNodes) return;
  fileSig = sig;
  fileNodes = new Map();                       // file idx -> {n, hue, cat, lang}
  for (let i = 0; i < N; i++) {
    if (!VIS[i]) continue;
    const f = G.file[i];
    let r = fileNodes.get(f);
    if (!r) fileNodes.set(f, r = { n: 0, tally: new Map() });
    r.n++;
    const key = G.lang[i] * 100 + G.cat[i];
    r.tally.set(key, (r.tally.get(key) || 0) + 1);
  }
  for (const [f, r] of fileNodes) {
    let bk = -1, bv = -1;
    for (const [k, v] of r.tally) if (v > bv) { bv = v; bk = k; }
    r.lang = (bk / 100) | 0; r.cat = bk % 100;
    r.color = css(mix(G.langs[r.lang], G.cats[r.cat]));
    r.tally = null;
  }
  const agg = new Map();
  for (let e = 0; e < E; e++) {
    if (st.conf.has(G.ec[e])) continue;
    const s = G.es[e], t = G.et[e];
    if (!VIS[s] || !VIS[t]) continue;
    const a = G.file[s], b = G.file[t];
    if (a === b) continue;
    const k = a * 100000 + b;
    const cur = agg.get(k);
    if (cur) { cur.w++; if (G.ec[e] === 3) cur.cross = 1; }
    else agg.set(k, { a, b, w: 1, cross: G.ec[e] === 3 ? 1 : 0 });
  }
  fileEdges = [...agg.values()];
}

/* ---------------------------------------------------------------- canvas */
const cv = document.getElementById("cv"), ctx = cv.getContext("2d", { alpha: false });
let W = 0, H = 0, DPR = 1;
function resize() {
  DPR = Math.min(2, window.devicePixelRatio || 1);
  W = cv.clientWidth; H = cv.clientHeight;
  cv.width = Math.round(W * DPR); cv.height = Math.round(H * DPR);
  ctx.setTransform(DPR, 0, 0, DPR, 0, 0);
  measure();
  draw();
}
const sx = wx => (wx - cam.x) * cam.k + W / 2;
const sy = wy => (wy - cam.y) * cam.k + H / 2;
const wxOf = px => (px - W / 2) / cam.k + cam.x;
const wyOf = py => (py - H / 2) / cam.k + cam.y;

function radius(i) { return 2.0 + Math.min(5.4, Math.sqrt(G.loc[i] || 1) * 0.42); }
// Screen radius is capped: past a point a bigger disc says nothing a label does
// not say better, and uncapped circles swallow the map at high zoom.
function screenRadius(i) { return Math.max(0.9, Math.min(9, radius(i) * cam.k)); }
// Each layout is a different size, so the zoom at which functions stop being
// legible is a property of the layout, not a constant. It is pinned to the
// zoom that fits the whole map: below ~1.7x that, draw file discs.
let fitK = 1, lodK = 0.3;
function mode() {
  if (st.view === "files") return "files";
  if (st.view === "fns") return "fns";
  return cam.k < lodK ? "files" : "fns";
}

let ego = null;
function computeEgo() {
  ego = null;
  if (st.sel < 0) return;
  const s = new Set([st.sel]), es = [];
  for (const e of callees(st.sel)) { if (VIS[G.et[e]] && !st.conf.has(G.ec[e])) { s.add(G.et[e]); es.push(e); } }
  for (const e of callers(st.sel)) { if (VIS[G.es[e]] && !st.conf.has(G.ec[e])) { s.add(G.es[e]); es.push(e); } }
  ego = { set: s, edges: es };
}

let raf = 0;
function draw() { if (!raf) raf = requestAnimationFrame(render); }

function render() {
  raf = 0;
  ctx.fillStyle = CSSV["--ground"];
  ctx.fillRect(0, 0, W, H);
  const m = mode();
  const pad = 60;
  const inScreen = (px, py, r) => px > -pad - r && px < W + pad + r && py > -pad - r && py < H + pad + r;
  let shown = 0;

  if (m === "files") {
    fileState();
    ctx.lineWidth = 1;
    ctx.strokeStyle = CSSV["--edge"];
    ctx.beginPath();
    for (const fe of fileEdges) {
      if (fe.w < (cam.k < 0.12 ? 3 : 1)) continue;
      if (FXY[fe.a * 3] === ABSENT || FXY[fe.b * 3] === ABSENT) continue;
      const ax = sx(FXY[fe.a * 3]), ay = sy(FXY[fe.a * 3 + 1]);
      const bx = sx(FXY[fe.b * 3]), by = sy(FXY[fe.b * 3 + 1]);
      if (!inScreen(ax, ay, 0) && !inScreen(bx, by, 0)) continue;
      ctx.moveTo(ax, ay); ctx.lineTo(bx, by);
    }
    ctx.stroke();
    ctx.strokeStyle = CSSV["--edge-cross"];
    ctx.lineWidth = 1.4;
    ctx.beginPath();
    for (const fe of fileEdges) {
      if (!fe.cross) continue;
      if (FXY[fe.a * 3] === ABSENT || FXY[fe.b * 3] === ABSENT) continue;
      const ax = sx(FXY[fe.a * 3]), ay = sy(FXY[fe.a * 3 + 1]);
      const bx = sx(FXY[fe.b * 3]), by = sy(FXY[fe.b * 3 + 1]);
      ctx.moveTo(ax, ay); ctx.lineTo(bx, by);
    }
    ctx.stroke();

    const labels = [];
    for (const [f, r] of fileNodes) {
      if (FXY[f * 3] === ABSENT) continue;
      const px = sx(FXY[f * 3]), py = sy(FXY[f * 3 + 1]);
      const rr = Math.max(2.2, FXY[f * 3 + 2] * cam.k);
      if (!inScreen(px, py, rr)) continue;
      shown++;
      ctx.fillStyle = r.color;
      ctx.globalAlpha = f === st.hovFile ? 1 : 0.82;
      ctx.beginPath(); ctx.arc(px, py, rr, 0, 6.2832); ctx.fill();
      ctx.globalAlpha = 1;
      if (f === st.hovFile) { ctx.strokeStyle = CSSV["--ink"]; ctx.lineWidth = 1.5; ctx.stroke(); }
      if (rr > 15) labels.push([px, py, rr, G.files[f], r.n]);
    }
    labels.sort((a, b) => b[2] - a[2]);
    ctx.textAlign = "center"; ctx.textBaseline = "middle";
    for (let i = 0; i < Math.min(labels.length, 90); i++) {
      const [px, py, rr, path, n] = labels[i];
      const nm = path.split("/").pop();
      ctx.font = "600 " + Math.min(15, Math.max(9, rr * 0.34)) + "px 'IBM Plex Mono', monospace";
      ctx.lineWidth = 3; ctx.strokeStyle = CSSV["--ground"];
      ctx.strokeText(nm, px, py); ctx.fillStyle = CSSV["--ink"]; ctx.fillText(nm, px, py);
      if (rr > 34) {
        ctx.font = "400 " + Math.min(11, rr * 0.2) + "px 'IBM Plex Mono', monospace";
        ctx.strokeText(n + " fn", px, py + rr * 0.34);
        ctx.fillStyle = CSSV["--ink-2"]; ctx.fillText(n + " fn", px, py + rr * 0.34);
      }
    }
  } else {
    // edges first, batched by class so the canvas takes three paths not 19k
    const drawEdges = (pred, style, wdt, alpha) => {
      ctx.strokeStyle = style; ctx.lineWidth = wdt; ctx.globalAlpha = alpha;
      ctx.beginPath();
      for (let e = 0; e < E; e++) {
        if (!pred(e)) continue;
        const s = G.es[e], t = G.et[e];
        if (!VIS[s] || !VIS[t]) continue;
        const ax = sx(X[s]), ay = sy(Y[s]), bx = sx(X[t]), by = sy(Y[t]);
        if ((ax < -pad && bx < -pad) || (ax > W + pad && bx > W + pad) ||
            (ay < -pad && by < -pad) || (ay > H + pad && by > H + pad)) continue;
        ctx.moveTo(ax, ay); ctx.lineTo(bx, by);
      }
      ctx.stroke(); ctx.globalAlpha = 1;
    };
    const dimmed = ego !== null;
    drawEdges(e => !st.conf.has(G.ec[e]) && G.ec[e] !== 3, CSSV["--edge"], 1, dimmed ? 0.3 : 1);
    drawEdges(e => !st.conf.has(3) && G.ec[e] === 3, CSSV["--edge-cross"], 1.3, dimmed ? 0.35 : 1);

    const labels = [];
    const showAll = cam.k > lodK * 3.4;
    for (let i = 0; i < N; i++) {
      if (!VIS[i]) continue;
      const px = sx(X[i]), py = sy(Y[i]);
      const r = screenRadius(i);
      if (!inScreen(px, py, r)) continue;
      shown++;
      const inEgo = !dimmed || ego.set.has(i);
      const hot = st.q && st.matches.has(i);
      ctx.fillStyle = hot ? CSSV["--accent"] : (inEgo ? COLOR[i] : COLOR_DIM[i]);
      ctx.globalAlpha = inEgo ? 1 : 0.72;
      ctx.beginPath(); ctx.arc(px, py, hot ? r + 1.4 : r, 0, 6.2832); ctx.fill();
      ctx.globalAlpha = 1;
      if (showAll && r > 2.4) labels.push([px, py, r, i, inEgo]);
    }

    if (dimmed) {
      ctx.strokeStyle = CSSV["--accent"]; ctx.lineWidth = 1.6;
      ctx.beginPath();
      for (const e of ego.edges) {
        ctx.moveTo(sx(X[G.es[e]]), sy(Y[G.es[e]]));
        ctx.lineTo(sx(X[G.et[e]]), sy(Y[G.et[e]]));
      }
      ctx.stroke();
    }

    ctx.textAlign = "left"; ctx.textBaseline = "middle";
    ctx.font = "500 11px 'IBM Plex Mono', monospace";
    let drawn = 0;
    for (const [px, py, r, i, inEgo] of labels) {
      if (drawn++ > 300) break;
      ctx.lineWidth = 3; ctx.strokeStyle = CSSV["--ground"];
      ctx.strokeText(G.name[i], px + r + 4, py);
      ctx.fillStyle = inEgo ? CSSV["--ink-2"] : CSSV["--ink-3"];
      ctx.fillText(G.name[i], px + r + 4, py);
    }

    for (const [i, ring] of [[st.hov, CSSV["--ink"]], [st.sel, CSSV["--accent"]]]) {
      if (i < 0 || !VIS[i]) continue;
      const px = sx(X[i]), py = sy(Y[i]), r = Math.max(3, screenRadius(i));
      ctx.strokeStyle = ring; ctx.lineWidth = 2;
      ctx.beginPath(); ctx.arc(px, py, r + 3.5, 0, 6.2832); ctx.stroke();
      ctx.font = "600 12px 'IBM Plex Mono', monospace";
      ctx.textAlign = "left"; ctx.textBaseline = "middle";
      ctx.lineWidth = 3.5; ctx.strokeStyle = CSSV["--ground"];
      ctx.strokeText(G.name[i], px + r + 7, py);
      ctx.fillStyle = CSSV["--ink"]; ctx.fillText(G.name[i], px + r + 7, py);
    }
  }

  document.getElementById("fz").textContent = cam.k.toFixed(2) + "×";
  document.getElementById("fv").textContent = shown.toLocaleString();
  document.getElementById("fmode").textContent =
    (st.view === "auto" ? "auto — " : "") + (m === "files" ? "file discs" : "functions");
}

/* ------------------------------------------------------------ navigation */
function bounds(pts) {
  let minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9, any = false;
  const each = i => {
    if (!VIS[i]) return;
    any = true;
    if (X[i] < minx) minx = X[i]; if (X[i] > maxx) maxx = X[i];
    if (Y[i] < miny) miny = Y[i]; if (Y[i] > maxy) maxy = Y[i];
  };
  if (pts) pts.forEach(each); else for (let i = 0; i < N; i++) each(i);
  if (!any) return null;
  return { minx, miny, maxx, maxy,
           w: Math.max(40, maxx - minx), h: Math.max(40, maxy - miny) };
}
function zoomFor(b) { return Math.min(W / (b.w * 1.12), H / (b.h * 1.12)); }
function measure() {
  const b = bounds(null);
  if (!b || !W) return;
  fitK = zoomFor(b);
  lodK = fitK * 1.7;
}
function fit(pts) {
  const b = bounds(pts);
  if (!b) return;
  flyTo((b.minx + b.maxx) / 2, (b.miny + b.maxy) / 2, zoomFor(b));
}
const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
let anim = null;
function flyTo(x, y, k) {
  k = Math.max(0.02, Math.min(5, k));
  if (reduce) { cam.x = x; cam.y = y; cam.k = k; draw(); return; }
  const t0 = performance.now(), a = { x: cam.x, y: cam.y, k: cam.k };
  anim = t => {
    const u = Math.min(1, (t - t0) / 420), e = u < .5 ? 4 * u * u * u : 1 - Math.pow(-2 * u + 2, 3) / 2;
    cam.x = a.x + (x - a.x) * e; cam.y = a.y + (y - a.y) * e;
    cam.k = Math.exp(Math.log(a.k) + (Math.log(k) - Math.log(a.k)) * e);
    render();
    if (u < 1) requestAnimationFrame(anim); else anim = null;
  };
  requestAnimationFrame(anim);
}

let dragging = false, moved = false, lastX = 0, lastY = 0;
cv.addEventListener("pointerdown", ev => {
  dragging = true; moved = false; lastX = ev.clientX; lastY = ev.clientY;
  cv.setPointerCapture(ev.pointerId); cv.classList.add("drag");
});
cv.addEventListener("pointermove", ev => {
  const rect = cv.getBoundingClientRect();
  if (dragging) {
    const dx = ev.clientX - lastX, dy = ev.clientY - lastY;
    if (Math.abs(dx) + Math.abs(dy) > 2) moved = true;
    cam.x -= dx / cam.k; cam.y -= dy / cam.k;
    lastX = ev.clientX; lastY = ev.clientY;
    draw();
    return;
  }
  const wx = wxOf(ev.clientX - rect.left), wy = wyOf(ev.clientY - rect.top);
  if (mode() === "files") {
    let best = -1, bd = 1e18;
    fileState();
    for (const [f] of fileNodes) {
      if (FXY[f * 3] === ABSENT) continue;
      const dx = FXY[f * 3] - wx, dy = FXY[f * 3 + 1] - wy, r = FXY[f * 3 + 2];
      const d = dx * dx + dy * dy;
      if (d < r * r && d < bd) { bd = d; best = f; }
    }
    if (best !== st.hovFile) { st.hovFile = best; draw(); }
    tip(ev, best < 0 ? null : G.files[best] + " · " + fileNodes.get(best).n + " functions", "click to open");
    document.getElementById("fh").textContent = best < 0 ? "—" : G.files[best];
  } else {
    const i = pickNode(wx, wy, 9 / cam.k);
    if (i !== st.hov) { st.hov = i; draw(); }
    tip(ev, i < 0 ? null : G.name[i], i < 0 ? "" : G.files[G.file[i]] + ":" + G.line[i]);
    document.getElementById("fh").textContent = i < 0 ? "—" : G.name[i];
  }
});
cv.addEventListener("pointerup", ev => {
  dragging = false; cv.classList.remove("drag");
  if (moved) return;
  const rect = cv.getBoundingClientRect();
  const wx = wxOf(ev.clientX - rect.left), wy = wyOf(ev.clientY - rect.top);
  if (mode() === "files") {
    if (st.hovFile >= 0) openFile(st.hovFile);
  } else {
    select(pickNode(wx, wy, 10 / cam.k));
  }
});
cv.addEventListener("pointerleave", () => { hideTip(); st.hov = -1; st.hovFile = -1; draw(); });
cv.addEventListener("wheel", ev => {
  ev.preventDefault();
  const rect = cv.getBoundingClientRect();
  const px = ev.clientX - rect.left, py = ev.clientY - rect.top;
  const wx = wxOf(px), wy = wyOf(py);
  const f = Math.exp(-(ev.deltaMode === 1 ? ev.deltaY * 16 : ev.deltaY) * 0.0016);
  cam.k = Math.max(0.02, Math.min(5, cam.k * f));
  cam.x = wx - (px - W / 2) / cam.k;
  cam.y = wy - (py - H / 2) / cam.k;
  draw();
}, { passive: false });

const tipEl = document.getElementById("tip");
function tip(ev, t1, t2) {
  if (!t1) return hideTip();
  tipEl.hidden = false;
  tipEl.innerHTML = "";
  tipEl.append(document.createTextNode(t1));
  if (t2) { const d = document.createElement("div"); d.className = "t2"; d.textContent = t2; tipEl.append(d); }
  const rect = cv.getBoundingClientRect();
  const x = ev.clientX - rect.left + 14, y = ev.clientY - rect.top + 14;
  tipEl.style.left = Math.min(x, W - tipEl.offsetWidth - 8) + "px";
  tipEl.style.top = Math.min(y, H - tipEl.offsetHeight - 8) + "px";
}
function hideTip() { tipEl.hidden = true; }

function openFile(f) {
  const members = [];
  for (let i = 0; i < N; i++) if (VIS[i] && G.file[i] === f) members.push(i);
  if (!members.length) return;
  if (st.view === "files") setView("auto");
  fit(members);
  members.sort((a, b) => (G.name[a] > G.name[b] ? 1 : -1));
  select(members.reduce((a, b) => (inDeg(b) > inDeg(a) ? b : a), members[0]), true);
}
function inDeg(i) { let n = 0; for (let e = inHead[i]; e !== -1; e = inNext[e]) n++; return n; }

/* ------------------------------------------------------------- inspector */
const selEl = document.getElementById("sel");
function fanCounts(i) {
  let o = 0, ii = 0;
  for (let e = outHead[i]; e !== -1; e = outNext[e]) if (VIS[G.et[e]] && !st.conf.has(G.ec[e])) o++;
  for (let e = inHead[i]; e !== -1; e = inNext[e]) if (VIS[G.es[e]] && !st.conf.has(G.ec[e])) ii++;
  return [ii, o];
}
function dotFor(i) {
  const s = document.createElement("span");
  s.className = "dot"; s.style.background = COLOR[i];
  return s;
}
function relList(title, edges, other) {
  const wrap = document.createElement("div"); wrap.className = "rel";
  const h = document.createElement("h3");
  h.append(document.createTextNode(title));
  const c = document.createElement("span"); c.textContent = edges.length; h.append(c);
  wrap.append(h);
  if (!edges.length) {
    const p = document.createElement("div"); p.className = "empty";
    p.textContent = "none in this view";
    wrap.append(p); return wrap;
  }
  const ul = document.createElement("ul");
  edges.sort((a, b) => (G.name[other(a)] > G.name[other(b)] ? 1 : -1));
  for (const e of edges.slice(0, 200)) {
    const j = other(e);
    const li = document.createElement("li"), b = document.createElement("button");
    b.append(dotFor(j));
    const nm = document.createElement("span"); nm.className = "nm";
    nm.textContent = G.name[j];
    nm.title = G.files[G.file[j]] + ":" + G.line[j];
    const cf = document.createElement("span"); cf.className = "cf";
    cf.textContent = G.confs[G.ec[e]]; cf.dataset.c = G.confs[G.ec[e]];
    b.append(nm, cf);
    b.addEventListener("click", () => { select(j); flyTo(X[j], Y[j], Math.max(cam.k, 1.9)); });
    li.append(b); ul.append(li);
  }
  wrap.append(ul);
  return wrap;
}
function select(i, quiet) {
  st.sel = i;
  computeEgo();
  renderSel();
  draw();
  if (i >= 0 && !quiet && mode() === "files") flyTo(X[i], Y[i], Math.max(cam.k, 1.9));
}
function renderSel() {
  selEl.innerHTML = "";
  const i = st.sel;
  if (i < 0) {
    const p = document.createElement("div"); p.className = "note";
    p.innerHTML = "<p><b>Nothing selected.</b> Click any node on the map, or search above, " +
      "to see what it calls and what calls it.</p><p>Every dot is one function, method or " +
      "closure with a body. Every line is a call site found in that body.</p>";
    selEl.append(p);
    return;
  }
  const h = document.createElement("h2"); h.className = "sel-name"; h.textContent = G.name[i];
  const p = document.createElement("p"); p.className = "sel-path";
  p.textContent = G.files[G.file[i]] + (G.line[i] ? ":" + G.line[i] : "");
  const tags = document.createElement("div"); tags.className = "tags";
  const mk = (txt, withDot) => {
    const s = document.createElement("span"); s.className = "tag";
    if (withDot) s.append(dotFor(i));
    s.append(document.createTextNode(txt)); return s;
  };
  tags.append(mk(LANG_LABEL[G.langs[G.lang[i]]], true));
  tags.append(mk(CAT_LABEL[G.cats[G.cat[i]]]));
  tags.append(mk(G.kinds[G.kind[i]]));
  if (G.exp[i]) tags.append(mk("exported"));
  const [fi, fo] = fanCounts(i);
  const met = document.createElement("div"); met.className = "metrics";
  for (const [k, v] of [["callers", fi], ["calls out", fo], ["lines", G.loc[i] || "—"]]) {
    const d = document.createElement("div"); d.className = "metric";
    const kk = document.createElement("div"); kk.className = "k"; kk.textContent = k;
    const vv = document.createElement("div"); vv.className = "v"; vv.textContent = v;
    d.append(kk, vv); met.append(d);
  }
  selEl.append(h, p, tags, met);
  selEl.append(relList("Calls out to", callees(i).filter(e => VIS[G.et[e]] && !st.conf.has(G.ec[e])), e => G.et[e]));
  selEl.append(relList("Called by", callers(i).filter(e => VIS[G.es[e]] && !st.conf.has(G.ec[e])), e => G.es[e]));
}

/* ---------------------------------------------------------------- search */
const qEl = document.getElementById("q"), resEl = document.getElementById("results");
let qTimer = 0;
qEl.addEventListener("input", () => { clearTimeout(qTimer); qTimer = setTimeout(runSearch, 90); });
function runSearch() {
  st.q = qEl.value.trim().toLowerCase();
  st.matches = new Set();
  resEl.innerHTML = "";
  if (!st.q) { resEl.hidden = true; draw(); return; }
  const hits = [];
  for (let i = 0; i < N; i++) {
    if (!VIS[i]) continue;
    const n = G.name[i].toLowerCase();
    let score = -1;
    if (n === st.q) score = 0;
    else if (n.startsWith(st.q)) score = 1;
    else if (n.includes(st.q)) score = 2;
    else if (G.files[G.file[i]].toLowerCase().includes(st.q)) score = 3;
    if (score >= 0) { st.matches.add(i); hits.push([score, -inDeg(i), i]); }
  }
  hits.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const h3 = document.createElement("h3");
  h3.style.cssText = "margin:0 0 6px;font-size:10px;letter-spacing:.13em;text-transform:uppercase;color:var(--ink-3);display:flex;justify-content:space-between";
  h3.innerHTML = "<span>Matches</span><span>" + hits.length + "</span>";
  const wrap = document.createElement("div"); wrap.className = "rel";
  wrap.append(h3);
  const ul = document.createElement("ul");
  for (const [, , i] of hits.slice(0, 60)) {
    const li = document.createElement("li"), b = document.createElement("button");
    b.append(dotFor(i));
    const nm = document.createElement("span"); nm.className = "nm"; nm.textContent = G.name[i];
    const cf = document.createElement("span"); cf.className = "cf";
    cf.textContent = G.files[G.file[i]].split("/").pop();
    b.append(nm, cf);
    b.addEventListener("click", () => { select(i, true); flyTo(X[i], Y[i], 2.4); });
    li.append(b); ul.append(li);
  }
  if (!hits.length) {
    const e = document.createElement("div"); e.className = "empty"; e.textContent = "no match in this view";
    wrap.append(e);
  } else wrap.append(ul);
  resEl.append(wrap); resEl.hidden = false;
  draw();
}
qEl.addEventListener("keydown", ev => {
  if (ev.key === "Enter") {
    const first = resEl.querySelector("li button");
    if (first) first.click();
  }
});

/* ----------------------------------------------------------------- rails */
function countBy(sel) {
  const m = new Map();
  for (let i = 0; i < N; i++) { if (!inView(i)) continue; const k = sel(i); m.set(k, (m.get(k) || 0) + 1); }
  return m;
}
function buildLegends() {
  const lc = countBy(i => G.langs[G.lang[i]]);
  const cc = countBy(i => G.cats[G.cat[i]]);
  const langBox = document.getElementById("langLegend"); langBox.innerHTML = "";
  G.langs.forEach((l, idx) => {
    const b = document.createElement("button"); b.className = "chip";
    b.dataset.off = st.lang.has(idx) ? "1" : "0";
    b.setAttribute("aria-pressed", st.lang.has(idx) ? "false" : "true");
    const d = document.createElement("span"); d.className = "dot";
    d.style.background = css(mix(l, "rules"));
    const n = document.createElement("span"); n.className = "nm"; n.textContent = LANG_LABEL[l];
    const c = document.createElement("span"); c.className = "ct"; c.textContent = (lc.get(l) || 0).toLocaleString();
    b.append(d, n, c);
    b.addEventListener("click", () => {
      st.lang.has(idx) ? st.lang.delete(idx) : st.lang.add(idx);
      refresh();
    });
    langBox.append(b);
  });
  const catBox = document.getElementById("catLegend"); catBox.innerHTML = "";
  G.cats.forEach((c0, idx) => {
    if (!cc.get(c0)) return;
    const b = document.createElement("button"); b.className = "chip";
    b.dataset.off = st.cat.has(idx) ? "1" : "0";
    b.setAttribute("aria-pressed", st.cat.has(idx) ? "false" : "true");
    const sw = document.createElement("span"); sw.className = "sw";
    const langs = ["c", "ts", "swift", "rust", "sql"];
    const stripes = langs.map((l, j) =>
      css(mix(l, c0)) + " " + (j * 20) + "% " + ((j + 1) * 20) + "%");
    sw.style.background = "linear-gradient(180deg," + stripes.join(",") + ")";
    const n = document.createElement("span"); n.className = "nm"; n.textContent = CAT_LABEL[c0];
    const ct = document.createElement("span"); ct.className = "ct"; ct.textContent = (cc.get(c0) || 0).toLocaleString();
    b.append(sw, n, ct);
    b.addEventListener("click", () => {
      st.cat.has(idx) ? st.cat.delete(idx) : st.cat.add(idx);
      refresh();
    });
    catBox.append(b);
  });
  const confBox = document.getElementById("confLegend"); confBox.innerHTML = "";
  const cCount = new Map();
  for (let e = 0; e < E; e++) {
    if (!inView(G.es[e]) || !inView(G.et[e])) continue;
    const k = G.ec[e]; cCount.set(k, (cCount.get(k) || 0) + 1);
  }
  G.confs.forEach((cn, idx) => {
    const b = document.createElement("button"); b.className = "chip";
    b.dataset.off = st.conf.has(idx) ? "1" : "0";
    b.setAttribute("aria-pressed", st.conf.has(idx) ? "false" : "true");
    const d = document.createElement("span"); d.className = "sw";
    d.style.background = idx === 3 ? CSSV["--accent"] : CSSV["--ink-3"];
    d.style.opacity = idx === 3 ? "1" : String(1 - idx * 0.28);
    const n = document.createElement("span"); n.className = "nm"; n.textContent = CONF_LABEL[cn];
    const c = document.createElement("span"); c.className = "ct"; c.textContent = (cCount.get(idx) || 0).toLocaleString();
    b.append(d, n, c);
    b.addEventListener("click", () => {
      st.conf.has(idx) ? st.conf.delete(idx) : st.conf.add(idx);
      refresh();
    });
    confBox.append(b);
  });
}
function buildCross() {
  const el = document.getElementById("crossStats"); el.innerHTML = "";
  const rows = [
    ["TypeScript \u2192 C", G.stats.pair["ts->c"] || 0, "calls into the wasm exports"],
    ["Swift \u2192 C", G.stats.pair["swift->c"] || 0, "calls through c/ios/ios_api.c"],
    ["TypeScript \u2192 SQL", G.stats.pair["ts->sql"] || 0, "supabase.rpc(...) by name"],
    ["SQL \u2192 TypeScript", G.stats.pair["sql->ts"] || 0,
     "the pg_cron job posts to an edge function"],
    ["Rust \u2192 anything", 0, "the port shares no code with the rest"],
  ];
  for (const [k, v, why] of rows) {
    const dt = document.createElement("dt"); dt.textContent = k; dt.title = why;
    const dd = document.createElement("dd"); dd.textContent = v.toLocaleString();
    el.append(dt, dd);
  }
}

function buildMethod() {
  document.getElementById("method").innerHTML =
    "<p><b>C</b> \u2014 clang's own JSON AST. Every translation unit is parsed twice, " +
    "once in the default build and once with <span class=\"mono\">FOOLISH_ORACLE_MT</span> " +
    "set, because whole files sit behind that <span class=\"mono\">#ifdef</span>; the two " +
    "readings are unioned.</p>" +
    "<p><b>TypeScript</b> \u2014 the compiler API with its checker. A call the checker " +
    "resolves to a body-less declaration (an interface member, an overload signature) is " +
    "matched back to the implementation by name.</p>" +
    "<p><b>Swift</b> \u2014 no Swift toolchain here, so a hand-written scope-aware parser: " +
    "comments and strings blanked, brace-depth type scopes, calls matched by name. Overloads " +
    "on different types collapse together; those edges are marked <i>name-ambiguous</i>.</p>" +
    "<p><b>Rust</b> \u2014 the <span class=\"mono\">syn</span> crate's full AST, named " +
    "closures included.</p>" +
    "<p><b>SQL</b> \u2014 the migrations replayed in order, keeping only the definition " +
    "of each routine still in force: <span class=\"mono\">commit_game</span> is dropped " +
    "and recreated six times as its signature changes. Triggers and the " +
    "<span class=\"mono\">pg_cron</span> job are included, since they are how a routine " +
    "runs with no call site anywhere in the app.</p>" +
    "<p>Dynamic dispatch, functions passed as values, SwiftUI's implicit " +
    "<span class=\"mono\">body</span> re-entry and anything reached through reflection are " +
    "invisible to all four. The map is what the source says, not what runs.</p>";
}

/* ---------------------------------------------------------------- toggles */
function setLayout() {
  LAY = G.layouts[layoutKey()];
  for (let i = 0; i < N; i++) { X[i] = LAY.xy[i * 2]; Y[i] = LAY.xy[i * 2 + 1]; }
  FXY = LAY.fileXY;
  document.getElementById("counts").textContent =
    LAY.nodes.toLocaleString() + " functions · " + LAY.edges.toLocaleString() + " calls";
  recomputeVisible();
  buildGrid();
  measure();
  fileSig = "";
}
function refresh(refit) {
  recomputeVisible();
  measure();
  fileSig = "";
  computeEgo();
  buildLegends();
  if (st.q) runSearch();
  renderSel();
  if (refit) fit();
  draw();
}
function setInclude(which, on) {
  if (st[which] === on) return;
  st[which] = on;
  document.getElementById(which === "tests" ? "tTests" : "tStd")
    .setAttribute("aria-pressed", String(on));
  st.hov = -1; st.hovFile = -1;
  setLayout();
  if (st.sel >= 0 && !inView(st.sel)) st.sel = -1;
  refresh(true);
}
function setView(v) {
  st.view = v;
  for (const [id, val] of [["vAuto", "auto"], ["vFiles", "files"], ["vFns", "fns"]])
    document.getElementById(id).setAttribute("aria-pressed", String(v === val));
  draw();
}
document.getElementById("tTests").addEventListener("click",
  () => setInclude("tests", !st.tests));
document.getElementById("tStd").addEventListener("click",
  () => setInclude("std", !st.std));
document.getElementById("vAuto").addEventListener("click", () => setView("auto"));
document.getElementById("vFiles").addEventListener("click", () => setView("files"));
document.getElementById("vFns").addEventListener("click", () => setView("fns"));
document.getElementById("zin").addEventListener("click", () => flyTo(cam.x, cam.y, cam.k * 1.7));
document.getElementById("zout").addEventListener("click", () => flyTo(cam.x, cam.y, cam.k / 1.7));
document.getElementById("fit").addEventListener("click", () => fit());

addEventListener("keydown", ev => {
  if (ev.target === qEl) { if (ev.key === "Escape") { qEl.value = ""; runSearch(); qEl.blur(); } return; }
  if (ev.key === "/") { ev.preventDefault(); qEl.focus(); qEl.select(); }
  else if (ev.key === "Escape") { select(-1); }
  else if (ev.key === "f" || ev.key === "F") { fit(); }
});
addEventListener("resize", resize);
const mq = matchMedia("(prefers-color-scheme: dark)");
mq.addEventListener("change", () => { readTheme(); readVars(); buildColors(); buildLegends(); renderSel(); draw(); });
new MutationObserver(() => { readTheme(); readVars(); buildColors(); buildLegends(); renderSel(); draw(); })
  .observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });

/* ------------------------------------------------------------------ boot */
setLayout();
buildLegends(); buildCross(); buildMethod();
resize();
fit();
// Open on the seam the whole repo turns around: the wasm entry point the web
// client calls to run a move through the C rules kernel.
let boot = -1;
for (let i = 0; i < N; i++) if (VIS[i] && G.name[i] === "wasm_apply_action") { boot = i; break; }
if (boot < 0) for (let i = 0; i < N; i++) if (VIS[i] && G.name[i] === "apply_action") { boot = i; break; }
select(boot, true);
setTimeout(() => { const h = document.getElementById("hint"); if (h) h.remove(); }, 9000);
