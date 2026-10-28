// Screenshots: the cdom table mid-game, the same table after one move and the bot's
// answer, and the React golden (e2e/fixtures/ui_dom/bout_2p_attacker.html) under the same CSS.
import fs from 'node:fs';
import path from 'node:path';
import { launch, serve, POC, FOOLISH } from './cdp.mjs';

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
<link href="/src/styles/index.css" rel="stylesheet"/></head><body><div id="root">${goldenFixed}</div></body></html>`;

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

  await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html');
  await b.until(`document.querySelector('[data-game-container]')?.dataset.turn === 'human'`);
  await b.evaluate('document.fonts.ready.then(() => 1)');
  await b.shot(path.join(shots, `cdom_midgame${sfx}.png`));

  // Select 6s: the Attack pill shows.
  await b.click('[data-location="hand"][data-card="0-5"]');
  await b.until(`document.querySelector('[data-pill="Attack"]')`);
  await b.shot(path.join(shots, `cdom_selected${sfx}.png`));
  // Throw it in, and wait for the bot to answer.
  await b.click('[data-pill="Attack"]');
  await b.until(`document.querySelector('[data-game-container]').dataset.turn === 'bot'`);
  await b.until(`document.querySelector('[data-game-container]').dataset.turn !== 'bot'`, 10000);
  await new Promise((r) => setTimeout(r, 400)); // the hand's own 0.1s slide (transition: all 0.1s)
  await b.shot(path.join(shots, `cdom_after_bot${sfx}.png`));
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
  console.log(fs.readdirSync(shots).map((f) => path.join(shots, f)).join('\n'));
} finally {
  await b.close();
  server.close();
}
