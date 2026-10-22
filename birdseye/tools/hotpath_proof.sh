#!/bin/bash
# hotpath_proof.sh - the static proof that the dev-only bot table (foolish/server/src/app/dev_bot_table.c
# and the small changes it needed in c_base and table_ops) leaves the game server's move and push path
# as it was. The method is lichess_ws's tools/hotpath_proof.sh: two copies of the sources, one with
# every changed file as BASE has it, each changed .c compiled with c_base's own compile command, every
# function's disassembly compared, and the functions that differ named with their diffs.
#
#   birdseye/tools/hotpath_proof.sh [BASE]     (default 061fdcb27, the branch before the route)
#
# Prints the verdict per file and per function; writes the diffs under $OUT (default build/hotpath
# under birdseye/server). Exit 0 always: which functions may differ is the reader's call, and the
# output says which ones did.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BASE=${1:-061fdcb27}
OUT=${OUT:-$ROOT/birdseye/server/build/hotpath}
rm -rf "$OUT"; mkdir -p "$OUT"

cd "$ROOT/c_base"
CMD=$(make -n -W ../foolish/server/src/app/table_ops.c build/obj/cbase/_root/foolish/server/src/app/table_ops.o 2>/dev/null \
      | grep -E 'table_ops\.c' | head -1 | sed -E 's/ -MD.*//')
[ -n "$CMD" ] || { echo "cannot find c_base's compile command"; exit 1; }
echo "compile: $CMD <file>"

for side in old new; do
  mkdir -p "$OUT/$side/tree"
  (cd "$ROOT" && tar --exclude='c_base/build' --exclude='c_base/dist' -cf - c_base shared foolish/c/src foolish/server/src | tar -xf - -C "$OUT/$side/tree")
done
CHANGED=$(git -C "$ROOT" diff --name-only "$BASE" -- c_base/core/src foolish/server/src foolish/c/src shared/c || true)
echo "changed files: $(echo $CHANGED | tr '\n' ' ')"
for f in $CHANGED; do
  if git -C "$ROOT" cat-file -e "$BASE:$f" 2>/dev/null; then git -C "$ROOT" show "$BASE:$f" > "$OUT/old/tree/$f"; else rm -f "$OUT/old/tree/$f"; fi
done

# Every server source is compiled, not only the changed ones: a changed header reaches files whose
# own text did not change (table_ops.h is read by every request path).
ALL=$(cd "$ROOT" && { ls c_base/core/src/*.c c_base/core/src/*/*.c foolish/server/src/*/*.c; echo "$CHANGED" | grep '\.c$' || true; } | sort -u)
n_same=0
for f in $ALL; do
  name=$(echo "$f" | tr '/' '_')
  for side in old new; do
    src="$OUT/$side/tree/$f"
    if [ ! -f "$src" ]; then : > "$OUT/$side/$name.dis"; continue; fi
    # c_base's command is relative to c_base/: a foolish file is ../foolish/..., a core file core/...
    rel=${f#c_base/}; [ "$rel" = "$f" ] && rel="../$f"
    (cd "$OUT/$side/tree/c_base" && $CMD -c "$rel" -o "$OUT/$side/$name.o")
    /usr/bin/objdump -d --no-show-raw-insn --no-addresses "$OUT/$side/$name.o" | sed -E '/file format/d; s/<[^>]*\+0x[0-9a-f]+>//; s/0x[0-9a-f]+//g' > "$OUT/$side/$name.dis"
  done
  if diff -q "$OUT/old/$name.dis" "$OUT/new/$name.dis" > /dev/null; then
    n_same=$((n_same + 1))
    continue
  fi
  echo "DIFFERS: $f"
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
done
echo "IDENTICAL disassembly: $n_same files (every other server source)"
echo "diffs under $OUT"
