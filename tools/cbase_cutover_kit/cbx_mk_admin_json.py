#!/usr/bin/env python3
import os
# Writes foolish/server/src/app/admin_json.{h,c} into the overlay: the console shapes of foolish's
# games and bots that left cbase's core_json, under foolish's own prefix (fl_js_*), from the cbase
# commit before the purge (git show), so the bytes they write are the same.
import re, subprocess, sys
S = os.path.dirname(os.path.abspath(__file__))   # the kit folder
O = S + '/foolish_overlay/foolish/server/src/app'
def show(path):
    return subprocess.check_output(['git', '-C', '/Users/alex/Dev/cbase', 'show', 'HEAD:' + path]).decode()
h = show('core/src/core_json.h')
c = show('core/src/core_json.c')
decl = h[h.index('// One game as the console shows it'):h.index('int           cb_js_admin_bots_end(cb_js_stream *s);\n') + len('int           cb_js_admin_bots_end(cb_js_stream *s);\n')]
impl = c[c.index("// One game's scalar columns and its seats, into an open object."):c.index('cb_js_stream *cb_js_admin_logs_begin(')]
def ren(s):
    s = s.replace('cb_js_stream', 'fl_js_stream').replace('cb_js_admin_', 'fl_js_admin_')
    return s
decl = ren(decl).replace('(a product\'s admin call fills it: foolish\n// reads', '(admin_panels.c fills it: it\n// reads')
impl = ren(impl)
open(O + '/admin_json.h', 'w').write('''// admin_json.h - foolish's one JSON file's shapes (admin_json.c): the console pages of its games
// and bots (admin_panels.c), which cbase's console draws from the panels the app declares
// (cbase/app.h cb_admin_panel).
#ifndef FL_ADMIN_JSON_H
#define FL_ADMIN_JSON_H

#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A document being written: begun, added to, ended (which frees it), as cbase's core_json streams.
typedef struct fl_js_stream fl_js_stream;

''' + decl + '''
#endif
''')
open(O + '/admin_json.c', 'w').write('''// admin_json.c - the JSON of foolish's console pages (admin_json.h): its one JSON file, which the
// JSON gate allows (APP_JSON_FILES), on cbase's writer (util/json.h).
#include "admin_json.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/json.h"

#include <stdlib.h>

struct fl_js_stream {
    cb_jw w;
};

static fl_js_stream *adm_open(cb_buf *out) {
    fl_js_stream *s = malloc(sizeof *s);
    if (!s) return NULL;
    cb_jw_init(&s->w, out);
    cb_jw_obj(&s->w);
    return s;
}

static int adm_close(fl_js_stream *s) {
    if (!s) return CB_E_NOMEM;
    cb_jw_end(&s->w);
    bool ok = cb_jw_ok(&s->w);
    free(s);
    return ok ? CB_OK : CB_E_NOMEM;
}

static void adm_kiso(cb_jw *w, const char *k, int64_t ms) {
    char t[40];
    if (ms <= 0 || cb_iso8601(ms, t, sizeof t) < 0) cb_jw_knull(w, k);
    else cb_jw_kstr(w, k, t);
}

static int adm_done(cb_jw *w) { return cb_jw_ok(w) ? CB_OK : CB_E_NOMEM; }

''' + impl)
print('wrote', O + '/admin_json.{h,c}')
