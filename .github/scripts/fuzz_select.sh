#!/bin/bash
# fuzz_select.sh - which libFuzzer targets a change can reach.
#
#   .github/scripts/fuzz_select.sh --base <sha> [--head <sha>]
#   .github/scripts/fuzz_select.sh [--against <rev|dir>] --files <path>...
#   .github/scripts/fuzz_select.sh --all
#
# Prints the selected target names on stdout, one line, space separated
# (empty when none), and why on stderr; --all prints every target. Run it from
# anywhere in the repo after `make -C c_base fuzz`: it reads the objects that
# build left behind (--all needs no build).
#
# THREE BUILDS, AND THE NAMES ON STDOUT:
#   <t>          c_base's fuzz build (c_base/fuzz/fuzz.mk, FUZZ_ROOTS):
#                make -C c_base build/fuzz/<t>; fuzz-smoke FUZZ_TARGETS=<t>
#   client:<t>   c_base/client's (its Makefile's `fuzz` goal; today client):
#                make -C c_base/client build/fuzz/<t>; fuzz-smoke
#   product:<t>  shared/fuzz's (its Makefile's `all` goal, TARGETS):
#                make -C shared/fuzz build/<t>; smoke TARGETS=<t>
# c_base's names carry no prefix (the contract before the other two came in);
# a consumer runs a name by its prefix. The second and third builds write one
# binary per target with one command (compile and link together, no objects),
# so the steps below are the first build's; the other two are THE ONE-COMMAND
# BUILDS further down, under the same rule.
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
# target.
#
# A makefile is in no target's set: what it decides is the COMMANDS, so those
# are what is compared, and no makefile is named here. `make -n -B fuzz` prints
# every command the fuzz build runs; it is run in this working tree and in a
# scratch checkout of the base (the merge base of --base and --head; with
# --files, --against, HEAD by default, or a directory holding a tree), and
# each command is keyed by the file it writes. An object or generated file
# whose command differs counts as changed, exactly like a changed source, so
# it selects the targets that reach it; a target whose link line differs (its
# flags or libraries, an object it reaches newly on it, or an object taken off
# it whose source still exists) is selected. So a flag in CB_CFLAGS,
# KERNEL_CAPS or FUZZ_SAN, or a new compile recipe in c_rules.mk, reruns every
# target (every compile line changed); FUZZ_EXTRA_<t> or FUZZ_EXCLUDE_<t>
# reruns <t>; a comment, a test rule, a wasm or iOS rule reruns nothing,
# because the fuzz build runs the same commands. A base whose fuzz build does
# not even dry-run selects every target. The workflow and this script build
# no target, so a change to them alone fuzzes nothing.
# Files moving keeps all of this true with no edit here.
#
# THE ONE-COMMAND BUILDS (one_builds below: c_base/client and shared/fuzz).
# `make -n -B <goal>` in the build's directory prints one command per target,
# keyed by the binary it writes (-o); the target is that binary's file name. A
# target is selected when its command differs from the base's (a flag, a
# source added to or taken off its list; or the base does not build it), when
# a changed file is one the command compiles or a header the compiler reads
# for it (the same command with -M, its -o and libraries dropped), or when a
# changed file is under its corpus (corpus/<t> beside fz_<t>.c). A base build
# that does not dry-run selects every target of that build. The changed files
# are the diff's own; what c_base's commands make "changed" is c_base's.
set -euo pipefail
# No `| head` anywhere: under pipefail, GNU sed (Linux, CI) fails on the
# broken pipe when head stops reading early, where macOS sed is silent. A
# reader stops itself instead (sed q, awk exit, or one awk over the file).

usage() { sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

base='' head=HEAD against='' files=() all=''
while [ $# -gt 0 ]; do
  case "$1" in
    --all) all=1; shift ;;
    --base) base=${2:?}; shift 2 ;;
    --head) head=${2:?}; shift 2 ;;
    --against) against=${2:?}; shift 2 ;;
    --files) shift; files=("$@"); set --; [ ${#files[@]} -gt 0 ] || usage ;;
    *) usage ;;
  esac
done
[ -n "$all" ] || [ -n "$base" ] || [ ${#files[@]} -gt 0 ] || usage

# Physical paths (pwd -P): make's CURDIR is one, so a makefile's $(abspath)
# names a file by it, and the commands are compared with the tree's path cut.
root=$(cd "$(git rev-parse --show-toplevel)" && pwd -P)
cb=$root/c_base
work=$(cd "$(mktemp -d)" && pwd -P)
trap 'rm -rf "$work"' EXIT

# The one-command builds: "<directory> <make goal> <name prefix>".
one_builds=(
  'c_base/client fuzz client'
  'shared/fuzz all product'
)
# one_cmds <tree> <directory> <goal>: "<binary>\t<command>" for each target
# the goal builds, the tree's path written @/. Fails when make does.
one_cmds() {
  local out
  out=$(MAKEFLAGS='' make -s -C "$1/$2" -n -B "$3" 2>/dev/null) || return 1
  awk -v r="$1/" -v OFS='\t' '
    { while ((i = index($0, r)) > 0) $0 = substr($0, 1, i - 1) "@/" substr($0, i + length(r))
      n = split($0, w, " "); out = ""
      for (i = 1; i < n; i++) if (w[i] == "-o") out = w[i + 1]
      if (out != "") print out, $0                 # mkdir has no -o
    }' <<< "$out"
}

if [ -n "$all" ]; then
  list=$(MAKEFLAGS='' make -s --no-print-directory -C "$cb" print-fuzz-targets)
  for b in "${one_builds[@]}"; do
    read -r dir goal prefix <<< "$b"
    one_cmds "$root" "$dir" "$goal" > "$work/one" || { echo "fuzz_select: make -n $goal fails in $dir" >&2; exit 1; }
    while IFS=$'\t' read -r out _; do list="$list $prefix:${out##*/}"; done < "$work/one"
  done
  echo "$list"
  exit 0
fi

if [ ${#files[@]} -eq 0 ]; then
  git -C "$root" diff --no-renames --name-only "$base...$head" > "$work/changed"
  against=$(git -C "$root" merge-base "$base" "$head")
else
  printf '%s\n' "${files[@]}" > "$work/changed"
  against=${against:-HEAD}
fi
cp "$work/changed" "$work/changed.diff"           # the diff's own, for the one-command builds

# The make database of the fuzz build.
make -C "$cb" -pq fuzz > "$work/db" 2>/dev/null || true
targets=$(sed -n '/^FUZZ_TARGETS := /{s///p;q;}' "$work/db")
[ -n "$targets" ] || { echo "fuzz_select: no FUZZ_TARGETS in the make database" >&2; exit 1; }

# Every object some target links, through nm once.
for t in $targets; do
  sed -n "\|^build/fuzz/$t: |{s|||p;q;}" "$work/db" | tr ' ' '\n' | grep '\.o$'
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
      print t, cbrel "/build/fuzz/" t                # its link line (step 4)
      if (ENVIRON["FUZZ_SELECT_STATS"] != "") {
        nr = 0; for (o in reach) nr++
        nl = 0; for (o in link) nl++
        printf "fuzz_select: %s reaches %d of the %d objects it links\n", t, nr, nl > "/dev/stderr"
      }
    }
  }
' "$work/nm" "$work/db" | sort -u > "$work/deps"

# 4. The commands (the header's "a makefile"): "C <file> <command>" for each
# file the fuzz build writes, "L <target> <link command less its objects>" and
# "O <target> <object>", from a dry run in the tree $1. Fails when make does.
cmds() {
  local out
  out=$(MAKEFLAGS='' make -s -C "$1/c_base" -n -B fuzz 2>/dev/null) || return 1
  awk -v r="$1/" -v OFS='\t' '
    { while ((i = index($0, r)) > 0) $0 = substr($0, 1, i - 1) "@/" substr($0, i + length(r))
      n = split($0, w, " "); out = ""
      for (i = 1; i < n; i++) if (w[i] == "-o" || w[i] == ">") out = w[i + 1]
      if (out == "") next                          # mkdir, echo, the stamp swap
      sub(/\.new$/, "", out)
      if (out !~ /^build\/fuzz\/[^\/]+$/) { print "C", out, $0; next }
      t = substr(out, 12); cmd = ""
      for (i = 1; i <= n; i++) if (w[i] ~ /\.o$/) print "O", t, w[i]; else cmd = cmd " " w[i]
      print "L", t, cmd
    }' <<< "$out"
}
if [ -d "$against" ]; then
  tree=$(cd "$against" && pwd -P)
else
  tree=$work/tree; mkdir "$tree"
  GIT_INDEX_FILE=$work/index git -C "$root" --work-tree="$tree" read-tree -u --reset "$against"
fi
cmds "$root" > "$work/cmds.head" || { echo "fuzz_select: make -n fuzz fails in this tree" >&2; exit 1; }
base_ok=1
if ! cmds "$tree" > "$work/cmds.base"; then
  echo "fuzz_select: every c_base target, the base's fuzz build does not dry-run ($against)" >&2
  base_ok=''
fi
# The files the base builds differently, as changed files: "<path>\t<why>".
[ -z "$base_ok" ] || (cd "$cb" && awk -F'\t' -v cbrel=c_base '
  FILENAME == ARGV[1] { if ($1 == "C") bc[$2] = $3; else if ($1 == "L") bl[$2] = $3; else bo[$2 SUBSEP $3] = 1; next }
  FILENAME == ARGV[2] { if ($1 == "C") hc[$2] = $3; else if ($1 == "L") hl[$2] = $3; else ho[$2 SUBSEP $3] = 1; next }
  { split($0, d, " "); dep[d[1] SUBSEP d[2]] = 1 }  # the "<target> <path>" table
  function source(o,   n, w, i) {                   # what a base compile of o read
    n = split(bc[o], w, " ")
    for (i = 1; i < n; i++) if (w[i] == "-c") return w[i + 1]
    return ""
  }
  function exists(f,   l, r) { if (f == "") return 0; r = (getline l < f); close(f); return r >= 0 }
  END {
    for (o in hc) if (!(o in bc)) print cbrel "/" o "\tthe base does not build it"
    else if (bc[o] != hc[o]) print cbrel "/" o "\tits command changed"
    for (t in hl) {
      why = ""
      if ((t in bl) && bl[t] != hl[t]) why = " its link command changed;"
      for (k in ho) if (!(k in bo)) {
        split(k, p, SUBSEP)
        if (p[1] == t && ((t SUBSEP cbrel "/" p[2]) in dep)) why = why " now links " p[2] ";"
      }
      if (t in bl) for (k in bo) if (!(k in ho)) {
        split(k, p, SUBSEP)
        if (p[1] == t && exists(source(p[2]))) why = why " no longer links " p[2] ";"
      }
      if (why != "") print cbrel "/build/fuzz/" t "\t" substr(why, 2, length(why) - 2)
    }
  }' "$work/cmds.base" "$work/cmds.head" "$work/deps") | sort > "$work/commands"
if [ -s "$work/commands" ]; then
  echo "fuzz_select: the base builds $(wc -l < "$work/commands" | tr -d ' ') files differently:" >&2
  # One reader, no `| head`: under pipefail a writer cut off by head fails.
  awk -F'\t' 'NR <= 20 { print "  " $1 ": " $2 } END { if (NR > 20) print "  and " NR - 20 " more" }' "$work/commands" >&2
  cut -f1 "$work/commands" >> "$work/changed"
fi

# hits <target> <changed list> <short name>: the changed files in <target>'s
# set or under its corpus (corpus/<short name> beside its fz_<short name>.c).
hits() {
  local src corpus
  src=$(awk -v t="$1" -v s="$3" '$1 == t && $2 ~ ("/fz_" s "\\.c$") { print $2; exit }' "$work/deps")
  corpus=${src%/*}/corpus/$3/
  { awk -v t="$1" '$1 == t { print $2 }' "$work/deps" | grep -Fxf - "$2" || true
    grep -F "$corpus" "$2" | awk -v c="$corpus" 'index($0, c) == 1' || true; } | sort -u
}

selected=''
for t in $targets; do
  h=$([ -n "$base_ok" ] || echo "the base's fuzz build does not dry-run"; hits "$t" "$work/changed" "$t")
  if [ -n "$h" ]; then
    selected="$selected $t"
    echo "fuzz_select: $t: ${h//$'\n'/ }" >&2
  fi
done

# THE ONE-COMMAND BUILDS (the header). Each target's files go in the same
# "<name> <repo path>" table: what its command names and the compiler reads.
for b in "${one_builds[@]}"; do
  read -r dir goal prefix <<< "$b"
  one_cmds "$root" "$dir" "$goal" > "$work/one.head" || { echo "fuzz_select: make -n $goal fails in $dir" >&2; exit 1; }
  [ -s "$work/one.head" ] || { echo "fuzz_select: make -n $goal in $dir builds no target" >&2; exit 1; }
  ob_ok=1
  if ! one_cmds "$tree" "$dir" "$goal" > "$work/one.base"; then
    echo "fuzz_select: every $prefix target, the base's $dir build does not dry-run ($against)" >&2
    ob_ok=''
  fi
  while IFS=$'\t' read -r out cmd; do
    s=${out##*/} t=$prefix:${out##*/} why=''
    if [ -z "$ob_ok" ]; then
      why="the base's build does not dry-run"
    else
      bcmd=$(awk -F'\t' -v o="$out" '$1 == o { print $2; exit }' "$work/one.base")
      if [ -z "$bcmd" ]; then why="the base does not build it"
      elif [ "$bcmd" != "$cmd" ]; then why="its command changed"; fi
    fi
    # The same command with -M: every file it compiles and every header read.
    mcmd=$(awk -v r="$root/" '{
      while ((i = index($0, "@/")) > 0) $0 = substr($0, 1, i - 1) r substr($0, i + 2)
      n = split($0, w, " "); out = ""
      for (i = 1; i <= n; i++) {
        if (w[i] == "-o") { i++; continue }
        if (w[i] ~ /^-l/) continue
        out = out " " w[i]
      }
      print out " -M" }' <<< "$cmd")
    (cd "$root/$dir" && sh -c "$mcmd") < /dev/null > "$work/m" 2> "$work/m.err" ||
      { echo "fuzz_select: $t: the compiler cannot list what it reads:" >&2; sed 's/^/  /' "$work/m.err" >&2; exit 1; }
    awk -v t="$t" -v r="$root/" -v d="$dir" '
      function norm(p,   n, i, a, out, k, s) {
        if (p ~ /^\//) { if (index(p, r) != 1) return ""; p = substr(p, length(r) + 1) }
        else p = d "/" p
        n = split(p, a, "/"); k = 0
        for (i = 1; i <= n; i++) {
          if (a[i] == "" || a[i] == ".") continue
          if (a[i] == "..") { if (k > 0) k--; continue }
          s[++k] = a[i]
        }
        out = s[1]; for (i = 2; i <= k; i++) out = out "/" s[i]
        return out
      }
      { for (i = 1; i <= NF; i++) if ($i != "\\" && $i !~ /:$/) { f = norm($i); if (f != "") print t, f } }
    ' "$work/m" | sort -u >> "$work/deps"
    h=$([ -z "$why" ] || echo "$why"; hits "$t" "$work/changed.diff" "$s")
    if [ -n "$h" ]; then
      selected="$selected $t"
      echo "fuzz_select: $t: ${h//$'\n'/ }" >&2
    fi
  done < "$work/one.head"
done
# FUZZ_SELECT_DEPS=<file>: keep the whole "<target> <path>" table to read.
[ -z "${FUZZ_SELECT_DEPS:-}" ] || sort -u "$work/deps" > "$FUZZ_SELECT_DEPS"

selected=${selected# }
if [ -z "$selected" ]; then
  echo "fuzz_select: no target is built from a changed file ($(wc -l < "$work/changed.diff" | tr -d ' ') changed)" >&2
fi
echo "$selected"
