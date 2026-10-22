#!/bin/bash
# mutate.sh - the mutation check: each line of a mutants file breaks one thing in one source file
# (an exact text, which must occur once, replaced by another; "\n" in either is a newline), the
# tests that should notice run, and the line passes only when they fail. A mutant that does not
# compile proves nothing and is reported INVALID. The file is restored from a copy after each mutant
# (never from git), and its objects and every linked program deleted: make's one-second mtimes
# would otherwise keep the mutant's object for the next run (and make 3.81, with .SECONDARY, spins
# for minutes on a missing object whose program still exists).
#
#   tools/mutate.sh tools/mutants.txt          (from c_chess/)
#
# A mutants line:  <file> ::: <exact text> ::: <replacement> ::: <make targets> ::: <test command>
set -uo pipefail
cd "$(dirname "$0")/.."
killed=0; bad=0
field() { python3 -c 'import sys; print(sys.argv[1].split(" ::: ")[int(sys.argv[2])])' "$1" "$2"; }
while IFS= read -r line; do
  [[ -z "$line" || "$line" == \#* ]] && continue
  file=$(field "$line" 0); from=$(field "$line" 1); to=$(field "$line" 2); targets=$(field "$line" 3); test=$(field "$line" 4)
  cp "$file" "$file.mutant-orig"
  if ! python3 - "$file" "$from" "$to" <<'PY'
import sys
p, a, b = sys.argv[1:4]
a, b = a.replace('\\n', '\n'), b.replace('\\n', '\n')
s = open(p).read()
if s.count(a) != 1: sys.exit(1)
open(p, 'w').write(s.replace(a, b))
PY
  then echo "SKIP (text not found exactly once): $file: $from"; mv "$file.mutant-orig" "$file"; bad=$((bad+1)); continue; fi
  obj=$(basename "$file" .c).o
  if ! make -s -j4 $targets > /tmp/cc-mutant.log 2>&1; then
    echo "INVALID (does not compile): $file: $from -> $to"; bad=$((bad+1))
  elif bash -c "$test" > /tmp/cc-mutant.log 2>&1; then
    echo "SURVIVED: $file: $from -> $to"; bad=$((bad+1))
  else
    echo "killed:   $file: $from -> $to   ($(grep -m1 -E '^FAIL' /tmp/cc-mutant.log | cut -c1-120))"; killed=$((killed+1))
  fi
  mv "$file.mutant-orig" "$file"
  find build/obj -name "$obj" -delete
  rm -f build/c_chess build/chessbench build/test/unit_* build/test/smoke* build/asan/c_chess build/asan/chessbench build/asan/test/*
done < "$1"
echo "mutants killed $killed, not killed $bad"
make -s -j4 all > /dev/null 2>&1
[ "$bad" -eq 0 ]
