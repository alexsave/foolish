#!/usr/bin/env bash
# lint_architecture.sh — enforces the module rules from IOS_APP_DESIGN.md §4 and
# §16.E1 with plain greps (the doc: "a plain grep on imports is fine"). Runs in
# CI and locally; exits non-zero on any violation. Portable — no Xcode needed.
set -euo pipefail
cd "$(dirname "$0")/.."   # ios/

fail=0
note() { echo "  ✗ $1"; fail=1; }

# Drop matches that land on comment lines (`  file:line:   // ...`) so the
# rules can discuss what they forbid without tripping themselves.
strip_comments() { grep -vE ':[0-9]+:[[:space:]]*//'; }

echo "[lint] no StoreKit anywhere in v1 (§10.5, §16.E1)…"
# v1 ships zero billing surface — StoreKit must not be imported ANYWHERE, not
# even in Entitlements/. The future StoreKitEntitlements adds it in Milestone G.
# Anchor to a real import statement (comment lines start with //, so excluded).
if grep -rnE --include='*.swift' '^[[:space:]]*import[[:space:]]+StoreKit' . | strip_comments | grep .; then
  note "found 'import StoreKit' — forbidden until the Oracle milestone"
fi

echo "[lint] no price strings / product IDs / 'coming soon' teasers (§10.5)…"
if grep -rniE --include='*.swift' '(\$[0-9]+\.[0-9]{2}|com\.foolish\..*\.(monthly|yearly|premium)|coming soon)' . | strip_comments | grep .; then
  note "found a billing teaser / price / product id — none allowed in v1"
fi

echo "[lint] the iMessage extension never imports Net (§4: serverless by design)…"
# FoolishMessages depends on FoolishKit's Engine/DesignSystem/Boards only.
if [ -d FoolishMessages ]; then
  if grep -rn --include='*.swift' 'import.*Net\|FoolishKit.Net' FoolishMessages; then
    note "FoolishMessages must not reach the Net layer"
  fi
fi

echo "[lint] the extension's frameworks carry no network stack…"
# Source greps above catch `import FoolishNet`; this catches the LINK, which is
# what actually decides whether the appex binary carries the Supabase SDK. The
# whole standalone-iMessage story depends on it (§17.5 memory ceiling, and a
# no-account extension staying clear of the host app's compliance surface), and
# a stray dependency line in project.yml would undo it silently — nothing on
# Linux CI compiles Swift, so this spec check is the only guard.
if [ -f project.yml ]; then
  python3 - <<'PY' || fail=1
import re, sys
spec = open('project.yml').read()
body = spec.split('\ntargets:', 1)[1] if '\ntargets:' in spec else ''
# Top-level target blocks are indented exactly two spaces.
blocks = dict(re.findall(r'\n  ([A-Za-z]\w*):\n(.*?)(?=\n  [A-Za-z]\w*:\n|\Z)', body, re.S))
bad = []
for name in ('FoolishKit', 'FoolishMessages', 'FoolishMessagesApp'):
    b = blocks.get(name)
    if b is None:
        bad.append(f'{name}: target missing from project.yml')
        continue
    if 'package: Supabase' in b:
        bad.append(f'{name}: links the Supabase package')
    if 'target: FoolishNet' in b:
        bad.append(f'{name}: depends on FoolishNet')
for line in bad:
    print(f'  {line}')
sys.exit(1 if bad else 0)
PY
  if [ "${fail:-0}" -ne 0 ]; then
    note "the iMessage extension must not link a network stack (see above)"
  fi
fi

echo "[lint] the C bridge stays inside the Swift SDK (sdk/swift/, §7.1)…"
# CFoolish (the fio_* kernel API) may be imported ONLY inside the Swift SDK —
# sdk/swift/ (A10). Never in the app layers: FoolishKit/{DesignSystem,Net,
# Boards}, FoolishApp, or FoolishMessages. This supersedes the older
# "only EngineC" wording, which the tree already outgrew (MessageEnvelope,
# the URL/message codec, legitimately calls the C base32/seal path too).
strays=$(grep -rlE '^[[:space:]]*import[[:space:]]+CFoolish' --include='*.swift' ../ios ../sdk/swift 2>/dev/null \
  | grep -v '/build/' | grep -vE '\.\./sdk/swift/' || true)
if [ -n "$strays" ]; then
  note "import CFoolish must stay inside sdk/swift/; found in the app layer:\n$strays"
fi

if [ "$fail" -eq 0 ]; then
  echo "[lint] architecture OK"
else
  echo "[lint] FAILED"
  exit 1
fi
