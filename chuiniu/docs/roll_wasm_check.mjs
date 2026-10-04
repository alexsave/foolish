// The throw's module, as the study carries it: loads the base64 cn_roll.wasm
// embedded in UI.html, bakes N seeds through the same exports the page calls,
// and prints what the C test measures. A smoke for the embed, not the test
// (that is chuiniu/c/tests/cn_roll_test.c).   node roll_wasm_check.mjs [seeds]
import fs from 'fs';
const html = fs.readFileSync(new URL('./UI.html', import.meta.url), 'utf8');
const m = html.match(/CN_ROLL_WASM_B64 = "([A-Za-z0-9+/=]+)"/);
if (!m) { console.error('UI.html carries no cn_roll.wasm: make -C chuiniu/c docs-roll'); process.exit(1); }
const bytes = Buffer.from(m[1], 'base64');
const { instance } = await WebAssembly.instantiate(bytes, {});
const e = instance.exports, N = +(process.argv[2] || 300), FF = e.cn_roll_frame_floats();
let frames = 0, ms = 0, forced = 0, incomplete = 0; const faces = [0, 0, 0, 0, 0, 0];
for (let s = 0; s < N; s++) {
  const t0 = performance.now();
  const n = e.cn_roll_run(1, 5, 179, 540, 59, 24, 34.7, 19, 339, 420, 600, 0, (1000 + s * 131) >>> 0, 0);
  ms += performance.now() - t0; frames += n; forced += e.cn_roll_forced(); if (!e.cn_roll_complete()) incomplete++;
  for (let d = 0; d < 5; d++) faces[e.cn_roll_up(d)]++;
}
const last = new Float32Array(e.memory.buffer, e.cn_roll_frames_ptr(), FF);
console.log(`cn_roll.wasm ${bytes.length} B: ${N} cup rolls, ${(ms / N).toFixed(2)} ms a bake, ${(frames / N).toFixed(0)} frames a throw, forced ${forced}, incomplete ${incomplete}, faces ${faces.join(' ')}`);
console.log(`first pose of the last bake: cup at ${[...last.slice(0, 3)].map(v => v.toFixed(1)).join(', ')} q ${[...last.slice(3, 7)].map(v => v.toFixed(3)).join(', ')}`);
process.exit(incomplete ? 1 : 0);
