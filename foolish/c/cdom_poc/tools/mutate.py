#!/usr/bin/env python3
# Mutation check for cdom_poc's tests (python3 tools/mutate.py [name ...]): each mutant edits one file,
# rebuilds, runs the test that should catch it, and prints what failed;
# and the file is restored from a copy taken first (never git checkout).
import subprocess, shutil, sys, os, tempfile
POC = '/Users/alex/Dev/foolish-cdom-port/foolish/c/cdom_poc'
MUTANTS = [
  ('face radius', 'src/table_app.c', 'sb_s(&s, "border-radius: 5px; border: 2px solid ");', 'sb_s(&s, "border-radius: 4px; border: 2px solid ");', 'tools/test.mjs'),
  ('bot never chooses', 'src/kernel_glue.c', 'int chosen = handwritten_prod_strategy_choose(g, seat, moves, 0);', 'int chosen = -1;', 'tools/test.mjs'),
  ('hand rebuilt on every render', 'src/table_app.c', '  cd_list_update(&hand, keys, (uint32_t)V->my_hand_count, &HAND_FNS);', '  cd_clear(hand_plane); cd_list_forget(&hand, &HAND_FNS);\n  cd_list_update(&hand, keys, (uint32_t)V->my_hand_count, &HAND_FNS);', 'tools/test.mjs'),
  ('hand patch writes no border', 'src/table_app.c', '(shown[cid].sel = sl) ? ZS("red") : ZS("black")', '(shown[cid].sel = sl) ? ZS("black") : ZS("black")', 'tools/test.mjs'),
  ('shim clone not attached', 'vendor/cdom.js', '    push(templates[templateId].cloneNode(true));', '    const element = templates[templateId].cloneNode(true);\n    stack[depth++] = element;\n    last = element;', 'tools/test.mjs'),
  ('no end screen', 'src/table_app.c', '  if (!cd_region_show(&end_screen, V->status == GAME_STATUS_GAME_OVER)) return;', '  if (!cd_region_show(&end_screen, 0)) return;', 'tools/test.mjs'),
  ('no resize listener', 'src/table_app.c', '  cd_listen_window(CD_EV_RESIZE);', '', 'tools/test.mjs'),
  ('count badge not anchored', 'src/table_app.c', '      u->minis.end = u->badge.shown ? u->badge.root : 0;', '      u->minis.end = 0;', 'tools/test.mjs'),
  ('no fern background', 'src/table_app.c', '  if (k == TEX_FERN) cd_set_style(CD_MOUNT, ZS("--tex-fern-bg"), ZS("#000000"));', '', 'tools/test.mjs'),
  ('styles interned', 'src/ui.h', '  return cd_tmp(s->b, n);', '  return cd_intern(s->b, n);', 'tools/strings.mjs'),
  ('wood edge', 'src/textures.c', '          double ea = 0.1 * edge, iea = 1 - ea;', '          double ea = 0.1 * edge, iea = 1.0000001 - ea;', 'tools/texparity.mjs'),
  ('wool clamp truncates', 'src/textures.c', '  return (uint8_t)__builtin_rint(v);', '  return (uint8_t)v;', 'tools/texparity.mjs'),
  ('wool skips too much', 'src/textures.c', 'if (t->r < x0 - 9 || t->r > x1 + 9) {', 'if (t->r < x0 + 9 || t->r > x1 - 9) {', 'tools/texparity.mjs'),
  ('wood drops the row above', 'src/textures.c', 'int here = a0 < a1, wrapped = xpos < 0 && b0 < b1;', 'int here = a0 < a1, wrapped = 0;', 'tools/texparity.mjs'),
  ('trig reduction rounded', 'src/trig.c', 'double r = (x - k * P1) - k * P2;', 'double r = x - k * (P1 + P2);', 'tools/texparity.mjs'),
  ('pill wood too narrow', 'src/table_app.c', 'enum { PILL_SPAN_W = 76, PILL_SPAN_H = 52 };', 'enum { PILL_SPAN_W = 40, PILL_SPAN_H = 52 };', 'tools/test.mjs'),
  ('results wood too short', 'src/table_app.c', '{SEED_RESULTS, 360, 100}', '{SEED_RESULTS, 360, 60}', 'tools/test.mjs'),
  ('store ignored', 'src/table_app.c', '  if (len > 0) { /* kept from an earlier load */', '  if (0) {', 'tools/test.mjs'),
  ('wool placed 16 px off', 'src/table_app.c', 'TEX_W, TEX_H, WOOL_X, WOOL_Y, ZS("image/jpeg")', 'TEX_W, TEX_H, WOOL_X + 16, WOOL_Y, ZS("image/jpeg")', 'tools/shots.mjs'),
  ('wool not placed', 'src/table_app.c', 'cd_encode_image_in(tex.wool.px, WOOL_W, WOOL_H, 3, TEX_W, TEX_H, WOOL_X, WOOL_Y,', 'cd_encode_image_in(tex.wool.px, WOOL_W, WOOL_H, 3, WOOL_W, WOOL_H, 0, 0,', 'tools/test.mjs'),
  ('fern samples', 'src/textures.c', '{{0.375, 0.875}, {0.875, 0.625}, {0.125, 0.375}, {0.625, 0.125}}', '{{0.375, 0.125}, {0.875, 0.375}, {0.125, 0.625}, {0.625, 0.875}}', 'tools/texparity.mjs'),
  ('cos not rounded', 'src/crtrig.c', '  return fast_two_sum(p.hi, p.lo + (a.hi * b.lo + a.lo * b.hi));', '  return fast_two_sum(p.hi, p.lo + a.hi * b.lo);', 'tools/texparity.mjs'),
  ('ws message bytes shifted', 'vendor/cdom.js', 'bytes(ptr, incoming.length).set(incoming);', 'bytes(ptr, incoming.length).set(incoming.map((v) => v ^ 1));', 'tests/net_test.mjs'),
  ('not canonical between operations', 'src/game_keep.c', 'void game_keep_canonical(Game *g) { state_canonical(g); }', 'void game_keep_canonical(Game *g) { (void)g; }', 'out/blob_test'),
  ('a drop that is no move spends the selection', 'src/table_app.c', '  if (!nsel || ask_play(target, &p) != CLIENT_OK || p.move_type < 0) return;\n  nsel = 0;', '  if (!nsel || ask_play(target, &p) != CLIENT_OK) return;\n  nsel = 0;\n  if (p.move_type < 0) return;', 'tools/test.mjs', 'React'),
  ('the bot not scheduled at the start', 'src/table_app.c', '  mark_shown();\n  bot_check();\n  render();\n}', '  mark_shown();\n  render();\n}', 'tools/test.mjs', 'golden|kernel says'),
  ('the bot at the port\'s old 650 ms', 'src/table_app.c', '  last_wait = bot_wait_ms(&G, game_human_mask(&G), (int64_t)shown_ms, (int64_t)settles_ms(), now);', '  last_wait = (int64_t)settles_ms() + 650 > now ? (int)((int64_t)settles_ms() + 650 - now) : 0;', 'tools/test.mjs', 'kernel says'),
  ('the bot not asked again when its timer fires', 'src/table_app.c', '  if (w > 0) { cd_timer_start(w, TIMER_BOT_BASE + bot_gen); return; }', '  (void)w;', 'tools/test.mjs', 'kernel says'),
  ('the end rows not the kernel\'s finish order', 'src/table_app.c', '    for (int i = 0; i < n; i++) if (rows[i].place == place) result_row(place, n, rows[i].seat);', '    for (int i = 0; i < n; i++) if (rows[i].place == place) result_row(place, n, rows[n - 1 - i].seat);', 'tools/test.mjs', 'React'),
  ('no (You) on my row', 'src/table_app.c', '  if (seat == ME) { OPEN("span"); CLASS("result-card__you");', '  if (0) { OPEN("span"); CLASS("result-card__you");', 'tools/test.mjs', 'React'),
  ('the game not kept after a bot step', 'src/table_app.c', '  bot_drive(&G, game_human_mask(&G), BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);\n  committed();', '  bot_drive(&G, game_human_mask(&G), BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);\n  game_keep_canonical(&G);', 'tools/test.mjs', 'reload'),
  ('the stored game ignored', 'src/table_app.c', '  game_adopted = len > 0 && game_keep_load(&G, p, len);', '  game_adopted = 0;', 'tools/test.mjs', 'reload'),
  ('a failed save ignored', 'src/table_app.c', '  save_failed = !ok;\n', '  save_failed = 0;\n', 'tools/test.mjs', 'not silent'),
  ('a failed save not tried again', 'src/table_app.c', '  if (token == TIMER_SAVE_RETRY) { save_retry_pending = 0; game_save(); }', '', 'tools/test.mjs', 'not silent'),
  # the animation pass: the capture (tests/run_test.c, native)
  ('the hook not installed', 'src/anim_run.c', '  engine_snap_hook = snap;', '  (void)snap;', 'out/run_test'),
  ('the stream written for nobody', 'src/anim_run.c', 'g, viewer, actor, ended,', 'g, -1, actor, ended,', 'out/run_test'),
  ('the final transition dropped', 'src/anim_run.c', 'g, viewer, actor, ended,', 'g, viewer, actor, 0,', 'out/run_test'),
  # the animation pass: the table (tools/test.mjs, the check named by the regex)
  ('the deal flies at 500 ms, not the kernel\'s', 'src/table_app.c', '  flight_ms = ps->duration_ms;\n  cd_viewport_rec vp;', '  flight_ms = 500;\n  cd_viewport_rec vp;', 'tools/test.mjs', 'deal is'),
  ('a landed step not committed', 'src/table_app.c', 'static const TableView *run_board(void) { return run_landed ? &run[run_landed - 1].board : &run_pre; }', 'static const TableView *run_board(void) { return &run_pre; }', 'tools/test.mjs', 'sweep'),
  ('the bot paced from the click', 'src/table_app.c', 'static double settles_ms(void) { return run_settles > shown_ms ? run_settles : shown_ms; }', 'static double settles_ms(void) { (void)run_settles; return shown_ms; }', 'tools/test.mjs', 'waits as the kernel'),
  ('no veil', 'src/table_app.c', 'static void veil_set(cd_id id, int on) { cd_set_style(id, ZS("visibility"), on ? ZS("hidden") : ZS("")); }', 'static void veil_set(cd_id id, int on) { (void)id; (void)on; }', 'tools/test.mjs', 'played card|kernel.s game'),
  ('flights never armed', 'src/table_app.c', '  if (gen != (flight_gen & 0x7fffffffu)) return;', '  if (gen || 1) return;', 'tools/test.mjs', 'played card'),
  ('the row does not glide', 'src/table_app.c', '    cd_set_style(battle_cell[moved[k]], ZS("transition"), sb_tmp(&t));', '    (void)sb_tmp(&t);', 'tools/test.mjs', 'played card'),
  ('the attack does not turn with its cover', 'src/table_app.c', '    if (e->type == EVW_T_COVER && e->has_target) SHOW.covering |= 1ull << card_to_id(e->target);', '', 'tools/test.mjs', 'cover flies'),
  ('my hand makes no room', 'src/table_app.c', '  hand_layout(&WEB_HAND, V->my_hand_count + SHOW.arriving, width, &HL);', '  hand_layout(&WEB_HAND, V->my_hand_count, width, &HL);', 'tools/test.mjs', 'cover flies'),
  ('a move during a run starts another', 'src/table_app.c', '  int was = run_on(), from = run_n;', '  int was = 0, from = run_n;', 'tools/test.mjs', 'joins it'),
  ('no coin', 'src/table_app.c', '    else if (mark != u->target) coin_start(u, mark);', '    else if (mark != u->target) { u->target = mark; face_draw(u, mark); (void)coin_start; }', 'tools/test.mjs', 'sweep'),
  ('the marks not frozen in a run', 'src/table_app.c', '  if (anim_shown_ledger_allows(ANIM_CLAIM_BYSTANDER, run_on())) roles_set(roles_of(V), V->num_battles > 0);', '  roles_set(roles_of(V), V->num_battles > 0);', 'tools/test.mjs', 'transfer'),
  ('the good does not lead the stream', 'src/table_app.c', '    roles_set(opening, SHOWN_ROLES.open);', '    (void)opening;', 'tools/test.mjs', 'sweep'),
  ('the shield waits for the landing', 'src/table_app.c', '      && anim_pass_hand_off(SHOWN_ROLES.roles, 1u << e->seat, run_final.defender, &out)) roles_set(out, 1);', '      && anim_pass_hand_off(SHOWN_ROLES.roles, 1u << e->seat, run_final.defender, &out)) (void)out;', 'tools/test.mjs', 'transfer'),
  ('the trump drawn out of the stock', 'src/table_app.c', '    int trump = !card_is_none(st->trump_out) && st->trump_from == ANIM_LOC_FLIPPED;', '    int trump = 0;', 'tools/test.mjs', 'played out by clicks'),
  ('http body not sent', 'vendor/cdom.js', 'body: bodyLen ? copyBytes(bodyPtr, bodyLen) : undefined,', 'body: undefined,', 'tests/net_test.mjs'),
  # the network pass: each import the live contract test exists for, broken (tests/live_test.mjs, a local c_base)
  ('live: request headers not sent', 'vendor/cdom.js', 'headers: headerList(headersPtr, headersLen),', 'headers: [],', 'tests/live_test.mjs'),
  ('live: the status not passed on', 'vendor/cdom.js', 'status = response.status;', 'status = 200;', 'tests/live_test.mjs'),
  ('live: no deadline', 'vendor/cdom.js', 'const timer = deadlineMs ? setTimeout(() => controller.abort(), deadlineMs) : 0;', 'const timer = 0;', 'tests/live_test.mjs'),
  ('live: the deadline dropped in C', 'vendor/cdom_net.c', '  j_http(token, method, mlen, url, ulen, headers, hlen, body, blen, deadline_ms);', '  j_http(token, method, mlen, url, ulen, headers, hlen, body, blen, 0 * deadline_ms);', 'tests/live_test.mjs'),
  ('live: an abort reported as a failure', 'vendor/cdom.js', 'controller.signal.aborted ? 1 : 0', '0', 'tests/live_test.mjs'),
  ('live: socket ids never reused', 'vendor/cdom.js', 'const id = freeSockets.length ? freeSockets.pop() : sockets.length;', 'const id = sockets.length;', 'tests/live_test.mjs'),
  ('live: bufferedAmount always 0', 'vendor/cdom.js', '    return sockets[id].bufferedAmount;', '    return 0;', 'tests/live_test.mjs'),
]
only = sys.argv[1:]
# the source's copy while a mutant is in it: outside the checkout, so no commit can take it
KEEP = tempfile.mkdtemp(prefix='cdom-poc-mutate-')
for name, f, old, new, test, *check in MUTANTS:
  if only and name not in only: continue
  path = os.path.join(POC, f); keep = os.path.join(KEEP, os.path.basename(f) + '.keep')
  src = open(path).read()
  assert src.count(old) == 1, f'{name}: anchor not found once'
  shutil.copy(path, keep)
  try:
    open(path, 'w').write(src.replace(old, new))
    b = subprocess.run(['./build.sh'], cwd=POC, capture_output=True, text=True)
    if b.returncode: print(f'{name}: BUILD FAILED\n{b.stderr[-800:]}'); continue
    env = dict(os.environ, ONLY=check[0]) if check else os.environ
    r = subprocess.run(['node', test] if test.endswith('.mjs') else [os.path.join(POC, test)], cwd=POC, capture_output=True, text=True, env=env)
    lines = [l for l in r.stdout.splitlines() if l.startswith('FAIL') or 'ok' in l or 'failed' in l][:12]
    print(f'== {name}: exit {r.returncode}')
    for l in r.stdout.splitlines():
      if l.startswith('FAIL') or l.startswith('     ') or l.startswith('ws') or l.startswith('http') or l.startswith('game 3') or l.startswith('all at') or l.startswith('done'): print('   ', l[:400])
  finally:
    shutil.move(keep, path)
b = subprocess.run(['./build.sh'], cwd=POC, capture_output=True, text=True)
print('restored and rebuilt:', b.returncode == 0)
