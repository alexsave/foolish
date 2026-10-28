// The cdom table's DOM after load against the React golden (e2e/fixtures/ui_dom/bout_2p_attacker.html),
// both serialized by the same browser, one tag per line; prints a unified diff.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { execFileSync } from 'node:child_process';
import { launch, serve, FOOLISH } from './cdp.mjs';
const golden = fs.readFileSync(path.join(FOOLISH, 'e2e/fixtures/ui_dom/bout_2p_attacker.html'), 'utf8');
const server = await serve(8720, FOOLISH);
const b = await launch();
try {
  await b.goto('http://127.0.0.1:8720/c/cdom_poc/out/index.html');
  await b.until(`document.querySelector('[data-game-container]')?.dataset.turn === 'human'`);
  const lines = (html) => html.replace(/></g, '>\n<').split('\n');
  const mine = await b.evaluate(`document.getElementById('root').innerHTML`);
  const theirs = await b.evaluate(`(() => { const d = document.createElement('div'); d.innerHTML = ${JSON.stringify(golden)}; return d.innerHTML; })()`);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'domdiff-'));
  fs.writeFileSync(path.join(dir, 'react.html'), lines(theirs).join('\n') + '\n');
  fs.writeFileSync(path.join(dir, 'cdom.html'), lines(mine).join('\n') + '\n');
  try { execFileSync('diff', ['-u', 'react.html', 'cdom.html'], { cwd: dir, stdio: 'inherit' }); console.log('identical'); }
  catch { /* diff exits 1 on a difference; it printed it */ }
  fs.rmSync(dir, { recursive: true, force: true });
} finally { await b.close(); server.close(); }
