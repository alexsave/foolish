#!/usr/bin/env bash
# lint_architecture.sh - no Swift knows the kernel's shape or a kernel byte.
#
# The Swift twin of foolish/e2e/no_ts_game_shape.test.ts, in the plain-grep
# form of foolish/ios/scripts/lint_architecture.sh. The doctrine is
# docs/ARCHITECTURE_AS_A_PATTERN.md, Part 1: the kernel owns the shape, and
# where Swift touches a kernel value it goes through the readers structgen
# generated from the real C layouts (chuiniu/ios/Generated/, a build output of
# `make -C chuiniu/c ios-lib`), behind the one seam (DECISIONS I2, I11).
#
# Over every chuiniu Swift file outside Generated/ (ChuiniuKit,
# ChuiniuKitTests, ChuiniuMessages, ChuiniuMessagesApp), four rules:
#
#   1. `import CChuiniu` only in ChuiniuKit/Kernel/BridgeKernel.swift, the
#      seam's one kernel. A test reaches the kernel through it too.
#
#   2. No byte is read by hand: no withUnsafeBytes, withUnsafeMutableBytes,
#      withMemoryRebound, bindMemory, assumingMemoryBound, load(fromByteOffset:),
#      loadUnaligned, storeBytes, .pointee, a buffer pointer made from a start
#      address, a pointer advanced(by:), MemoryLayout offsets, or Data copied
#      from a pointer plus an offset. A layout read through a generated reader
#      needs none of them. Bytes handed TO C ([UInt8] and String arguments)
#      cross as Swift arrays and strings, which also need none.
#
#   3. In a file that reaches the kernel (it names a cn_api_ call, a reader's
#      Snap or the module), no shift by 4, 8, 16 or 24 bits: a nibble or a
#      byte of a layout. A colour's hex literal in a file that never sees the
#      kernel is not a layout and is not scanned.
#
#   4. No struct or class declares two or more of the kernel's own field
#      names. The names are read from the generated readers on every run: the
#      fields of every `Cn*Snap` whose name is a compound of the C struct's
#      (bidQ, diceN, cupsUp: a C snake_case name structgen turned to camel
#      case). A single English word (phase, round, winner) is anybody's word
#      and is left out; two of the kernel's own spellings together are a
#      CnView or a CnStageHud coming back by hand. The seam's TableModel
#      holds the kernel's answers in its own words, so it declares one
#      (myDice), and the rule holds that it never grows a second.
#
# THE ALLOWLIST IS SHRINK-ONLY AND EMPTY. `ALLOWED` below would name a
# `file|rule` that is still tolerated; it has no entries, and the lint fails
# if anyone adds one (the doctrine of no_ts_game_shape's NOT_YET_GENERATED:
# the test asserts the list is empty, not a ceiling). Fix the code instead.
#
# The lint first proves it is not vacuous: it reaches the product's files,
# the field list is read from the readers, and each detector finds the
# forbidden thing in a probe and passes the allowed one.
#
# Usage: chuiniu/ios/scripts/lint_architecture.sh   (after `make -C chuiniu/c ios-lib`)
# Runs from scripts/mac_tests.sh after the readers are generated. No Xcode needed.
# Mutation checks: chuiniu/ios/TESTS_MUTATED.md, "lint_architecture.sh".
set -euo pipefail
cd "$(dirname "$0")/.."                # chuiniu/ios

GEN=Generated/ChuiniuKernel.swift
BRIDGE=ChuiniuKit/Kernel/BridgeKernel.swift
ROOTS=(ChuiniuKit ChuiniuKitTests ChuiniuMessages ChuiniuMessagesApp)

# SHRINK-ONLY, AND EMPTY: entries are "path|rule". Asserted empty below.
ALLOWED=()

fail=0
bad() { printf '  x %s\n' "$1"; fail=1; }

# ---- the code, without its comments and string literals --------------------
# Prints "LINE<TAB>code" for every line of $1: `//` comments, /* */ comments
# and the insides of "..." literals are blanked, so the rules can talk about
# what they forbid, and a test's message cannot trip them.
code_of() {
  awk '
    BEGIN { blk = 0 }
    {
      s = $0; out = ""; i = 1; n = length(s); str = 0
      while (i <= n) {
        c = substr(s, i, 1); c2 = substr(s, i, 2)
        if (blk) { if (c2 == "*/") { blk = 0; i += 2; continue } i++; continue }
        if (str) {
          if (c == "\\") { i += 2; continue }
          if (c == "\"") { str = 0; out = out "\"" }
          i++; continue
        }
        if (c2 == "//") break
        if (c2 == "/*") { blk = 1; i += 2; continue }
        if (c == "\"") { str = 1; out = out "\""; i++; continue }
        out = out c; i++
      }
      printf "%d\t%s\n", NR, out
    }' "$1"
}

# ---- the detectors: each prints "path:line: what" per hit ------------------

detect_import() {   # $1 file
  [ "$1" = "$BRIDGE" ] && return 0
  code_of "$1" | awk -F'\t' -v f="$1" '$2 ~ /^[[:space:]]*(@[A-Za-z_]+[[:space:]]+)*import[[:space:]]+CChuiniu([[:space:]]|$)/ {
    printf "%s:%d: import CChuiniu (only %s may)\n", f, $1, "BridgeKernel.swift" }'
}

# (bracketed, not escaped: awk -v eats a backslash)
BYTE_TOKENS='withUnsafeBytes|withUnsafeMutableBytes|withMemoryRebound|bindMemory|assumingMemoryBound|load[(]fromByteOffset|loadUnaligned|storeBytes|[.]pointee|Unsafe(Mutable)?(Raw)?BufferPointer[(]start:|advanced[(]by:|MemoryLayout<[^>]*>[.]offset|Data[(]bytes:[^,]*[+]'
detect_bytes() {    # $1 file
  code_of "$1" | awk -F'\t' -v f="$1" -v re="$BYTE_TOKENS" '{
    s = $2
    while (match(s, re)) { printf "%s:%d: %s\n", f, $1, substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH) } }'
}

REACHES_KERNEL='cn_api_|Snap[^A-Za-z0-9_]|Snap$|import[[:space:]]+CChuiniu|read(Cn|Sg)[A-Za-z]*\('
detect_shifts() {   # $1 file
  # a here-string, not a pipe: `grep -q` closing a pipe early is a SIGPIPE,
  # which pipefail turns into "does not reach the kernel" (a mutant found it)
  local code; code="$(code_of "$1")"
  grep -Eq "$REACHES_KERNEL" <<< "$code" || return 0
  awk -F'\t' -v f="$1" '{
    s = $2
    while (match(s, /(<<|>>)=?[[:space:]]*(4|8|16|24)([^0-9]|$)/)) {
      t = substr(s, RSTART, RLENGTH); sub(/[^0-9]$/, "", t)
      printf "%s:%d: shift %s on a file that reaches the kernel\n", f, $1, t; s = substr(s, RSTART + RLENGTH) } }' <<< "$code"
}

# The kernel's own field names, from the readers (rule 4).
kernel_fields() {
  awk '/^public struct Cn[A-Za-z0-9]*Snap/ { ins = 1; next }
       ins && /^}/ { ins = 0 }
       ins && match($0, /^    public let [A-Za-z0-9_]+:/) {
         n = substr($0, 16, RLENGTH - 16)
         if (n ~ /^[a-z]+[A-Z0-9]/ && n !~ /^pad[0-9]*$/) print n }' "$GEN" | sort -u
}

detect_shapes() {   # $1 file, $2 space-separated field names
  code_of "$1" | awk -F'\t' -v f="$1" -v fields="$2" '
    BEGIN { k = split(fields, a, " "); for (i = 1; i <= k; i++) F[a[i]] = 1; depth = 0; sp = 0 }
    function opens(s,  t) { t = s; return gsub(/\{/, "", t) }
    function closes(s,  t) { t = s; return gsub(/\}/, "", t) }
    {
      line = $2
      while (sp > 0 && depth < bdepth[sp]) { report(sp); sp-- }
      if (sp > 0 && depth == bdepth[sp] &&
          match(line, /^[[:space:]]*((public|private|fileprivate|internal|static|final|lazy|weak|unowned|nonisolated|override|@[A-Za-z_]+(\([^)]*\))?)[[:space:]]+)*(var|let)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*/)) {
        d = substr(line, RSTART, RLENGTH); sub(/.*(var|let)[[:space:]]+/, "", d)
        if (d in F) { got[sp] = got[sp] (got[sp] == "" ? "" : ", ") d; cnt[sp]++ }
      }
      if (match(line, /^[[:space:]]*((public|private|fileprivate|internal|final|@[A-Za-z_]+(\([^)]*\))?)[[:space:]]+)*(struct|class)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*/)) {
        d = substr(line, RSTART, RLENGTH); sub(/.*(struct|class)[[:space:]]+/, "", d)
        sp++; tname[sp] = d; tline[sp] = $1; bdepth[sp] = depth + 1; got[sp] = ""; cnt[sp] = 0
      }
      depth += opens(line) - closes(line)
    }
    function report(i) { if (cnt[i] >= 2) printf "%s:%d: struct %s declares %s\n", f, tline[i], tname[i], got[i] }
    END { while (sp > 0) { report(sp); sp-- } }'
}

# ---- 0. the lint is not vacuous --------------------------------------------
echo "[lint] the walk reaches the product, and the detectors see what they are for..."
[ -f "$GEN" ] || { echo "  x $GEN is missing: run 'make -C chuiniu/c ios-lib' first"; exit 1; }

files=()
while IFS= read -r f; do files+=("$f"); done < <(find "${ROOTS[@]}" -name '*.swift' -not -path '*/Generated/*' | sort)
[ "${#files[@]}" -ge 15 ] || bad "the walk found only ${#files[@]} Swift files"
for must in "$BRIDGE" ChuiniuKit/Kernel/KernelSeam.swift ChuiniuMessages/MessagesViewController.swift \
            ChuiniuKit/Screens/BubbleSnapshot.swift ChuiniuKitTests/BridgeKernelTests.swift; do
  grep -qxF "$must" <<< "$(printf '%s\n' "${files[@]}")" || bad "the walk does not reach $must"
done

FIELDS="$(kernel_fields | tr '\n' ' ')"
nfields=$(printf '%s' "$FIELDS" | wc -w | tr -d ' ')
[ "$nfields" -ge 30 ] || bad "only $nfields kernel field names read from $GEN"
for must in bidQ diceN cupsUp myDice nameX; do
  case " $FIELDS " in *" $must "*) ;; *) bad "the field list lacks $must";; esac
done
case " $FIELDS " in *" phase "*|*" winner "*|*" pad0 "*) bad "the field list holds an English word";; esac

probe_dir="$(mktemp -d -t chuiniu_lint)"
trap 'rm -rf "$probe_dir"' EXIT
probe() { printf '%s\n' "$2" > "$probe_dir/$1"; printf '%s' "$probe_dir/$1"; }
expect() {  # $1 want-count, $2 what, then the command
  local want="$1" what="$2"; shift 2
  local got; got=$("$@" | grep -c . || true)
  [ "$got" -eq "$want" ] || bad "detector self-test: $what (found $got, want $want)"
}
p=$(probe a.swift 'import CChuiniu
import Foundation')
expect 1 "an import of CChuiniu" detect_import "$p"
p=$(probe b.swift '// import CChuiniu, said in a comment
let s = "import CChuiniu"')
expect 0 "the import in a comment or a string" detect_import "$p"
p=$(probe c.swift 'func f(_ d: Data) -> Int { d.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt16.self) } }
let x = p.loadUnaligned(as: UInt32.self) + q.pointee')
expect 4 "four byte reads" detect_bytes "$p"
p=$(probe d.swift 'let b = [UInt8](data) // withUnsafeBytes is not needed
let t = "withUnsafeBytes"')
expect 0 "no byte read in plain code, a comment or a string" detect_bytes "$p"
p=$(probe e.swift 'let v = cn_api_table()
let q = Int(b[0]) | Int(b[1]) << 8')
expect 1 "a byte shift in a file that reaches the kernel" detect_shifts "$p"
p=$(probe f.swift 'let r = Double((hex >> 16) & 0xFF) / 255')
expect 0 "a colour shift in a file that never sees the kernel" detect_shifts "$p"
p=$(probe g.swift 'public struct Copy {
    public let bidQ: Int
    var note = "{"
    public var bidF: Int
}
struct Fine {
    let myDice: [Int]
    struct Inner { let diceN: Int }
    let phase: Int
}
enum Outer { struct Deep { var cupsUp = 0; let x = 1 } }')
expect 1 "a struct with two kernel fields, and not one with one" detect_shapes "$p" "$FIELDS"
p=$(probe h.swift 'struct Seat {
    let diceN: Int
    func f() { let myDice = 1; let bidQ = 2 }
}')
expect 0 "locals in a method are not fields" detect_shapes "$p" "$FIELDS"

# ---- 1-4. the product -------------------------------------------------------
offenders=""
for f in "${files[@]}"; do
  offenders+="$(detect_import "$f")"$'\n'
  offenders+="$(detect_bytes "$f")"$'\n'
  offenders+="$(detect_shifts "$f")"$'\n'
  offenders+="$(detect_shapes "$f" "$FIELDS")"$'\n'
done
offenders="$(printf '%s' "$offenders" | grep . || true)"

echo "[lint] import CChuiniu only in $BRIDGE; no byte read, layout shift or kernel struct by hand..."
if [ -n "$offenders" ]; then
  while IFS= read -r o; do
    path="${o%%:*}"
    for a in ${ALLOWED[@]+"${ALLOWED[@]}"}; do [ "${a%%|*}" = "$path" ] && continue 2; done
    bad "$o"
  done <<< "$offenders"
fi

echo "[lint] the allowlist is empty (shrink-only)..."
[ "${#ALLOWED[@]}" -eq 0 ] || bad "ALLOWED has ${#ALLOWED[@]} entries; it only shrinks, and it is empty: fix the code"

if [ "$fail" -eq 0 ]; then
  echo "[lint] architecture OK (${#files[@]} files, $nfields kernel field names)"
else
  echo "[lint] FAILED"
  exit 1
fi
