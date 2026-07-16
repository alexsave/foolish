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
