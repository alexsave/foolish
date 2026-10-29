// Screenshots of the gestures held mid-way (shots/, W and H for another window):
//   drag_attack   the golden's 6s dragged over the open table: the shadow and its Attack badge over
//                 the drop cell the row holds open, the dragged card's red border;
//   drag_cover    board 1's 9h held over the 7h it covers: the cell in amber, the Cover badge;
//   drag_pass     board 2's 6s and 6c, both selected, dragged out: the Pass badge, two drop cells;
//   keys_target   board 1's trump 6c aimed by the arrows at the Qd: the cursor and the red arrow;
//   keys_pass     board 2's 6c aimed at the transfer: the dashed arrow, the dashed place, PASS.
// With the textures (as they land from the store), the bot held.
import fs from 'node:fs';
import path from 'node:path';
import { launch, serve, POC, FOOLISH, openTable } from './cdp.mjs';
import { key, centre } from './input.mjs';

const W = Number(process.env.W || 1024), H = Number(process.env.H || 768);
const shots = path.join(POC, 'shots');
const sfx = W === 1024 ? '' : `_${W}x${H}`;
const server = await serve(8720, FOOLISH);
const b = await launch({ width: W, height: H, flags: ['--disable-gpu-rasterization'] });
const mouse = (type, at) => b.send('Input.dispatchMouseEvent', { type, x: at.x, y: at.y, button: 'left', buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1 });
const idle = () => b.until('window.app.then((x) => !x.app_anim_busy())', 30000);
const board = async (k) => {
  await openTable(b, `http://127.0.0.1:8720/c/cdom_poc/out/index.html?debug`, { hold: true, forget: true });
  await b.until(`window.app.then((x) => x.app_tex_ms(x.app_tex_count()) === x.app_tex_count())`, 60000);
  if (k) await b.evaluate(`window.app.then((x) => x.app_debug_board(${k}))`);
  await idle();
};
// A press on `card`, moved in steps to `to`, held there for the shot, then let go back in the hand.
const held = async (card, to, name) => {
  const from = await centre(b, `[data-hand-plane] [data-card="${card}"]`);
  await mouse('mousePressed', from);
  for (let k = 1; k <= 8; k++) await mouse('mouseMoved', { x: from.x + (to.x - from.x) * k / 8, y: from.y + (to.y - from.y) * k / 8 });
  await new Promise((r) => setTimeout(r, 300));   // the hand's 0.1 s transitions
  await b.shot(path.join(shots, `${name}${sfx}.png`));
  await mouse('mouseMoved', from);
  await mouse('mouseReleased', from);
};
try {
  await board(0);
  await held('0-5', { x: W / 2, y: H * 0.39 }, 'drag_attack');
  await board(1);
  await held('1-8', await centre(b, '[data-table-container] [data-battle-index][data-card="1-6"]'), 'drag_cover');
  await board(2);
  await b.click('[data-hand-plane] [data-card="0-5"]');
  await b.click('[data-hand-plane] [data-card="2-5"]');
  await held('0-5', { x: W / 2, y: H * 0.39 }, 'drag_pass');
  await board(1);
  for (let i = 0; i < 3; i++) await key(b, 'ArrowRight');
  await key(b, 'ArrowUp');
  await key(b, 'ArrowRight');
  await new Promise((r) => setTimeout(r, 300));
  await b.shot(path.join(shots, `keys_target${sfx}.png`));
  await board(2);
  await key(b, 'ArrowRight');
  await key(b, 'ArrowUp');
  await key(b, 'ArrowRight');
  await new Promise((r) => setTimeout(r, 300));
  await b.shot(path.join(shots, `keys_pass${sfx}.png`));
  console.log(`shots/{drag_attack,drag_cover,drag_pass,keys_target,keys_pass}${sfx}.png`);
} finally {
  await b.close();
  server.close();
}
