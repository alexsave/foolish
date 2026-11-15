#!/bin/bash
# make_trace_engine.sh DEST : copies the engine headers to DEST/a/b/soundtrack/engine (so ../../../../shared resolves through
# DEST/shared) and patches the COPY of sampler.h with trace lines (note start, loop wraps). The real engine is not touched.
set -e
D="$1"; E=/Users/alex/Dev/foolish-wt-city-songs-v5/foolish/tools/soundtrack/engine
rm -rf "$D"; mkdir -p "$D/a/b/soundtrack/engine"; ln -s /Users/alex/Dev/foolish-wt-city-songs-v5/shared "$D/shared"
cp "$E"/*.h "$D/a/b/soundtrack/engine/"
python3 -I - "$D/a/b/soundtrack/engine/sampler.h" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
a='    double rel = p[0] > 0 ? p[0] : 0.25, start'
assert a in s
s=s.replace(a,'''    fprintf(stderr, "TRACE note t=%.4f dur=%.4f layer=%d file=%s n=%ld sr=%.0f root=%d ratio=%.6f att=%.3f\\n", n->t, n->dur, layer, x->path, x->n, x->sr, x->root, ratio, x->att);
'''+a,1)
b='if (over >= xf) { pos = la + (over - xf); over = -1; }'
assert b in s
s=s.replace(b,'if (over >= xf) { pos = la + (over - xf); over = -1; fprintf(stderr, "TRACE wrap t=%.4f la=%ld lb=%ld xf=%ld\\n", n->t + i / (double)SR, la, lb, xf); }',1)
open(p,'w').write(s)
PY
