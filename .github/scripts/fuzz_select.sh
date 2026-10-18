#!/bin/bash
# fuzz_select.sh - which of c_base's libFuzzer targets a change can reach.
#
#   .github/scripts/fuzz_select.sh --base <sha> [--head <sha>]
#   .github/scripts/fuzz_select.sh --files <path>...      (a synthetic diff)
#
# Prints the selected target names on stdout, one line, space separated
# (empty when none), and why on stderr. Run it from anywhere in the repo after
# `make -C c_base fuzz`: it reads the objects that build left behind.
#
# Nothing here is a hand list of files. A target's sources are found in three
# steps, each answered by the toolchain:
#   1. its link line: the prerequisites make has for build/fuzz/<target>
#      (`make -pq`, the rule fuzz/fuzz.mk generates);
#   2. the objects on that line its own code can reach: starting from
#      fz_<target>.o, every object that defines a symbol a reached object
#      leaves undefined (`nm`), so a library file the target links but never
#      calls does not select it;
#   3. every file those objects were built from: make's prerequisite graph,
#      which holds the compiler's -MD dependency files (each source and every
#      header it included) and the generator rules (build/gen/*.c from their
#      inputs), walked to the files in the repository.
# A changed file selects every target whose set holds it. A file under the
# target's corpus (fuzz/corpus/<target>, beside fz_<target>.c) selects that
# target. A change to any makefile either build reads (MAKEFILE_LIST of
# c_base's and of the kernel's, which c_base asks for its source list), to the
# workflow, or to this script selects every target: flags change every object.
# Files moving keeps all of this true with no edit here.
set -euo pipefail

usage() { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

base='' head=HEAD files=()
while [ $# -gt 0 ]; do
  case "$1" in
    --base) base=${2:?}; shift 2 ;;
    --head) head=${2:?}; shift 2 ;;
    --files) shift; files=("$@"); set --; [ ${#files[@]} -gt 0 ] || usage ;;
    *) usage ;;
  esac
done
[ -n "$base" ] || [ ${#files[@]} -gt 0 ] || usage

root=$(git rev-parse --show-toplevel)
cb=$root/c_base
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ ${#files[@]} -eq 0 ]; then
  git -C "$root" diff --no-renames --name-only "$base...$head" > "$work/changed"
else
  printf '%s\n' "${files[@]}" > "$work/changed"
fi

# The make database of the fuzz build, and the kernel make's makefiles.
make -C "$cb" -pq fuzz > "$work/db" 2>/dev/null || true
kdir=$(sed -n 's/^KDIR := //p' "$work/db" | head -1)
make -C "$cb/$kdir" -pq print-kernel 2>/dev/null | sed -n 's/^MAKEFILE_LIST := //p' > "$work/kmk" || true
targets=$(sed -n 's/^FUZZ_TARGETS := //p' "$work/db" | head -1)
[ -n "$targets" ] || { echo "fuzz_select: no FUZZ_TARGETS in the make database" >&2; exit 1; }

# Every object some target links, through nm once.
for t in $targets; do
  sed -n "s|^build/fuzz/$t: ||p" "$work/db" | head -1 | tr ' ' '\n' | grep '\.o$'
done | sort -u > "$work/objs"
[ -s "$work/objs" ] || { echo "fuzz_select: no fuzz objects; run make -C c_base fuzz first" >&2; exit 1; }
while read -r o; do
  [ -f "$cb/$o" ] || { echo "fuzz_select: $o is missing; run make -C c_base fuzz first" >&2; exit 1; }
done < "$work/objs"
(cd "$cb" && xargs nm -A -P < "$work/objs") > "$work/nm"

# Steps 1-3 above: "<target> <repo path>" for every file a target is built from.
awk -v targets="$targets" -v cbrel=c_base '
  function norm(p,   n, i, a, out, k, s) {
    if (p ~ /^\//) return ""                       # outside the repository
    n = split(cbrel "/" p, a, "/"); k = 0
    for (i = 1; i <= n; i++) {
      if (a[i] == "" || a[i] == ".") continue
      if (a[i] == "..") { if (k > 0) k--; continue }
      s[++k] = a[i]
    }
    out = s[1]; for (i = 2; i <= k; i++) out = out "/" s[i]
    return out
  }
  FILENAME == ARGV[1] {                            # nm -A -P
    obj = $1; sub(/:$/, "", obj); sym = $2; ty = $3
    if (ty == "U" || ty == "w" || ty == "v") und[obj] = und[obj] " " sym
    else if (ty ~ /^[A-Z]$/) defs[sym] = defs[sym] " " obj
    next
  }
  /^[^#\t ][^=]*: / && $0 !~ / :?= / {             # make -p rule lines
    i = index($0, ": "); tgt = substr($0, 1, i - 1)
    pre[tgt] = pre[tgt] " " substr($0, i + 2)
  }
  END {
    nt = split(targets, T, " ")
    for (ti = 1; ti <= nt; ti++) {
      t = T[ti]; delete link; delete reach; delete seen
      n = split(pre["build/fuzz/" t], L, " ")
      start = ""
      for (i = 1; i <= n; i++) if (L[i] ~ /\.o$/) {
        link[L[i]] = 1
        if (L[i] ~ ("/fz_" t "\\.o$")) start = L[i]
      }
      if (start == "") { print "fuzz_select: no fz_" t ".o on its link line" > "/dev/stderr"; exit 1 }
      # 2. objects reached through undefined symbols
      q[1] = start; reach[start] = 1; qh = 1; qt = 1
      while (qh <= qt) {
        o = q[qh++]
        ns = split(und[o], S, " ")
        for (j = 1; j <= ns; j++) {
          nd = split(defs[S[j]], D, " ")
          for (k = 1; k <= nd; k++)
            if ((D[k] in link) && !(D[k] in reach)) { reach[D[k]] = 1; q[++qt] = D[k] }
        }
      }
      # 3. files those objects are built from
      sp = 0
      for (o in reach) { st[++sp] = o; seen[o] = 1 }
      while (sp > 0) {
        f = st[sp--]
        np = split(pre[f], P, " ")
        for (j = 1; j <= np; j++)
          if (P[j] != "|" && !(P[j] in seen)) { seen[P[j]] = 1; st[++sp] = P[j] }
      }
      for (f in seen) { r = norm(f); if (r != "") print t, r }
      if (ENVIRON["FUZZ_SELECT_STATS"] != "") {
        nr = 0; for (o in reach) nr++
        nl = 0; for (o in link) nl++
        printf "fuzz_select: %s reaches %d of the %d objects it links\n", t, nr, nl > "/dev/stderr"
      }
    }
  }
' "$work/nm" "$work/db" | sort -u > "$work/deps"
# FUZZ_SELECT_DEPS=<file>: keep the whole "<target> <path>" table to read.
[ -z "${FUZZ_SELECT_DEPS:-}" ] || cp "$work/deps" "$FUZZ_SELECT_DEPS"

# The makefiles of both builds, the workflow and this script select everything.
{
  sed -n 's/^MAKEFILE_LIST := //p' "$work/db" | head -1 | tr ' ' '\n' | sed '/^$/d; /\.d$/d; s|^|c_base/|'
  tr ' ' '\n' < "$work/kmk" | sed '/^$/d; s|^|c_base/'"$kdir"'/|'
} | awk '{ n = split($0, a, "/"); k = 0
           for (i = 1; i <= n; i++) { if (a[i] == "..") k--; else if (a[i] != "." && a[i] != "") s[++k] = a[i] }
           o = s[1]; for (i = 2; i <= k; i++) o = o "/" s[i]; print o }' > "$work/all_triggers"
echo .github/workflows/c_base.yml >> "$work/all_triggers"
echo .github/scripts/fuzz_select.sh >> "$work/all_triggers"

all=$(grep -Fxf "$work/all_triggers" "$work/changed" || true)
if [ -n "$all" ]; then
  echo "fuzz_select: every target, a build rule or the workflow changed:" >&2
  while read -r f; do echo "  $f"; done <<< "$all" >&2
  echo "$targets"
  exit 0
fi

selected=''
for t in $targets; do
  src=$(awk -v t="$t" '$1 == t && $2 ~ ("/fz_" t "\\.c$") { print $2 }' "$work/deps" | head -1)
  corpus=${src%/*}/corpus/$t/
  hits=$( { awk -v t="$t" '$1 == t { print $2 }' "$work/deps" | grep -Fxf - "$work/changed" || true
            grep -F "$corpus" "$work/changed" | awk -v c="$corpus" 'index($0, c) == 1' || true; } | sort -u)
  if [ -n "$hits" ]; then
    selected="$selected $t"
    echo "fuzz_select: $t: ${hits//$'\n'/ }" >&2
  fi
done
selected=${selected# }
if [ -z "$selected" ]; then
  echo "fuzz_select: no target is built from a changed file ($(wc -l < "$work/changed" | tr -d ' ') changed)" >&2
fi
echo "$selected"
