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

  // the trig against Chrome's own, on arguments in the textures' ranges and beyond
  { const m = mul(seed ^ 0x5bd1e995); let ds = 0, dc = 0, dt = 0, n = 0;
    for (const span of [0.5, 4, 30, 1000, 4e5]) for (let i = 0; i < 600000; i++, n++) { const a = (m() - 0.5) * 2 * span;
      if (!Object.is(Math.sin(a), C.sin(a))) ds++; if (!Object.is(Math.cos(a), C.cos(a))) dc++; if (!Object.is(Math.tan(a), C.tan(a))) dt++; }
    r.trig = { arguments: n, sin_differing: ds, cos_differing: dc, tan_differing: dt }; }

  // wool: paintWool into a buffer, offsets as the app draws them
  { const m = mul(seed), ox = m() * 1000 - 500, oy = m() * 1000 - 500, w = 3840, h = 2160;
    const ts = new Uint8ClampedArray(w * h * 4); let t0 = performance.now();
    await TS.paintWool(ts, { width: w, height: h, offsetX: ox, offsetY: oy }); const tsMs = performance.now() - t0;
    C.reset(); t0 = performance.now(); const p = C.wool(w, h, ox, oy); const cMs = performance.now() - t0;
    r.wool = { size: w + 'x' + h, ...diff(ts, bytes(p, w * h * 4), w, h), ts_ms: Math.round(tsMs), c_ms: Math.round(cMs) }; }

  // wood: the float colours, the WebGL blit the app uses, and the CPU fallback
  { const w = 1920, h = 1080;
    Math.random = mul(seed); let t0 = performance.now(); const pd = await TS.generateWoodPixelData(w, h); const tsMs = performance.now() - t0;
    C.reset(); t0 = performance.now(); const p = C.wood(w, h, seed); const cMs = performance.now() - t0;
    const cf = new Float32Array(C.memory.buffer, C.wood_col(), w * h * 3); let fd = 0, fmax = 0;
    for (let i = 0; i < w * h * 3; i++) if (cf[i] !== pd.colors[i]) { fd++; fmax = Math.max(fmax, Math.abs(cf[i] - pd.colors[i])); }
    const mine = bytes(p, w * h * 4).slice();
    const gl = await decode(await TS.generateTextureViaWebGL(w, h, 'wood', [70 / 255, 14 / 255, 9 / 255, 1], TS.generateWoodPixelData, TS.generateWoodTextureFallback), w, h);
    const cpu = await decode(await noWebGL(TS.generateWoodTextureFallback)(w, h), w, h);
    r.wood = { size: w + 'x' + h, float_values_differing: fd + ' of ' + w * h * 3, float_max_abs: fmax,
      vs_webgl: diff(gl, mine, w, h), vs_cpu_fallback: diff(cpu, mine, w, h), ts_ms: Math.round(tsMs), c_ms: Math.round(cMs) }; }

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
const bad = [res.random !== 'same 1000 draws' && 'random', res.trig.sin_differing && 'sin', res.trig.cos_differing && 'cos',
  res.wool.differing && 'wool', res.wood.vs_webgl.differing && 'wood', res.fern.points_one_pixel_each_vs_ts_points.differing && 'fern points',
  res.fern.multisampled_vs_webgl.differing > 0.03 * res.fern.multisampled_vs_webgl.of && 'fern multisampled'].filter(Boolean);
console.log(bad.length ? 'FAIL ' + bad.join(', ') : 'all at the bar');
process.exit(bad.length ? 1 : 0);
