// The reference for `make tex-compare`: the study's own TEX3 textures (docs/UI.html), captured in headless
// Chromium as raw RGBA, and their bumps as int8 at a 20th exactly as the page uploads them (NAME_WxH.rgba / .bump).
// A measuring harness: never shipped, never run by `make run`; it needs the network for the study's Google Fonts.
//   npm i playwright-core   (resolvable from here)
//   CHROME=/path/to/chrome node tools/cn_tex_capture.mjs ../docs/UI.html /abs/out/dir
//   make tex-compare REF=/abs/out/dir
import { chromium } from 'playwright-core';
import fs from 'fs';
const SRC = process.argv[2], OUT = process.argv[3];
if (!SRC || !OUT || !process.env.CHROME) { console.error('usage: CHROME=/path/to/chrome node cn_tex_capture.mjs UI.html OUTDIR'); process.exit(2); }
fs.mkdirSync(OUT, { recursive: true });
let html = fs.readFileSync(SRC, 'utf8').split('\n');
// expose TEX3 and matCanvas (both live inside main()) on a copy of the page, after TEX3's closing line
const idx = html.findIndex(l => l.startsWith('    return { cupSide, cupCrown'));
if (idx < 0) { console.error('TEX3 not found in ' + SRC); process.exit(1); }
html.splice(idx + 2, 0, '  window.TEX3 = TEX3; window.matCanvas = matCanvas;');
fs.writeFileSync(OUT + '/UI_exposed.html', html.join('\n'));
const browser = await chromium.launch({ executablePath: process.env.CHROME });
const page = await browser.newPage();
page.on('pageerror', e => console.error('pageerror', e.message));
await page.goto('file://' + OUT + '/UI_exposed.html', { waitUntil: 'load' });
await page.waitForFunction(() => window.TEX3 && document.fonts.status === 'loaded', null, { timeout: 60000 });
const fontOk = await page.evaluate(async () => { await document.fonts.load("112px 'IM Fell English'"); await document.fonts.load("184px 'IM Fell English'"); return document.fonts.check("112px 'IM Fell English'"); });
console.log('font loaded', fontOk);
const items = await page.evaluate(() => {
  TEX3.reset();
  const b64 = u8 => { let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000)); return btoa(s); };
  const rgba = c => b64(new Uint8Array(c.getContext('2d').getImageData(0, 0, c.width, c.height).data.buffer));
  const bump = c => { const n = c._bump.n, b = new Int8Array(n.length); for (let i = 0; i < n.length; i++) { const v = Math.round(n[i] * 20); b[i] = v > 127 ? 127 : v < -127 ? -127 : v; } return b64(new Uint8Array(b.buffer)); };
  const out = [];
  const put = (name, c, withBump) => out.push({ name, w: c.width, h: c.height, rgba: rgba(c), bump: withBump && c._bump ? bump(c) : null });
  put('verd', matCanvas['c-verdigris']); put('bone', matCanvas['m-tallow']);
  for (const s of [1, 7, 42]) {
    put('side_' + s, TEX3.cupSide(s), 1); put('inner_' + s, TEX3.cupInner(s), 1); put('floor_' + s, TEX3.cupFloor(s), 1);
    put('crown_' + s + '_c3_112', TEX3.cupCrown(s, 3, false, 112), 1); put('crown_' + s + '_c5_184', TEX3.cupCrown(s, 5, false, 184), 1);
    put('crown_' + s + '_out', TEX3.cupCrown(s, 0, true, 112), 1); put('crown_' + s + '_none', TEX3.cupCrown(s, null, false, 112), 1);
    put('die_' + s, TEX3.dieAtlas(s), 1);
  }
  // glyph metrics as the canvas sees them
  const g = document.createElement('canvas').getContext('2d'); const met = {};
  for (const px of [112, 184]) { g.font = `${px}px 'IM Fell English', serif`; g.textAlign = 'center'; g.textBaseline = 'middle';
    for (let d = 0; d <= 9; d++) { const m = g.measureText(String(d)); met[px + ':' + d] = [m.width, m.actualBoundingBoxLeft, m.actualBoundingBoxRight, m.actualBoundingBoxAscent, m.actualBoundingBoxDescent, m.fontBoundingBoxAscent, m.fontBoundingBoxDescent]; }
    g.textBaseline = 'alphabetic'; const m = g.measureText('0'); met[px + ':alpha'] = [m.fontBoundingBoxAscent, m.fontBoundingBoxDescent, m.emHeightAscent, m.emHeightDescent]; }
  // glyph masks on black: white text at the crown's anchor
  for (const px of [112, 184]) for (let d = 0; d <= 9; d++) {
    const c = document.createElement('canvas'); c.width = 256; c.height = 256; const x = c.getContext('2d');
    x.fillStyle = '#000'; x.fillRect(0, 0, 256, 256); x.font = `${px}px 'IM Fell English', serif`; x.textAlign = 'center'; x.textBaseline = 'middle'; x.fillStyle = '#fff'; x.fillText(String(d), 128, 136);
    put('glyph_' + px + '_' + d, c, 0);
  }
  return { out, met };
});
for (const it of items.out) {
  fs.writeFileSync(`${OUT}/${it.name}_${it.w}x${it.h}.rgba`, Buffer.from(it.rgba, 'base64'));
  if (it.bump) fs.writeFileSync(`${OUT}/${it.name}_${it.w}x${it.h}.bump`, Buffer.from(it.bump, 'base64'));
}
fs.writeFileSync(OUT + '/metrics.txt', Object.entries(items.met).map(([k, v]) => k + ' ' + v.join(' ')).join('\n') + '\n');
console.log('wrote', items.out.length, 'textures');
await browser.close();
