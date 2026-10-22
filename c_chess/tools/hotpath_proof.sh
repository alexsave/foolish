#!/bin/bash
# hotpath_proof.sh - the static proof of what a c_base change does to foolish's code: two copies of
# c_base's source tree, one with the changed files as BASE (default origin/main) has them, compiled
# with c_base's own compile command (foolish's flags), every function's disassembly compared, and
# the functions that differ named with their diffs. A new file has no old side and is listed as
# added. Run from c_chess/; prints the verdict and writes the diffs under build/hotpath/.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(cd .. && pwd)
OUT=$PWD/build/hotpath
rm -rf "$OUT"; mkdir -p "$OUT"
BASE=${1:-origin/main}

# c_base's exact compile command for a core object (flags, includes, defines), minus the in/out files
cd "$ROOT/c_base"
CMD=$(make -n build/obj/cbase/core/src/net/server.o 2>/dev/null | grep -E 'server\.c' | head -1 | sed -E 's/ -MD.*//')
[ -n "$CMD" ] || { echo "cannot find c_base's compile command"; exit 1; }
echo "compile: $CMD <file>"

# two source trees: new (this branch) and old (the changed files as BASE has them)
for side in old new; do
  mkdir -p "$OUT/$side/tree"
  (cd "$ROOT" && tar --exclude='c_base/build' --exclude='c_base/dist' -cf - c_base shared | tar -xf - -C "$OUT/$side/tree")
done
CHANGED=$(git -C "$ROOT" diff --name-only "$BASE" -- c_base || true)
echo "changed files: $(echo $CHANGED | tr '\n' ' ')"
for f in $CHANGED; do
  if git -C "$ROOT" cat-file -e "$BASE:$f" 2>/dev/null; then git -C "$ROOT" show "$BASE:$f" > "$OUT/old/tree/$f"; else rm -f "$OUT/old/tree/$f"; fi
done

status=0
for f in $CHANGED; do
  case "$f" in *.c) ;; *) continue;; esac
  rel=${f#c_base/}
  case "$rel" in core/test/*) continue;; esac   # the tests are not the hot path
  name=$(echo "$rel" | tr '/' '_')
  if [ ! -f "$OUT/old/tree/$f" ]; then echo "ADDED: $rel (a new file: nothing of foolish's old code is in it)"; continue; fi
  for side in old new; do
    (cd "$OUT/$side/tree/c_base" && $CMD -c "$rel" -o "$OUT/$side/$name.o")
    objdump -d --no-show-raw-insn --no-addresses "$OUT/$side/$name.o" | sed -E 's/<[^>]*\+0x[0-9a-f]+>//; s/0x[0-9a-f]+//g' > "$OUT/$side/$name.dis"
  done
  if diff -q "$OUT/old/$name.dis" "$OUT/new/$name.dis" > /dev/null; then
    echo "IDENTICAL disassembly: $rel"
  else
    echo "DIFFERS: $rel"
    diff "$OUT/old/$name.dis" "$OUT/new/$name.dis" > "$OUT/$name.diff" || true
    python3 - "$OUT/old/$name.dis" "$OUT/new/$name.dis" "$OUT/$name.functions.diff" <<'EOF'
import sys, re, difflib
def funcs(path):
    d, cur = {}, None
    for line in open(path):
        m = re.match(r'^<(.+)>:$', line.strip())
        if m: cur = m.group(1); d[cur] = []
        elif cur: d[cur].append(line.rstrip())
    return d
a, b = funcs(sys.argv[1]), funcs(sys.argv[2])
same = [f for f in a if f in b and a[f] == b[f]]
changed = [f for f in a if f in b and a[f] != b[f]]
only_old = [f for f in a if f not in b]; only_new = [f for f in b if f not in a]
print(f"  functions: {len(same)} identical, {len(changed)} changed ({', '.join(changed) or '-'}), {len(only_old)} removed ({', '.join(only_old) or '-'}), {len(only_new)} added ({', '.join(only_new) or '-'})")
with open(sys.argv[3], 'w') as out:
    for f in changed:
        out.write(f"==== {f}\n")
        out.writelines(l + "\n" for l in difflib.unified_diff(a[f], b[f], 'old', 'new', lineterm='', n=2))
EOF
    status=1
  fi
done
echo "diffs under $OUT"
exit $status
