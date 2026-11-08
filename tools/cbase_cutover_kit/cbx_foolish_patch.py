#!/usr/bin/env python3
# cbx_foolish_patch.py <foolish clone>: what foolish's own files change at the cutover, beyond the
# files that move: the console pages it declares (its shapes in its own admin_json.c), the
# overview's counter by its generic name. Idempotent.
import re, sys
C = sys.argv[1]
def ed(p, pairs):
    s = open(C + '/' + p).read()
    for a, b in pairs:
        if b in s and a not in s:
            continue
        assert s.count(a) >= 1, (p, a[:70])
        s = s.replace(a, b)
    open(C + '/' + p, 'w').write(s)

p = 'foolish/server/src/app/admin_panels.c'
s = open(C + '/' + p).read()
s = s.replace('#include "core_json.h"\n', '#include "admin_json.h"\n#include "core_json.h"\n', 1) if 'admin_json.h' not in s else s
for t in ['cb_js_admin_seat', 'cb_js_admin_games_begin', 'cb_js_admin_games_add', 'cb_js_admin_games_end', 'cb_js_admin_game_write',
          'cb_js_admin_game', 'cb_js_admin_envelope', 'cb_js_admin_bots_begin', 'cb_js_admin_bots_add', 'cb_js_admin_bots_end',
          'cb_js_admin_bot', 'cb_js_stream']:
    s = re.sub(r'\b' + t + r'\b', t.replace('cb_js_', 'fl_js_'), s)
if 'cb_foolish_admin_panels' not in s:
    s = s.replace('const cb_admin_call cb_foolish_admin_calls[3] = {', '''// The console's pages of them (cbase/app.h cb_admin_panel): the games, paged and filtered by id
// prefix, each opened by id; the bots in ladder order.
const cb_admin_panel cb_foolish_admin_panels[2] = {
    { "games", "Games", "Each row is the stored game. Open one for its byte sizes and the spectator envelope, the bytes spectator_views answers.",
      "games", "prefix", "next", "game" },
    { "bots", "Bots", "The bot ladder with ratings, in ladder order.", "bots", NULL, NULL, NULL },
};

const cb_admin_call cb_foolish_admin_calls[3] = {''', 1)
open(C + '/' + p, 'w').write(s)

p = 'foolish/server/src/app/admin_panels.h'
s = open(C + '/' + p).read()
if 'cb_foolish_admin_panels' not in s:
    s = re.sub(r'(extern const cb_admin_call cb_foolish_admin_calls\[3\];\n)', r'\1extern const cb_admin_panel cb_foolish_admin_panels[2];\n', s, 1)
    assert 'cb_foolish_admin_panels' in s, 'admin_panels.h: no cb_foolish_admin_calls declaration'
open(C + '/' + p, 'w').write(s)

ed('foolish/server/src/app/foolish_app.c', [
    ('    .admin_count = cb_admin_games_needing_bots,\n',
     '    .admin_panels = cb_foolish_admin_panels,\n    .n_admin_panels = 2,\n    .admin_count = cb_admin_games_needing_bots,\n    .admin_count_label = "Games needing bots",\n'),
])
ed('foolish/server/test/unit_admin.c', [('"games_needing_bots", &n)', '"app_count", &n)')])

# ---- the product's knobs, read by the product under the names its deployments use -------------
import os, glob
for root in ['foolish/server/src', 'foolish/server/test', 'foolish/server/bench', 'foolish/server/fuzz', 'foolish/server/migrate', 'foolish/server/tools']:
    for f in glob.glob(C + '/' + root + '/**/*.[ch]', recursive=True):
        s = open(f).read()
        t = re.sub(r'\bbot_speedup\b', 'clock_speedup', s)
        t = re.sub(r'\bbot_workers\b', 'job_workers', t)
        t = t.replace('(ctx->cfg ? ctx->cfg->rated_min_age_s : 72 * 3600)', 'cb_foolish_rated_min_age_s()')
        t = t.replace('g_cfg.rated_min_age_s = 1 << 30;', 'cb_foolish_rated_min_age_set(1 << 30);')
        if t != s: open(f, 'w').write(t)

p = 'foolish/server/src/app/foolish_app.h'
s = open(C + '/' + p).read()
if 'cb_foolish_config' not in s:
    s = s.replace('int cb_kernel_selftest(char *err, size_t cap);', '''int cb_kernel_selftest(char *err, size_t cap);
// foolish's own settings (cbase/app.h config), under the names its deployments use:
// CBASE_BOT_WORKERS (1..64) the job pool cbase serve runs the bots on, CBASE_BOT_SPEEDUP (1..100)
// every timer of the bot loop and the platform's (cbase's clock speedup), and CBASE_RATED_MIN_AGE
// (seconds, 0..30 days; 72 h) the age under which an account's game ends unrated (M6).
int cb_foolish_config(struct cb_config *cfg, char *err, size_t cap);
int cb_foolish_rated_min_age_s(void);
void cb_foolish_rated_min_age_set(int s);   // a test's''', 1)
    open(C + '/' + p, 'w').write(s)

p = 'foolish/server/src/app/foolish_app.c'
s = open(C + '/' + p).read()
if 'cb_foolish_config(' not in s:
    s = s.replace('int cb_kernel_selftest(char *err, size_t cap) {', '''// ---- foolish's own settings (foolish_app.h cb_foolish_config) ---------------------------------

static int g_rated_min_age_s = 72 * 3600;
int cb_foolish_rated_min_age_s(void) { return g_rated_min_age_s; }
void cb_foolish_rated_min_age_set(int s) { g_rated_min_age_s = s; }

// An integer setting in [lo, hi] when it is set: true and *out, or false with the sentence in err.
static bool knob(const char *name, int64_t lo, int64_t hi, bool *set, int64_t *out, char *err, size_t cap) {
    const char *v = getenv(name);
    *set = v && *v;
    if (!*set) return true;
    if (num_parse_i64(v, strlen(v), lo, hi, out)) return true;
    snprintf(err, cap, "%s=%s is not a number in %lld..%lld", name, v, (long long)lo, (long long)hi);
    return false;
}

int cb_foolish_config(cb_config *cfg, char *err, size_t cap) {
    bool set;
    int64_t v;
    if (!knob("CBASE_BOT_WORKERS", 1, 64, &set, &v, err, cap)) return CB_E_INVALID;
    if (set) cfg->job_workers = (int)v;
    if (!knob("CBASE_BOT_SPEEDUP", 1, 100, &set, &v, err, cap)) return CB_E_INVALID;
    if (set) cfg->clock_speedup = (int)v;
    if (!knob("CBASE_RATED_MIN_AGE", 0, 30 * 86400, &set, &v, err, cap)) return CB_E_INVALID;
    g_rated_min_age_s = set ? (int)v : 72 * 3600;
    return CB_OK;
}

int cb_kernel_selftest(char *err, size_t cap) {''', 1)
    s = s.replace('    .selftest = cb_kernel_selftest,\n', '    .selftest = cb_kernel_selftest,\n    .config = cb_foolish_config,\n', 1)
    for inc in ['#include <stdlib.h>\n', '#include <string.h>\n', '#include <stdio.h>\n']:
        if inc not in s: s = s.replace('#include "foolish_app.h"\n', '#include "foolish_app.h"\n' + inc, 1)
    if '"num_parse.h"' not in s:
        s = s.replace('#include "foolish_app.h"\n', '#include "foolish_app.h"\n#include "num_parse.h"\n', 1)
    open(C + '/' + p, 'w').write(s)

# ---- its key families on the item layout, and the console's name for its games ------------------
p = 'foolish/server/src/store/rows_foolish.h'
s = open(C + '/' + p).read()
if 'CB_ADMIN_GAMES' not in s:
    s = s.replace('extern const cb_store_app cb_foolish_store;', '''// The console's name for the games, the same on both backends (cb_store_app family_aliases): the
// games table on the SQL store, the GAME family on the item layout.
#define CB_ADMIN_GAMES "games"
extern const cb_store_app cb_foolish_store;''', 1)
    open(C + '/' + p, 'w').write(s)
p = 'foolish/server/src/store/rows_sql.c'
s = open(C + '/' + p).read()
if 'FOOLISH_FAMILIES' not in s:
    s = s.replace('const cb_store_app cb_foolish_store = {', '''// foolish's key families on the item layout (rows_foolish.c), after the platform's; SCAN is one
// partition (the heartbeat's).
static const cb_store_family FOOLISH_FAMILIES[] = {
    { "GAME", false }, { "BOT", false }, { "SNAP", false }, { "CHAT", false }, { "SCAN", true },
};
static const cb_store_alias FOOLISH_ALIASES[] = { { CB_ADMIN_GAMES, "GAME" } };

const cb_store_app cb_foolish_store = {
    .families = FOOLISH_FAMILIES,
    .n_families = 5,
    .family_aliases = FOOLISH_ALIASES,
    .n_family_aliases = 1,''', 1)
    open(C + '/' + p, 'w').write(s)

# ---- its legacy tables, dropped by itself before the schemas (cb_store_app sql_before) ----------
p = 'foolish/server/src/store/rows_sql.c'
s = open(C + '/' + p).read()
if 'sql_before_legacy' not in s:
    s = s.replace('// foolish\'s key families on the item layout', '''// Its tables of schemas 1 to 5, children first, dropped before any schema is applied (cb_store_app
// sql_before); the platform drops its own after them.
static int sql_before_legacy(cb_sql *x, int64_t from) {
    if (from >= 6) return CB_OK;
    return cb_sql_exec(x, "DROP TABLE IF EXISTS chat_messages; DROP TABLE IF EXISTS game_snapshots; "
                          "DROP TABLE IF EXISTS game_members; DROP TABLE IF EXISTS game_bots; DROP TABLE IF EXISTS ratings; "
                          "DROP TABLE IF EXISTS games; DROP TABLE IF EXISTS bots; DROP TABLE IF EXISTS bot_seed;");
}

// foolish's key families on the item layout''', 1)
    s = s.replace('    .sql_upgrade = sql_upgrade,\n', '    .sql_upgrade = sql_upgrade,\n    .sql_before = sql_before_legacy,\n', 1)
    open(C + '/' + p, 'w').write(s)

# ---- its keys' shapes for the DynamoDB key capture (cb_store_app key_templates) -----------------
p = 'foolish/server/src/store/rows_sql.c'
s = open(C + '/' + p).read()
if 'FOOLISH_KEYS_PK' not in s:
    s = s.replace('// foolish\'s key families on the item layout', '''// The shapes of its keys (rows_foolish.c's item layout), beside the platform's, for the key
// capture (its golden file).
static const char *const FOOLISH_KEYS_PK[] = { "GAME#{}", "BOT#{}", "SNAP#{}", "CHAT#{}", "SCAN", NULL };
static const char *const FOOLISH_KEYS_SK[] = { "RATING", "ROW", "BOT", "SNAP", "MEMBER#{}", "BOT#{}", "PLAYER#{}", "GAME#{}", "{}#{}", NULL };
static const char *const FOOLISH_KEYS_GSI1PK[] = { "USERGAMES#{}", "USERCHAT#{}", "GAMESNAPS#{}", NULL };
static const char *const FOOLISH_KEYS_GSI2PK[] = { "RATINGS", "BOTS", "PLAYER#{}", NULL };
static const char *const FOOLISH_KEYS_GSI2SK[] = { "E#{}#{}", NULL };

// foolish's key families on the item layout''', 1)
    s = s.replace('    .n_family_aliases = 1,', '''    .n_family_aliases = 1,
    .key_templates = { FOOLISH_KEYS_PK, FOOLISH_KEYS_SK, FOOLISH_KEYS_GSI1PK, NULL, FOOLISH_KEYS_GSI2PK, FOOLISH_KEYS_GSI2SK },''', 1)
    open(C + '/' + p, 'w').write(s)
