#!/usr/bin/env python3
# Mutation check for cdom_poc's tests (python3 tools/mutate.py [name ...]): each mutant edits one file,
# rebuilds, runs the test that should catch it, and prints what failed;
# and the file is restored from a copy taken first (never git checkout).
import subprocess, shutil, sys, os
POC = '/Users/alex/Dev/foolish-cdom-port/foolish/c/cdom_poc'
MUTANTS = [
  ('face radius', 'src/table_app.c', 'sb_s(&s, "border-radius: 5px; border: 2px solid ");', 'sb_s(&s, "border-radius: 4px; border: 2px solid ");', 'tools/test.mjs'),
  ('bot never chooses', 'src/kernel_glue.c', 'int chosen = handwritten_prod_strategy_choose(g, seat, moves, 0);', 'int chosen = -1;', 'tools/test.mjs'),
  ('hand rebuilt on every render', 'src/table_app.c', '  cd_list_update(&hand, keys, (uint32_t)V->my_hand_count, &HAND_FNS);', '  cd_clear(hand_plane); cd_list_forget(&hand, &HAND_FNS);\n  cd_list_update(&hand, keys, (uint32_t)V->my_hand_count, &HAND_FNS);', 'tools/test.mjs'),
  ('hand patch writes no border', 'src/table_app.c', '(shown[cid].sel = sl) ? ZS("red") : ZS("black")', '(shown[cid].sel = sl) ? ZS("black") : ZS("black")', 'tools/test.mjs'),
  ('shim clone not attached', 'vendor/cdom.js', 'function clone(t) { push(tpl[t].cloneNode(true)); }', 'function clone(t) { const el = tpl[t].cloneNode(true); stack[sp++] = el; last = el; }', 'tools/test.mjs'),
  ('no end screen', 'src/table_app.c', '  if (!cd_region_show(&end_screen, V->status == GAME_STATUS_GAME_OVER)) return;', '  if (!cd_region_show(&end_screen, 0)) return;', 'tools/test.mjs'),
  ('no resize listener', 'src/table_app.c', '  cd_listen_window(CD_EV_RESIZE);', '', 'tools/test.mjs'),
  ('count badge not anchored', 'src/table_app.c', '      u->minis.end = u->badge.shown ? u->badge.root : 0;', '      u->minis.end = 0;', 'tools/test.mjs'),
  ('no fern background', 'src/table_app.c', '    if (e->key == TEX_FERN) cd_set_style(CD_MOUNT, ZS("--tex-fern-bg"), ZS("#000000"));', '', 'tools/test.mjs'),
  ('styles interned', 'src/ui.h', '  return cd_tmp(s->b, n);', '  return cd_intern(s->b, n);', 'tools/strings.mjs'),
  ('wood edge', 'src/textures.c', '          double ea = 0.1 * edge, iea = 1 - ea;', '          double ea = 0.1 * edge, iea = 1.0000001 - ea;', 'tools/texparity.mjs'),
  ('wool clamp truncates', 'src/textures.c', '  if (f + 0.5 < v) return (uint8_t)(f + 1);', '', 'tools/texparity.mjs'),
  ('fern samples', 'src/textures.c', '{{0.375, 0.875}, {0.875, 0.625}, {0.125, 0.375}, {0.625, 0.125}}', '{{0.375, 0.125}, {0.875, 0.375}, {0.125, 0.625}, {0.625, 0.875}}', 'tools/texparity.mjs'),
  ('cos not rounded', 'src/crtrig.c', '  return fast_two_sum(p.hi, p.lo + (a.hi * b.lo + a.lo * b.hi));', '  return fast_two_sum(p.hi, p.lo + a.hi * b.lo);', 'tools/texparity.mjs'),
  ('ws message bytes shifted', 'vendor/cdom.js', 'new Uint8Array(mem.buffer, p, b.length).set(b); f(a, p, b.length);', 'new Uint8Array(mem.buffer, p, b.length).set(b.map((v) => v ^ 1)); f(a, p, b.length);', 'tests/net_test.mjs'),
  ('http body not sent', 'vendor/cdom.js', 'body: bn ? new Uint8Array(mem.buffer, bp, bn).slice() : undefined', 'body: undefined', 'tests/net_test.mjs'),
]
only = sys.argv[1:] 
for name, f, old, new, test in MUTANTS:
  if only and name not in only: continue
  path = os.path.join(POC, f); keep = path + '.keep'
  src = open(path).read()
  assert src.count(old) == 1, f'{name}: anchor not found once'
  shutil.copy(path, keep)
  try:
    open(path, 'w').write(src.replace(old, new))
    b = subprocess.run(['./build.sh'], cwd=POC, capture_output=True, text=True)
    if b.returncode: print(f'{name}: BUILD FAILED\n{b.stderr[-800:]}'); continue
    r = subprocess.run(['node', test], cwd=POC, capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.startswith('FAIL') or 'ok' in l or 'failed' in l][:12]
    print(f'== {name}: exit {r.returncode}')
    for l in r.stdout.splitlines():
      if l.startswith('FAIL') or l.startswith('     ') or l.startswith('ws') or l.startswith('http') or l.startswith('game 3') or l.startswith('all at'): print('   ', l[:220])
  finally:
    shutil.move(keep, path)
b = subprocess.run(['./build.sh'], cwd=POC, capture_output=True, text=True)
print('restored and rebuilt:', b.returncode == 0)
