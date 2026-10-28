#!/usr/bin/env python3
# Mutation check for cdom_poc's tests (python3 tools/mutate.py [name ...]): each mutant edits one file,
# rebuilds, runs the test that should catch it, and prints what failed;
# and the file is restored from a copy taken first (never git checkout).
import subprocess, shutil, sys, os
POC = '/Users/alex/Dev/foolish-cdom-port/foolish/c/cdom_poc'
MUTANTS = [
  ('face radius', 'src/table_app.c', 'sb_s(&s, "border-radius: 5px; border: 2px solid ");', 'sb_s(&s, "border-radius: 4px; border: 2px solid ");', 'tools/test.mjs'),
  ('bot never chooses', 'src/kernel_glue.c', 'int chosen = handwritten_prod_strategy_choose(g, seat, moves, 0);', 'int chosen = -1;', 'tools/test.mjs'),
  ('hand rebuilt on every render', 'src/table_app.c', '  cd_list_update(&hand, keys, (uint32_t)V->my_hand_count, &HAND_FNS);', '  cd_clear(hand_plane); list_forget(&hand);\n  cd_list_update(&hand, keys, (uint32_t)V->my_hand_count, &HAND_FNS);', 'tools/test.mjs'),
  ('hand patch writes no style', 'src/table_app.c', '  if (s != hand_style[cid]) cd_set_attr(it->node, S_STYLE, hand_style[cid] = s);', '  (void)s;', 'tools/test.mjs'),
  ('shim clone not attached', 'vendor/cdom.js', 'if (sp) stack[sp - 1].appendChild(el); stack[sp++] = el; last = el; }', 'stack[sp++] = el; last = el; }', 'tools/test.mjs'),
  ('no end screen', 'src/table_app.c', '  int over = V->status == GAME_STATUS_GAME_OVER;', '  int over = 0;', 'tools/test.mjs'),
  ('no resize listener', 'src/index.html', " window.addEventListener('resize', () => x.app_resize());", '', 'tools/test.mjs'),
  ('ws message bytes shifted', 'vendor/cdom.js', 'new Uint8Array(mem.buffer, p, b.length).set(b);', 'new Uint8Array(mem.buffer, p, b.length).set(b.map((v) => v ^ 1));', 'tests/net_test.mjs'),
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
      if l.startswith('FAIL') or l.startswith('     ') or l.startswith('ws') or l.startswith('http'): print('   ', l[:220])
  finally:
    shutil.move(keep, path)
b = subprocess.run(['./build.sh'], cwd=POC, capture_output=True, text=True)
print('restored and rebuilt:', b.returncode == 0)
