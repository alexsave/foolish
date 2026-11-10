#!/usr/bin/env python3
# mutate.py: the memory series' mutation checks (foolish #480, #481, #483) on a clone at merged main.
# Each mutation breaks one spot, builds the test binary that covers it, runs it, and checks it FAILS on
# the expected assertion (a FAIL line containing `expect`). The file is restored from a copy afterwards
# (never git checkout) and touched, so make rebuilds it (a restored copy keeps an mtime older than the mutant's object:
# run 1 of this script missed that and left mutants in later builds). The script starts and ends with clean builds that
# must pass, and ends with `git status --short` and `git diff --stat` of the clone.
import subprocess, shutil, sys, os
R = '/Users/alex/Dev/cdom-memtest/final/mut'
K = ('foolish/c', 'make -s -j8 build/cnitro_tests', './build/cnitro_tests')
J = ('c_base', 'make -s -j8 build/test/unit_journal', './build/test/unit_journal')
A = ('c_base', 'make -s -j8 build/test/unit_app', './build/test/unit_app')
RO = 'foolish/c/src/roster.c'
JN = 'foolish/server/src/store/journal.c'
TB = 'foolish/c/src/table.c'
BM = 'foolish/c/src/bot_memory.c'
TO = 'foolish/server/src/app/table_ops.c'
M = [
 # #480 roster format 2 (kernel: test_roster_*; journal: seat refs; app: the row's roster)
 ('480-K1', RO, 'is_packed[s] = (uint8_t)roster_id_pack(x->id, x->id_len, packed[s]);', 'is_packed[s] = 0;', K, 'a UUID id costs 17 bytes'),
 ('480-K2', RO, 'if (canon && roster_id_pack(', 'if (0 && canon && roster_id_pack(', K, 'a canonical UUID written as text is refused'),
 ('480-K3', RO, 'return at == len ? ROSTER_OK : ROSTER_E_LENGTH;   // nothing after the last seat', 'return ROSTER_OK;', K, 'a byte after the last seat is refused'),
 ('480-K4', RO, 'if (!mask[i] && p[i]) return ROSTER_E_PADDING;', '(void)mask;', K, 'format 1: a byte past the title is refused'),
 ('480-K5', RO, 'q[SEAT_ID_AT], 0, q[SEAT_BRAIN_AT] > 0 };', 'q[SEAT_ID_AT], 0, 0 };', K, "each seat's kind is the roster's"),
 ('480-K6', RO, 'if (s->packed) return packed && bytes_eq((const char *)s->id, (const char *)packed, ROSTER_UUID_BYTES);', 'if (s->packed) return packed != 0;', K, 'a packed seat is not another UUID'),
 ('480-K7', RO, 'const int v = unpack_v2(r, p, len, 0);', 'const int v = unpack_v2(r, p, len, 0) ? ROSTER_E_LENGTH : roster_validate(r);', K, 'unpack leaves the rules to the load that made the resident'),
 ('480-K8', RO, '24, 26, 28, 30, 32, 34 };', '26, 24, 28, 30, 32, 34 };', K, 'a packed id decodes as its text'),
 ('480-S1', JN, 'if (!refs[i].bot && roster_ref_is(', 'if (roster_ref_is(', J, 'FAIL'),
 ('480-S2', JN, '    if (src->roster_len) {\n        uint8_t *roster = g->row.roster;', '    if (0) {\n        uint8_t *roster = g->row.roster;', J, 'FAIL'),
 ('480-S3', TO, 'np->roster = c->roster.len ? (uint8_t *)A + c->roster.off : row->roster;\n    np->roster_len = c->roster.len ? (size_t)c->roster.len : row->roster_len;', 'np->roster = (uint8_t *)A + c->roster.off;\n    np->roster_len = (size_t)c->roster.len;', A, 'FAIL'),
 # #481 state buffer (journal: a_resident_keeps_state_bytes_of_its_size)
 ('481-J1', JN, 'rc = grow_copy(&state, &g->state_cap, buf, n);', 'rc = grow_copy(&state, &g->state_cap, buf, TABLE_STATE_MAX);', J, '[a_resident_keeps_state_bytes_of_its_size] f.state_bytes >= held.state_len && f.state_bytes < TABLE_STATE_MAX / 4'),
 ('481-J2', JN, '    if (g->row_fresh || !g->res_valid) return row_dup(dst, &g->row);', '    if (1) return row_dup(dst, &g->row);', J, '[a_resident_keeps_state_bytes_of_its_size] tables.state_len == held.state_len'),
 # #483 bot memory (kernel: test_table_resident_holds_memory_only_where_read; journal: a_resident_is_held_at_its_size)
 ('483-K1', TB, 'const bool full = valid && !m->counts_only;', 'const bool full = valid;', K, 'every resident is held at the size its seats call for'),
 ('483-K2', TB, 'const bool full = valid && !m->counts_only;', 'const bool full = false;', K, 'its resident holds the memory'),
 ('483-K3', TB, 'out->mem_records = full ? 0 : m->n_records;', 'out->mem_records = 0;', K, 'every held resident encodes to its row\'s bytes'),
 ('483-K4', TB, '    m->counts_only = 1;\n}', '    m->counts_only = 0;\n}', K, 'holds the memory one adopted from the whole struct does'),
 ('483-K5', BM, 'out[at++] = 0xff;   // cur_def -1', 'out[at++] = 0;   // cur_def -1', K, 'every held resident encodes to its row\'s bytes'),
 ('483-K6', TB, '    m->num_players = res->mem_players;\n', '', K, 'holds the memory one adopted from the whole struct does'),
 ('483-J1', JN, '    const size_t cap = table_resident_bytes(*res);', '    const size_t cap = sizeof(TableResident);', J, '[a_resident_is_held_at_its_size] facts(s, gid, &rc).resident_bytes'),
 ('483-J2', JN, '    const size_t need = table_resident_bytes(src);', '    const size_t need = sizeof(TableResident);', J, '[a_resident_is_held_at_its_size] facts(s, gid, &rc).resident_bytes'),
 ('483-J3', TO, 'gc.resident = table_resident_into(t, &c->clock, s->res, s->res_small) == s->res ? &s->res : &s->res_small;', 'gc.resident = table_resident_into(t, &c->clock, s->res, s->res) == s->res ? &s->res : &s->res_small;', J, '[a_resident_is_held_at_its_size]'),
]
only = sys.argv[1:]
def clean_pass(when):
    for f in {m[1] for m in M}: os.utime(f'{R}/{f}')
    for d, b, run in (K, J, A):
        assert subprocess.run(b, shell=True, cwd=f'{R}/{d}', capture_output=True).returncode == 0, (when, b)
        o = subprocess.run(run, shell=True, cwd=f'{R}/{d}', capture_output=True, text=True)
        print(when, run, 'rc', o.returncode, [l for l in (o.stdout + o.stderr).split('\n') if 'failed' in l][-1:], flush=True)
clean_pass('baseline')
for tag, f, old, new, (d, b, run), expect in M:
    if only and tag not in only: continue
    p = f'{R}/{f}'; bak = p + '.mutbak'; shutil.copy(p, bak)
    try:
        s = open(p).read(); assert s.count(old) == 1, (tag, 'not unique', s.count(old)); open(p, 'w').write(s.replace(old, new))
        bo = subprocess.run(b, shell=True, cwd=f'{R}/{d}', capture_output=True, text=True)
        if bo.returncode: print(tag, 'BUILD FAILED', bo.stderr[-300:], flush=True); continue
        o = subprocess.run(run, shell=True, cwd=f'{R}/{d}', capture_output=True, text=True)
        out = o.stdout + o.stderr
        fails = [l for l in out.split('\n') if l.startswith('FAIL')]
        hit = any(expect in l for l in fails) if expect != 'FAIL' else bool(fails)
        crash = o.returncode < 0
        fails = [l for l in fails if expect in l] + [l for l in fails if expect not in l]
        print(tag, 'RED' if hit else ('CRASH rc=%d' % o.returncode if crash else 'GREEN (mutation survived)'), '|', ' || '.join(fails[:3])[:500], flush=True)
        if crash: print('   last test lines before the crash:', ' || '.join([l for l in out.split('\n') if l.strip()][-4:])[:700], flush=True)
    finally:
        shutil.move(bak, p); os.utime(p)
clean_pass('after')
print(subprocess.run('git status --short; git diff --stat', shell=True, cwd=R, capture_output=True, text=True).stdout or 'clone clean: no diff')
