// film.mjs - what a page does over time, frame by frame, for evidence (README, Animations).
//
// film(b, play) records every frame the compositor produces while play() runs (CDP
// Page.startScreencast, each frame acknowledged so the next one comes), with the frame's own
// timestamp. writeFilm(name, frames) keeps them: every frame as a PNG in out/anim/<name>/ (not
// committed), a contact sheet of one frame every `step` ms with its time written on it (drawn by
// Chrome itself in a canvas) and a GIF timed as recorded (ffmpeg), both in shots/anim/.
//
//   node tools/film.mjs [scenario ...]   (after ./build.sh)
// Scenarios, on the golden's board with the bot held and its waits fast, the flights at 1x:
//   cover-sweep   the bot covers 6d with the trump 6c (the attack turns under it), I say Good: the
//                 table sweeps to the discard pile and both hands refill from the stock
//   attack-take   the bot covers, I throw in 6s, the bot takes the table (to its seat), and the refill
//   deal          a new deal, one card at a time (Play again's), the trump turned last
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { launch, serve, FOOLISH, POC, openTable, sleep } from './cdp.mjs';

/** Records the frames the page paints while `play` runs: [{ t (ms from the first), png }]. */
export async function film(b, play, { settleMs = 300 } = {}) {
  const frames = [];
  const off = b.listen('Page.screencastFrame', (f) => {
    frames.push({ ts: f.metadata.timestamp * 1000, data: f.data });
    b.send('Page.screencastFrameAck', { sessionId: f.sessionId }).catch(() => {});
  });
  await b.send('Page.startScreencast', { format: 'png', everyNthFrame: 1 });
  try {
    await play();
    await sleep(settleMs);
  } finally {
    await b.send('Page.stopScreencast');
    off();
  }
  // The screencast sends a frame only when the page changes: the first is the page as it stood,
  // and the film starts 100 ms before the first change after it.
  const t0 = frames.length > 1 ? frames[1].ts - 100 : frames.length ? frames[0].ts : 0;
  return frames.map((f, i) => ({ t: i ? f.ts - t0 : 0, png: Buffer.from(f.data, 'base64') }));
}

/** Keeps a film: out/anim/<name>/ every frame, shots/anim/<name>_sheet.png and <name>.gif. */
export async function writeFilm(b, name, frames, { step = 50, cols = 6, width = 300, from = 0, to = Infinity } = {}) {
  const all = path.join(POC, 'out/anim', name);
  fs.rmSync(all, { recursive: true, force: true });
  fs.mkdirSync(all, { recursive: true });
  frames.forEach((f, i) => fs.writeFileSync(path.join(all, `${String(i).padStart(4, '0')}_${Math.round(f.t)}ms.png`), f.png));
  const shots = path.join(POC, 'shots/anim');
  fs.mkdirSync(shots, { recursive: true });
  // The contact sheet: the last frame painted at or before every `step` ms, its time on it.
  const picked = [];
  for (let at = from, i = 0; at <= Math.min(to, frames.at(-1).t); at += step) {
    while (i + 1 < frames.length && frames[i + 1].t <= at) i++;
    picked.push({ at, png: frames[i].png });
  }
  const urls = picked.map((p) => 'data:image/png;base64,' + p.png.toString('base64'));
  const sheet = await b.evaluate(`(async () => {
    const urls = ${JSON.stringify(urls)}, ats = ${JSON.stringify(picked.map((p) => p.at))};
    const imgs = await Promise.all(urls.map((u) => new Promise((r) => { const i = new Image(); i.onload = () => r(i); i.src = u; })));
    const w = ${width}, h = Math.round(w * imgs[0].height / imgs[0].width), cols = ${cols}, rows = Math.ceil(imgs.length / cols);
    const c = document.createElement('canvas'); c.width = cols * w; c.height = rows * h;
    const g = c.getContext('2d');
    g.fillStyle = '#000'; g.fillRect(0, 0, c.width, c.height);
    imgs.forEach((im, k) => {
      const x = (k % cols) * w, y = Math.floor(k / cols) * h;
      g.drawImage(im, x, y, w - 2, h - 2);
      g.fillStyle = 'rgba(0,0,0,0.7)'; g.fillRect(x, y, 70, 18);
      g.fillStyle = '#fff'; g.font = '13px monospace'; g.fillText(ats[k] + ' ms', x + 4, y + 13);
    });
    return c.toDataURL('image/png').slice(22);
  })()`);
  fs.writeFileSync(path.join(shots, `${name}_sheet.png`), Buffer.from(sheet, 'base64'));
  // The GIF, each frame shown for as long as it was on screen.
  const list = path.join(all, 'list.txt');
  const used = frames.filter((f) => f.t >= from && f.t <= to);
  fs.writeFileSync(list, used.map((f, i) => {
    const file = path.join(all, `${String(frames.indexOf(f)).padStart(4, '0')}_${Math.round(f.t)}ms.png`);
    const next = used[i + 1] ? used[i + 1].t - f.t : 100;
    return `file '${file}'\nduration ${(next / 1000).toFixed(4)}`;
  }).join('\n') + `\nfile '${path.join(all, `${String(frames.indexOf(used.at(-1))).padStart(4, '0')}_${Math.round(used.at(-1).t)}ms.png`)}'\n`);
  execFileSync('ffmpeg', ['-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', list,
    '-vf', `fps=30,scale=${width * 2}:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=128[p];[b][p]paletteuse=dither=bayer`,
    path.join(shots, `${name}.gif`)]);
  return { frames: frames.length, sheet: picked.length, dir: all };
}

// ---- the scenarios ------------------------------------------------------------------------------
const URL_TABLE = 'http://127.0.0.1:8720/c/cdom_poc/out/index.html?notex&debug';
const idle = (b) => b.until('window.app.then((x) => !x.app_anim_busy())', 30000);
const release = async (b) => {
  await b.until('window.app.then((x) => x.app_debug_due())', 15000);
  const n = await b.evaluate('window.app.then((x) => { const n = x.app_bot_steps(); x.app_debug_release(); return n; })');
  await b.until(`window.app.then((x) => x.app_bot_steps() > ${n})`);
};
const SCENARIOS = {
  'cover-sweep': async (b) => {
    await release(b); await idle(b);                      // the bot covers 6d with 6c
    await b.click('[data-pill="Good"]'); await idle(b);   // the sweep and the refill
  },
  'attack-take': async (b) => {
    await release(b); await idle(b);
    await b.click('[data-hand-plane] [data-card="0-5"]');
    await b.until(`document.querySelector('[data-pill="Attack"]')`);
    await b.click('[data-pill="Attack"]'); await idle(b);
    await release(b); await idle(b);                      // the bot takes the table, and the refill
  },
  deal: async (b) => {
    await b.evaluate('window.app.then((x) => x.app_debug_deal())');
    await idle(b);
  },
};

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  const names = process.argv.slice(2).length ? process.argv.slice(2) : Object.keys(SCENARIOS);
  const server = await serve(8720, FOOLISH);
  const b = await launch();
  try {
    for (const name of names) {
      await openTable(b, URL_TABLE, { hold: true, fast: true });
      await sleep(300);
      const frames = await film(b, () => SCENARIOS[name](b));
      const r = await writeFilm(b, name, frames);
      console.log(`${name}: ${r.frames} frames over ${Math.round(frames.at(-1).t)} ms, sheet of ${r.sheet} (shots/anim/${name}_sheet.png, ${name}.gif)`);
    }
  } finally { await b.close(); server.close(); }
}
