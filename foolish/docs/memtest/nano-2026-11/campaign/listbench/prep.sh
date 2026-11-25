#!/bin/bash
# Makes the scratch journal copies the bench includes (never the worktrees' own files).
set -euo pipefail
D=/Users/alex/Dev/foolish-bench-results/campaign/listbench
cd "$D"
fix_inc() { sed -e 's|"\.\./\.\./\.\./\.\./shared/c/|"|g' "$1"; }
# A: 59c115054 as it is.
fix_inc journal_A_orig.c > journal_A.c
# Ac: A with counters (walk-back steps, touches, in-place touches, wait list begins/ends).
fix_inc journal_A_orig.c | perl -0pe '
  s/(static void lru_in\(cb_journal \*j, jgame \*g\) \{\n    jgame \*a = j->lru_tail;\n    while \(a && a->touched_ms > g->touched_ms\)) a = a->lru_prev;/$1 { a = a->lru_prev; lb_walk++; }/ or die "lru_in";
  s/(static void ov_touch\(cb_journal \*j, jgame \*g, int64_t now\) \{\n    g->touched_ms = now;\n)/$1    lb_touch++;\n/ or die "ov_touch";
  s/(\(!g->lru_prev \|\| g->lru_prev->touched_ms <= now\)\)) return;/$1 { lb_inplace++; return; }/ or die "inplace";
  s/(static void wait_begin\(cb_journal \*j, jgame \*g\) \{\n)/$1    lb_wbegin++;\n/ or die "wb";
  s/(static void wait_end\(cb_journal \*j, jgame \*g\) \{\n)/$1    lb_wend++;\n/ or die "we";
  s/(#include <unistd.h>\n)/$1long lb_walk, lb_touch, lb_inplace, lb_wbegin, lb_wend;\n/ or die "inc";
' > journal_Ac.c
# A0: A with the residency relink of ov_touch taken out (the stamp kept): the residency upkeep alone.
fix_inc journal_A_orig.c | perl -0pe '
  s/(\(!g->lru_prev \|\| g->lru_prev->touched_ms <= now\)\)) return;   \/\/ in place already\n    lru_out\(j, g\);\n    lru_in\(j, g\);/$1 return;/ or die "a0";
' > journal_A0.c
# B: 9c5057b64 with the bench's clock for every stamp, and no flush thread (A's two test opts, backported).
fix_inc journal_B_orig.c | perl -0pe '
  s/cb_mono_ms\(\)/lb_clock()/g;
  s/(#include <unistd.h>\n)/$1int64_t lb_clock(void);\n/ or die "inc";
  s/if \(rc == CB_OK && pthread_create\(&j->thr, NULL, flusher, j\) != 0\) \{/if (rc == CB_OK && 0 \&\& pthread_create(\&j->thr, NULL, flusher, j) != 0) {/ or die "thr";
  s/    j->thr_started = true;\n/    j->thr_started = false;\n    (void)flusher;\n/ or die "started";
' > journal_B.c
grep -c lb_ journal_Ac.c journal_B.c
diff journal_A.c journal_A0.c | head -20 || true
