// Texture parity: the C generators (out/tex_test.wasm) against the app's own TS generators, run in
// headless Chrome with the same seed and size, Math.random replaced by the same mulberry32 the C
// uses. Prints, per texture and per TS path, the pixels that differ and the largest per-channel
// difference. The TS is bundled from src/ as it is (esbuild, from the main checkout's
// node_modules); its unexported generators are reached by appending an export line at load.
//   node tools/texparity.mjs [seed]        (after ./build.sh)
import fs from 'node:fs';
import path from 'node:path';
import { launch, serve, FOOLISH, POC } from './cdp.mjs';

const MAIN_MODULES = '/Users/alex/Dev/foolish/foolish/node_modules';
const { build } = await import(path.join(MAIN_MODULES, 'esbuild/lib/main.js'));
const SEED = Number(process.argv[2] || 20261028) >>> 0;
const out = path.join(POC, 'out');

// The TS as the app has it, plus one export line for what the module keeps to itself.
const EXTRA = {
  'WoodTexture.tsx': 'export { generateWoodPixelData, generateWoodTextureFallback };',
  'fernFractal.tsx': 'export { generateFernPoints, createCardParams, generateFernPattern, generateFernPatternFallback };',
};
fs.writeFileSync(path.join(out, 'tex_entry.ts'), `
export { paintWool } from '${FOOLISH}/src/utils/woolPixels';
export { generateTextureViaWebGL } from '${FOOLISH}/src/utils/webglTexture';
export { generateWoodPixelData, generateWoodTextureFallback } from '${FOOLISH}/src/components/WoodTexture';
export { generateFernPoints, createCardParams, generateFernPattern, generateFernPatternFallback } from '${FOOLISH}/src/utils/fernFractal';
`);
await build({
  entryPoints: [path.join(out, 'tex_entry.ts')], bundle: true, format: 'iife', globalName: 'TS', outfile: path.join(out, 'tex_ts.js'),
  nodePaths: [MAIN_MODULES], logLevel: 'error', jsx: 'automatic', define: { 'process.env.NODE_ENV': '"production"' },
  plugins: [{ name: 'export-internals', setup(b) {
    b.onLoad({ filter: /(WoodTexture|fernFractal)\.tsx$/ }, (a) => ({
      contents: fs.readFileSync(a.path, 'utf8') + '\n' + EXTRA[path.basename(a.path)], loader: 'tsx', resolveDir: path.dirname(a.path) }));
  } }],
});
fs.writeFileSync(path.join(out, 'texparity.html'), `<!doctype html><html><head><meta charset="utf-8"></head><body>
<script src="tex_ts.js"></script></body></html>`);

const PAGE = `(async () => {
  const mul = (seed) => { let a = seed >>> 0; return () => { a = (a + 0x6D2B79F5) >>> 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; };
  const real = Math.random;
  const C = (await WebAssembly.instantiateStreaming(fetch('tex_test.wasm'))).instance.exports;
  const bytes = (p, n) => new Uint8Array(C.memory.buffer, p, n);
  const diff = (a, b, w, h) => { let px = 0, max = 0, sum = 0;
    for (let i = 0; i < w * h * 4; i += 4) { let any = false;
      for (let k = 0; k < 4; k++) { const d = Math.abs(a[i + k] - b[i + k]); if (d) { any = true; sum += d; if (d > max) max = d; } } if (any) px++; }
    return { differing: px, of: w * h, max, mean: px ? +(sum / px).toFixed(2) : 0 }; };
  const decode = async (url, w, h) => { const img = new Image(); img.src = url; await img.decode();
    const cv = document.createElement('canvas'); cv.width = w; cv.height = h; const cx = cv.getContext('2d');
    cx.drawImage(img, 0, 0); return cx.getImageData(0, 0, w, h).data; };
  // The PNG's own bytes, alpha not premultiplied on the way (the fern has partial alpha).
  const decodeRaw = async (url, w, h) => { const bm = await createImageBitmap(await (await fetch(url)).blob(), { premultiplyAlpha: 'none' });
    const cv = new OffscreenCanvas(w, h); const cx = cv.getContext('2d'); cx.drawImage(bm, 0, 0); return cx.getImageData(0, 0, w, h).data; };
  const noWebGL = (f) => async (...a) => { const g = HTMLCanvasElement.prototype.getContext;
    HTMLCanvasElement.prototype.getContext = function (k, o) { return k === 'webgl' ? null : g.call(this, k, o); };
    try { return await f(...a); } finally { HTMLCanvasElement.prototype.getContext = g; } };
  const r = {};
  const seed = ${SEED};
  // the PRNG itself
  { const m = mul(seed); let v; for (let i = 0; i < 1000; i++) v = m(); r.random = v === C.random(seed, 1000) ? 'same 1000 draws' : 'DIFFERENT'; }

  // the trig against Chrome's own, on arguments in the textures' ranges and beyond: the wood's cos
  // (crtrig.c) must be Chrome's bit for bit; trig.c's sin, cos and tan within an ulp.
  { const m = mul(seed ^ 0x5bd1e995); const ulps = (a, b) => { if (Object.is(a, b)) return 0;
      const f = new Float64Array([a, b]), i = new BigInt64Array(f.buffer); const d = i[0] - i[1]; return Number(d < 0n ? -d : d); };
    const t = { cr_cos: [0, 0], sin: [0, 0], cos: [0, 0], tan: [0, 0] }; let n = 0;
    const see = (k, u) => { if (u) { t[k][0]++; t[k][1] = Math.max(t[k][1], u); } };
    for (const span of [0.5, 4, 30, 1000, 4e5, 1e7]) for (let i = 0; i < 500000; i++, n++) { const a = (m() - 0.5) * 2 * span;
      if (span < 4e5) see('cr_cos', ulps(Math.cos(a), C.cr_cos(a)));
      see('sin', ulps(Math.sin(a), C.sin(a))); see('cos', ulps(Math.cos(a), C.cos(a))); see('tan', ulps(Math.tan(a), C.tan(a))); }
    r.trig = { arguments: n, differing_and_max_ulps: Object.fromEntries(Object.entries(t).map(([k, [d, u]]) => [k, d + ' differ, max ' + u + ' ulp'])), raw: t }; }

  // wool: paintWool into a buffer, offsets as the app draws them, then the JPEG the app encodes
  // (woolWorker.ts: putImageData, convertToBlob at 0.92). The C weaves the whole picture and, as the
  // page does, only its middle half (the crop must be the same pixels as the TS's middle half).
  { const m = mul(seed), ox = m() * 1000 - 500, oy = m() * 1000 - 500, w = 3840, h = 2160, cw = w / 2, ch = h / 2, cx = (w - cw) / 2, cy = (h - ch) / 2;
    const ts = new Uint8ClampedArray(w * h * 4); let t0 = performance.now();
    await TS.paintWool(ts, { width: w, height: h, offsetX: ox, offsetY: oy }); const tsMs = performance.now() - t0;
    t0 = performance.now(); const cv = new OffscreenCanvas(w, h); cv.getContext('2d').putImageData(new ImageData(ts, w, h), 0, 0);
    await cv.convertToBlob({ type: 'image/jpeg', quality: 0.92 }); const tsEnc = performance.now() - t0;
    C.reset(); t0 = performance.now(); const p = C.wool(w, h, 0, 0, w, h, ox, oy); const cMs = performance.now() - t0;
    const full = diff(ts, bytes(p, w * h * 4), w, h);
    C.reset(); t0 = performance.now(); const q = C.wool(w, h, cx, cy, cw, ch, ox, oy); const cCrop = performance.now() - t0;
    const mid = new Uint8ClampedArray(cw * ch * 4);
    for (let y = 0; y < ch; y++) mid.set(ts.subarray(((y + cy) * w + cx) * 4, ((y + cy) * w + cx + cw) * 4), y * cw * 4);
    r.wool = { size: w + 'x' + h, ...full, crop: cw + 'x' + ch + ' at ' + cx + ',' + cy, crop_vs_ts_middle: diff(mid, bytes(q, cw * ch * 4), cw, ch),
      ts_ms: Math.round(tsMs), ts_encode_ms: Math.round(tsEnc), c_ms: Math.round(cMs), c_crop_ms: Math.round(cCrop) }; }

  // wood: the float colours, the WebGL blit the app uses, and the CPU fallback; then the wood worked
  // out only inside rectangles, as the page does, which must be the same pixels there and the base
  // colour elsewhere. The rectangles take in the tile's edges: a stripe that starts left of x = 0
  // lands at the right end of the row above, and row 0's such stores are dropped.
  { const w = 1920, h = 1080;
    Math.random = mul(seed); let t0 = performance.now(); const pd = await TS.generateWoodPixelData(w, h); const tsMs = performance.now() - t0;
    C.reset(); t0 = performance.now(); const p = C.wood(w, h, seed, 0); const cMs = performance.now() - t0;
    const cf = new Float32Array(C.memory.buffer, C.wood_col(), w * h * 3); let fd = 0, fmax = 0;
    for (let i = 0; i < w * h * 3; i++) if (cf[i] !== pd.colors[i]) { fd++; fmax = Math.max(fmax, Math.abs(cf[i] - pd.colors[i])); }
    const mine = bytes(p, w * h * 4).slice();
    t0 = performance.now();
    const glUrl = await TS.generateTextureViaWebGL(w, h, 'wood', [70 / 255, 14 / 255, 9 / 255, 1], TS.generateWoodPixelData, TS.generateWoodTextureFallback);
    const tsAll = performance.now() - t0;
    const gl = await decode(glUrl, w, h);
    const cpu = await decode(await noWebGL(TS.generateWoodTextureFallback)(w, h), w, h);
    const rects = [[1700, 0, 220, 64], [0, 500, 268, 68], [1438, 246, 268, 68], [1880, 1000, 40, 80], [900, 1012, 368, 68], [1850, 1, 70, 30]];
    C.reset(); new Int32Array(C.memory.buffer, C.wood_rects(), rects.length * 4).set(rects.flat());
    t0 = performance.now(); const q = C.wood(w, h, seed, rects.length); const cRects = performance.now() - t0;
    const part = bytes(q, w * h * 4).slice(), base = [gl[0], 0, 0];
    let inside = 0, inDiff = 0, inMax = 0, outDiff = 0;
    const inRect = (x, y) => rects.some(([rx, ry, rw, rh]) => x >= rx && x < rx + rw && y >= ry && y < ry + rh);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const i = (y * w + x) * 4;
      if (inRect(x, y)) { inside++; let d = 0; for (let k = 0; k < 4; k++) d = Math.max(d, Math.abs(part[i + k] - gl[i + k])); if (d) inDiff++; inMax = Math.max(inMax, d); }
      else if (part[i] !== 70 || part[i + 1] !== 14 || part[i + 2] !== 9 || part[i + 3] !== 255) outDiff++; }
    r.wood = { size: w + 'x' + h, float_values_differing: fd + ' of ' + w * h * 3, float_max_abs: fmax,
      vs_webgl: diff(gl, mine, w, h), vs_cpu_fallback: diff(cpu, mine, w, h),
      rects: { pixels: inside, differing_from_webgl: inDiff, max: inMax, outside_not_base: outDiff },
      ts_ms: Math.round(tsMs), ts_with_webgl_and_png_ms: Math.round(tsAll), c_ms: Math.round(cMs), c_rects_ms: Math.round(cRects) }; }

  // fern: the points plotted as the GL draw plots them, the WebGL output, the CPU fallback
  { const w = 1000, h = 1400, params = TS.createCardParams(w, h), n = params.render.iterations;
    const pts = new Uint8ClampedArray(w * h * 4);
    Math.random = mul(seed); let t0 = performance.now();
    await TS.generateFernPoints(params, w, h, 1e9, (px, py, R, G, B) => { const x = Math.floor(Math.fround(px)), y = Math.floor(Math.fround(py));
      if (x >= 0 && x < w && y >= 0 && y < h) { const i = (y * w + x) * 4; pts[i] = R; pts[i + 1] = G; pts[i + 2] = B; pts[i + 3] = 255; } });
    const tsMs = performance.now() - t0;
    C.reset(); const plain = bytes(C.fern(w, h, n, seed, 0), w * h * 4).slice();
    C.reset(); t0 = performance.now(); const p = C.fern(w, h, n, seed, 1); const cMs = performance.now() - t0;
    const mine = bytes(p, w * h * 4).slice();
    Math.random = mul(seed); const gl = await decodeRaw(await TS.generateFernPattern(params, { width: w, height: h, bypassCache: true }), w, h);
    Math.random = mul(seed); const cpu = await decodeRaw(await TS.generateFernPatternFallback(w, h, params), w, h);
    r.fern = { size: w + 'x' + h, iterations: n, points_one_pixel_each_vs_ts_points: diff(pts, plain, w, h),
      multisampled_vs_webgl: diff(gl, mine, w, h), one_pixel_each_vs_webgl: diff(gl, plain, w, h),
      one_pixel_each_vs_cpu_fallback: diff(cpu, plain, w, h), ts_ms: Math.round(tsMs), c_ms: Math.round(cMs) }; }
  Math.random = real;
  return r;
})()`;

const server = await serve(8720, out);
const b = await launch();
let res;
try {
  await b.goto('http://127.0.0.1:8720/texparity.html');
  res = await b.evaluate(PAGE);
} finally { await b.close(); server.close(); }
console.log(JSON.stringify(res, null, 1));
// The bar: the PRNG, sin and cos, the wool, the wood (floats and the WebGL picture) and the fern's
// points exact; the multisampled fern within 3% of the WebGL picture (README, Textures).
const bad = [res.random !== 'same 1000 draws' && 'random', res.trig.raw.cr_cos[0] && 'cr_cos', ['sin', 'cos', 'tan'].some((k) => res.trig.raw[k][1] > 1) && 'trig ulp',
  (res.wool.differing || res.wool.crop_vs_ts_middle.differing) && 'wool', (res.wood.vs_webgl.differing || res.wood.rects.differing_from_webgl || res.wood.rects.outside_not_base) && 'wood', res.fern.points_one_pixel_each_vs_ts_points.differing && 'fern points',
  res.fern.multisampled_vs_webgl.differing > 0.03 * res.fern.multisampled_vs_webgl.of && 'fern multisampled'].filter(Boolean);
console.log(bad.length ? 'FAIL ' + bad.join(', ') : 'all at the bar');
process.exit(bad.length ? 1 : 0);
