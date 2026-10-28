// Screenshots: the cdom table mid-game, the same table after one move and the bot's
// answer, and the React golden (e2e/fixtures/ui_dom/bout_2p_attacker.html) under the same CSS.
// First without textures (?notex: the golden has none), then with them: the cdom table once its
// three textures have landed, and the golden with the app's own TS textures made from the same
// seed and applied as the React components apply them (needs out/tex_ts.js: tools/texparity.mjs).
import fs from 'node:fs';
import path from 'node:path';
import { launch, serve, POC, FOOLISH } from './cdp.mjs';
import { playToEnd } from './play.mjs';

const W = Number(process.env.W || 1024), H = Number(process.env.H || 768);
const shots = path.join(POC, 'shots');
fs.mkdirSync(shots, { recursive: true });

// The golden page: the recorded React DOM inside the page the app renders into.
const golden = fs.readFileSync(path.join(FOOLISH, 'e2e/fixtures/ui_dom/bout_2p_attacker.html'), 'utf8');
// jsdom drops the action area's bottom: max(10px, env(safe-area-inset-bottom)) (it cannot parse
// max() or env()), so the golden's hand would stand at the top of the screen. It is put back,
// exactly as ActionButtons.tsx writes it, and nothing else in the golden is touched.
const ACTION = 'position: absolute; left: 0px; right: 0px; justify-content: end;';
if (!golden.includes(ACTION)) throw new Error('the golden action area moved');
const goldenFixed = golden.replace(ACTION, 'position: absolute; bottom: max(10px, env(safe-area-inset-bottom)); left: 0px; right: 0px; justify-content: end;');
const goldenPage = `<!DOCTYPE html><html lang="en" data-theme="default"><head><meta charset="utf-8"/>
<link href="/src/styles/index.css" rel="stylesheet"/><script src="/c/cdom_poc/out/tex_ts.js"></script></head><body><div id="root">${goldenFixed}</div></body></html>`;

// In the golden page: the TS textures from seed S, in the draw order the C uses, put where
// WoolBackgroundLayer, getTextureStyle, CardBack and PlayerRing put them.
const TEXTURE_GOLDEN = (S) => `(async () => {
  const mul = (seed) => { let a = seed >>> 0; return () => { a = (a + 0x6D2B79F5) >>> 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; };
  const real = Math.random;
  Math.random = mul(${S}); const fern = await TS.generateFernPattern(TS.createCardParams(1000, 1400), { width: 1000, height: 1400, bypassCache: true });
  Math.random = mul(${S}); const wood = await TS.generateTextureViaWebGL(1920, 1080, 'wood', [70 / 255, 14 / 255, 9 / 255, 1], TS.generateWoodPixelData, TS.generateWoodTextureFallback);
  Math.random = real;
  const m = mul(${S}); m(); m(); const ox = m() * 1000 - 500, oy = m() * 1000 - 500;
  const cv = document.createElement('canvas'); cv.width = 3840; cv.height = 2160; const cx = cv.getContext('2d');
  const im = cx.createImageData(3840, 2160); await TS.paintWool(im.data, { width: 3840, height: 2160, offsetX: ox, offsetY: oy }); cx.putImageData(im, 0, 0);
  const wool = URL.createObjectURL(await new Promise((r) => cv.toBlob(r, 'image/jpeg', 0.92)));
  document.querySelector('.bg-wool').style.backgroundImage = 'url(' + wool + ')';
  for (const e of document.querySelectorAll('#root [style]')) {
    if (e.style.backgroundImage.startsWith('repeating-linear-gradient(90deg')) e.style.backgroundImage = 'url(' + wood + ')';
    if (e.style.backgroundColor === 'rgb(180, 14, 9)') { e.style.backgroundColor = '#000000'; e.style.backgroundImage = 'url(' + fern + ')'; }
  }
  await Promise.all([wool, wood, fern].map((u) => { const i = new Image(); i.src = u; return i.decode(); }));
  return 1; })()`;

const server = await serve(8720, FOOLISH, (req, res) => {
  if (req.url !== '/golden.html') return false;
  res.writeHead(200, { 'content-type': 'text/html' }); res.end(goldenPage); return true;
});
const b = await launch({ width: W, height: H });
try {
  const sfx = W === 1024 ? '' : `_${W}x${H}`;
  await b.goto('http://127.0.0.1:8720/golden.html');
  await b.evaluate('document.fonts.ready.then(() => 1)');
  await b.shot(path.join(shots, `react_golden${sfx}.png`));

  for (const tex of [false, true]) {
  const pre = tex ? 'cdom_tex_' : 'cdom_';
  await b.goto(`http://127.0.0.1:8720/c/cdom_poc/out/index.html${tex ? '' : '?notex'}`);
  await b.until(`document.querySelector('[data-game-container]')?.dataset.turn === 'human'`);
  await b.evaluate('document.fonts.ready.then(() => 1)');
  if (tex) { // every texture landed and decoded
    await b.until(`window.app.then((x) => x.app_tex_ms(3) === 3)`, 60000);
    await b.until(`document.getElementById('root').style.getPropertyValue('--tex-wool') !== ''`, 10000);
    await b.evaluate(`Promise.all(['--tex-fern', '--tex-wood', '--tex-wool'].map((v) => { const i = new Image();
      i.src = document.getElementById('root').style.getPropertyValue(v).slice(4, -1); return i.decode(); })).then(() => new Promise((r) => setTimeout(r, 300)))`);
    const seed = await b.evaluate('window.app.then((x) => x.app_tex_seed())');
    await b.shot(path.join(shots, `${pre}midgame${sfx}.png`));
    await b.goto('http://127.0.0.1:8720/golden.html');
    await b.evaluate('document.fonts.ready.then(() => 1)');
    await b.evaluate(TEXTURE_GOLDEN(seed));
    await new Promise((r) => setTimeout(r, 300));
    await b.shot(path.join(shots, `react_golden_tex${sfx}.png`));
    await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html');
    await b.until(`document.querySelector('[data-game-container]')?.dataset.turn === 'human'`);
    await b.until(`window.app.then((x) => x.app_tex_ms(3) === 3)`, 60000);
    await b.until(`document.getElementById('root').style.getPropertyValue('--tex-wool') !== ''`, 10000);
    await new Promise((r) => setTimeout(r, 300));
  } else await b.shot(path.join(shots, `${pre}midgame${sfx}.png`));

  // Select 6s: the Attack pill shows.
  await b.click('[data-location="hand"][data-card="0-5"]');
  await b.until(`document.querySelector('[data-pill="Attack"]')`);
  await b.shot(path.join(shots, `${pre}selected${sfx}.png`));
  // Throw it in, and wait for the bot to answer.
  await b.click('[data-pill="Attack"]');
  await b.until(`document.querySelector('[data-game-container]').dataset.turn === 'bot'`);
  await b.until(`document.querySelector('[data-game-container]').dataset.turn !== 'bot'`, 10000);
  await new Promise((r) => setTimeout(r, 400)); // the hand's own 0.1s slide (transition: all 0.1s)
  await b.shot(path.join(shots, `${pre}after_bot${sfx}.png`));
  // Played out to the end screen.
  await playToEnd(b);
  await b.shot(path.join(shots, `${pre}end${sfx}.png`));
  }
  // How many pixels differ between two shots: the React golden against the cdom first paint, and,
  // as the measure's own check, against the shot with a card selected (which must differ).
  const differ = (x, y) => b.evaluate(`(async () => {
    const load = (src) => new Promise((r, j) => { const i = new Image(); i.onload = () => r(i); i.onerror = j; i.src = src; });
    const px = async (src) => { const i = await load(src); const c = new OffscreenCanvas(i.width, i.height);
      const g = c.getContext('2d'); g.drawImage(i, 0, 0); return g.getImageData(0, 0, i.width, i.height).data; };
    const a = await px('data:image/png;base64,${fs.readFileSync(path.join(shots, x)).toString('base64')}');
    const b = await px('data:image/png;base64,${fs.readFileSync(path.join(shots, y)).toString('base64')}');
    if (a.length !== b.length) return -1;
    let n = 0; for (let i = 0; i < a.length; i += 4) if (a[i] !== b[i] || a[i + 1] !== b[i + 1] || a[i + 2] !== b[i + 2]) n++;
    return n; })()`);
  console.log(`pixels differing, React golden vs cdom first paint: ${await differ(`react_golden${sfx}.png`, `cdom_midgame${sfx}.png`)} of ${W * H}`);
  console.log(`pixels differing, React golden vs cdom with 6s selected: ${await differ(`react_golden${sfx}.png`, `cdom_selected${sfx}.png`)}`);
  console.log(`pixels differing, React golden with the TS textures vs cdom with its C textures: ${await differ(`react_golden_tex${sfx}.png`, `cdom_tex_midgame${sfx}.png`)} of ${W * H}`);
  console.log(fs.readdirSync(shots).map((f) => path.join(shots, f)).join('\n'));
} finally {
  await b.close();
  server.close();
}
