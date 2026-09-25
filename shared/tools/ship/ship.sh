#!/bin/bash
# Ship an iMessage app to TestFlight: kernel library, project, archive, signed
# .ipa, release checks, upload, and wait until App Store Connect says VALID.
#
#   shared/tools/ship/ship.sh <product ship.env> [--build N] [--no-upload] [--dry-run]
#
#   --build N     the CFBundleVersion to ship. Default: one above the highest
#                 build App Store Connect has for the app. A number can be
#                 uploaded ONCE, ever, even if its processing fails.
#   --no-upload   stop after the export and the .ipa checks (release strings included).
#   --dry-run     print the resolved product and every command that would run,
#                 touch nothing. Needs no credentials when --build is given.
#
# The product is ONE env file (the way the rig and testflight.py pick one). It
# sets, with paths relative to the repo root:
#   SHIP_NAME          short name; outputs land in build/ship/<SHIP_NAME>
#   SHIP_APP_ID        App Store Connect app id
#   SHIP_BUNDLE        container bundle id
#   SHIP_EXT_BUNDLE    the Messages extension's bundle id
#   SHIP_TEAM          team id
#   SHIP_APP_PROFILE   store profile name for SHIP_BUNDLE
#   SHIP_EXT_PROFILE   store profile name for SHIP_EXT_BUNDLE
#   SHIP_SCHEME        the scheme to archive (the container, never a host app)
#   SHIP_XCPROJ        the Xcode project
#   SHIP_KERNEL_DIR    where `make ios-lib` builds the kernel xcframework
#   SHIP_IOS_DIR       where `xcodegen generate` runs
#   SHIP_VERSION_FILE  the file holding `MARKETING_VERSION: "x.y"` (first match wins)
#   SHIP_APP           the container bundle's file name inside the archive (X.app)
#   SHIP_APPEX         the extension's file name inside it (Y.appex)
#   SHIP_APP_GROUP     the App Group the store build is signed with; empty
#                      means the store build must carry NONE
#   SHIP_FORBID_FRAMEWORKS  space-separated frameworks the .ipa must not carry
#
# Credentials come from the environment, never from a file in the repo:
#   ASC_KEY_ID, ASC_ISSUER_ID     App Store Connect API key; the .p8 must be at
#                                 ~/.appstoreconnect/private_keys/AuthKey_<id>.p8
#                                 (where altool looks for it)
#   SIGNING_KEYCHAIN_PASSWORD     the keychain holding the Apple Distribution key
#   SIGNING_KEYCHAIN              the keychain file itself (the product env file may default it)
#   ASC_PY                        API client, default ~/.appstoreconnect/asc.py
#                                 (python3 + openssl only; exposes call(method, path, body))
#
# The build number goes on the archive command line, which overrides every
# target's CURRENT_PROJECT_VERSION at once - so a project that spells it in more
# than one place (a watch target, say) cannot ship a mismatched pair.
#
# THE TRAPS, each one met for real:
# - Archive AUTO-signs, export signs MANUALLY with the store profiles. Xcode's
#   automatic store profile is bound to a certificate this Mac has no key for,
#   so an automatic export fails with "doesn't include signing certificate".
# - The store profiles are fetched from the API and installed on every run, so
#   a regenerated profile is picked up without anyone opening Xcode.
# - errSecInternalComponent from codesign = the distribution key's keychain is
#   locked or codesign is not in its partition list. Both are set before export.
# - Export does NOT upload (no `destination: upload`): that path goes through
#   Xcode's account session, which goes stale and fails "Failed to Use
#   Accounts". altool with the API key needs no session.
# - The archive's bundle id is checked before exporting, because a wrong
#   product (a host app scheme) fails export with capability errors that read
#   like a profile problem.
# - xcodegen blanks some tracked .entitlements files. Every one that was clean
#   before it ran is restored after, so a ship never leaves a dirty tree.
# - The signed .ipa's App Group is checked against SHIP_APP_GROUP so a
#   regression fails HERE, not in Apple's processing half an hour later.
set -euo pipefail

ENV_FILE="${1:?usage: ship.sh <product ship.env> [--build N] [--no-upload] [--dry-run]}"; shift
[ -f "$ENV_FILE" ] || { echo "no product env file at $ENV_FILE" >&2; exit 2; }
# shellcheck disable=SC1090
source "$ENV_FILE"
for v in SHIP_NAME SHIP_APP_ID SHIP_BUNDLE SHIP_EXT_BUNDLE SHIP_TEAM SHIP_APP_PROFILE \
         SHIP_EXT_PROFILE SHIP_SCHEME SHIP_XCPROJ SHIP_KERNEL_DIR SHIP_IOS_DIR \
         SHIP_VERSION_FILE SHIP_APP SHIP_APPEX; do
  [ -n "${!v:-}" ] || { echo "$ENV_FILE does not set $v" >&2; exit 2; }
done
SHIP_APP_GROUP="${SHIP_APP_GROUP:-}"
SHIP_FORBID_FRAMEWORKS="${SHIP_FORBID_FRAMEWORKS:-}"

BUILD="" UPLOAD=1 DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --build) BUILD="$2"; shift 2 ;;
    --no-upload) UPLOAD=0; shift ;;
    --dry-run) DRY=1; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
IOS="$ROOT/$SHIP_IOS_DIR"
XCPROJ="$ROOT/$SHIP_XCPROJ"
OUT="$ROOT/build/ship/$SHIP_NAME"
DD="$OUT/dd"
ARCH="$OUT/$SHIP_NAME.xcarchive"
EXP="$OUT/export"
KEYCHAIN="${SIGNING_KEYCHAIN:?set SIGNING_KEYCHAIN (the product env file usually does)}"
ASC_PY="${ASC_PY:-$HOME/.appstoreconnect/asc.py}"
PROFILES_DIR="$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"

if [ "$DRY" = 0 ] || [ -z "$BUILD" ]; then
  : "${ASC_KEY_ID:?set ASC_KEY_ID}"
  : "${ASC_ISSUER_ID:?set ASC_ISSUER_ID}"
  KEY_PATH="$HOME/.appstoreconnect/private_keys/AuthKey_${ASC_KEY_ID}.p8"
  [ -f "$KEY_PATH" ] || { echo "no API key at $KEY_PATH" >&2; exit 2; }
  [ -f "$ASC_PY" ] || { echo "no API client at $ASC_PY" >&2; exit 2; }
  export ASC_KEY_ID ASC_ISSUER="$ASC_ISSUER_ID"   # asc.py reads ASC_ISSUER
fi
[ "$DRY" = 1 ] || : "${SIGNING_KEYCHAIN_PASSWORD:?set SIGNING_KEYCHAIN_PASSWORD}"
ASC_KEY_ID="${ASC_KEY_ID:-<ASC_KEY_ID>}" ASC_ISSUER_ID="${ASC_ISSUER_ID:-<ASC_ISSUER_ID>}"
KEY_PATH="${KEY_PATH:-$HOME/.appstoreconnect/private_keys/AuthKey_${ASC_KEY_ID}.p8}"

asc() {  # asc <python expression over call()> - prints what the expression returns
  python3 - "$ASC_PY" "$@" <<'PY'
import sys, importlib.util
spec = importlib.util.spec_from_file_location("asc", sys.argv[1])
asc = importlib.util.module_from_spec(spec); spec.loader.exec_module(asc)
call = asc.call
exec(sys.argv[2])
PY
}

step() { echo; echo "== $*"; }
# run CMD... - runs it, or under --dry-run prints it, shell-quoted
run() {
  if [ "$DRY" = 1 ]; then printf '  $'; printf ' %q' "$@"; echo; else "$@"; fi
}

# ---- build number -----------------------------------------------------------
if [ -z "$BUILD" ]; then
  BUILD=$(asc '
st, js = call("GET", "/v1/builds?filter[app]='"$SHIP_APP_ID"'&fields[builds]=version&limit=200")
assert st == 200, (st, js)
print(max([int(b["attributes"]["version"]) for b in js["data"]] or [0]) + 1)')
fi
[[ "$BUILD" =~ ^[0-9]+$ ]] || { echo "build number must be an integer, got $BUILD" >&2; exit 2; }
VERSION=$(sed -n 's/^ *MARKETING_VERSION: "\(.*\)"/\1/p' "$ROOT/$SHIP_VERSION_FILE" | head -1)
[ -n "$VERSION" ] || { echo "no MARKETING_VERSION in $SHIP_VERSION_FILE" >&2; exit 2; }
echo "shipping $SHIP_BUNDLE $VERSION($BUILD)  [$SHIP_SCHEME, app group: ${SHIP_APP_GROUP:-none}]"
[ "$DRY" = 1 ] && echo "--dry-run: printing commands, running none"
mkdir -p "$OUT" "$PROFILES_DIR"

# ---- kernel library + project -----------------------------------------------
step "kernel xcframework"
if [ "$DRY" = 1 ]; then run make -C "$ROOT/$SHIP_KERNEL_DIR" ios-lib
else
  make -C "$ROOT/$SHIP_KERNEL_DIR" ios-lib > "$OUT/ios-lib.log" 2>&1 \
    || { tail -20 "$OUT/ios-lib.log" >&2; exit 1; }
fi
step "xcodegen"
CLEAN_ENTS=()
while IFS= read -r f; do
  git -C "$ROOT" diff --quiet -- "$f" && CLEAN_ENTS+=("$f")
done < <(git -C "$ROOT" ls-files -- "$SHIP_IOS_DIR/*.entitlements")
(cd "$IOS" && run xcodegen generate -q)
if [ "${#CLEAN_ENTS[@]}" -gt 0 ]; then
  for f in "${CLEAN_ENTS[@]}"; do
    git -C "$ROOT" diff --quiet -- "$f" || run git -C "$ROOT" checkout -- "$f"
  done
fi

# ---- store profiles, fresh from the API -------------------------------------
step "store profiles"
if [ "$DRY" = 1 ]; then
  echo "  fetch + install \"$SHIP_APP_PROFILE\" ($SHIP_BUNDLE), \"$SHIP_EXT_PROFILE\" ($SHIP_EXT_BUNDLE)"
else
asc '
import base64, os, plistlib, subprocess
want = {"'"$SHIP_APP_PROFILE"'": "'"$SHIP_BUNDLE"'", "'"$SHIP_EXT_PROFILE"'": "'"$SHIP_EXT_BUNDLE"'"}
st, js = call("GET", "/v1/profiles?filter[name]=" + ",".join(want).replace(" ", "%20") + "&limit=10")
assert st == 200, (st, js)
got = {p["attributes"]["name"]: p["attributes"] for p in js["data"]}
for name, bundle in want.items():
    a = got.get(name)
    assert a and a["profileState"] == "ACTIVE", f"profile {name!r} missing or not ACTIVE"
    raw = base64.b64decode(a["profileContent"])
    path = os.path.join("'"$PROFILES_DIR"'", a["uuid"] + ".mobileprovision")
    open(path, "wb").write(raw)
    ent = plistlib.loads(subprocess.run(["security", "cms", "-D"], input=raw,
                         capture_output=True, check=True).stdout)["Entitlements"]
    assert ent["application-identifier"] == "'"$SHIP_TEAM"'." + bundle, ent["application-identifier"]
    groups = ent.get("com.apple.security.application-groups")
    print("  %s  %s  expires %s  app-groups=%s"
          % (name, a["uuid"], a["expirationDate"][:10], groups))
'
fi

# ---- archive (automatic signing) --------------------------------------------
step "archive (log: $ARCH.log)"
[ "$DRY" = 1 ] || rm -rf "$ARCH"
ARCHIVE_CMD=(xcodebuild archive -project "$XCPROJ" -scheme "$SHIP_SCHEME"
  -destination 'generic/platform=iOS' -archivePath "$ARCH" -derivedDataPath "$DD"
  -allowProvisioningUpdates
  -authenticationKeyPath "$KEY_PATH" -authenticationKeyID "$ASC_KEY_ID"
  -authenticationKeyIssuerID "$ASC_ISSUER_ID"
  CODE_SIGN_STYLE=Automatic DEVELOPMENT_TEAM="$SHIP_TEAM"
  CURRENT_PROJECT_VERSION="$BUILD")
if [ "$DRY" = 1 ]; then run "${ARCHIVE_CMD[@]}"
else
  "${ARCHIVE_CMD[@]}" > "$ARCH.log" 2>&1 \
    || { grep -E "error:" "$ARCH.log" | head -20 >&2; tail -5 "$ARCH.log" >&2; exit 1; }

  pb() { /usr/libexec/PlistBuddy -c "Print :$1" "$2"; }
  ID=$(pb ApplicationProperties:CFBundleIdentifier "$ARCH/Info.plist")
  AV=$(pb ApplicationProperties:CFBundleShortVersionString "$ARCH/Info.plist")
  AB=$(pb ApplicationProperties:CFBundleVersion "$ARCH/Info.plist")
  [ "$ID" = "$SHIP_BUNDLE" ] || { echo "WRONG PRODUCT: archive is $ID, expected $SHIP_BUNDLE" >&2; exit 1; }
  [ "$AV" = "$VERSION" ] && [ "$AB" = "$BUILD" ] \
    || { echo "archive says $AV($AB), expected $VERSION($BUILD)" >&2; exit 1; }
  if ls "$ARCH/Products/Applications" | grep -qvx "$SHIP_APP"; then
    echo "archive carries more than the container app:" >&2; ls "$ARCH/Products/Applications" >&2; exit 1
  fi
  echo "archive ok: $ID $AV($AB)"
fi

# ---- export (manual signing, local only) ------------------------------------
step "export (log: $EXP.log)"
OPTS="$OUT/ExportOptions.plist"
cat > "$OPTS" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>method</key><string>app-store-connect</string>
  <key>teamID</key><string>$SHIP_TEAM</string>
  <key>signingStyle</key><string>manual</string>
  <key>signingCertificate</key><string>Apple Distribution</string>
  <key>provisioningProfiles</key>
  <dict>
    <key>$SHIP_BUNDLE</key><string>$SHIP_APP_PROFILE</string>
    <key>$SHIP_EXT_BUNDLE</key><string>$SHIP_EXT_PROFILE</string>
  </dict>
  <key>uploadSymbols</key><true/>
  <key>manageAppVersionAndBuildNumber</key><false/>
</dict>
</plist>
EOF
[ "$DRY" = 1 ] && echo "  export options: $OPTS"
if [ "$DRY" = 1 ]; then
  run security unlock-keychain -p '<SIGNING_KEYCHAIN_PASSWORD>' "$KEYCHAIN"
  run xcodebuild -exportArchive -archivePath "$ARCH" -exportOptionsPlist "$OPTS" -exportPath "$EXP"
else
  security unlock-keychain -p "$SIGNING_KEYCHAIN_PASSWORD" "$KEYCHAIN"
  security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$SIGNING_KEYCHAIN_PASSWORD" "$KEYCHAIN" > /dev/null
  rm -rf "$EXP"
  xcodebuild -exportArchive -archivePath "$ARCH" -exportOptionsPlist "$OPTS" -exportPath "$EXP" \
    > "$EXP.log" 2>&1 || {
    grep -q errSecInternalComponent "$EXP.log" && echo "errSecInternalComponent: the signing keychain is locked or codesign is not in its partition list" >&2
    grep -E "error" "$EXP.log" | head -20 >&2; exit 1; }
fi
IPA=$(ls "$EXP"/*.ipa 2>/dev/null | head -1 || true)

# ---- what is actually in the .ipa -------------------------------------------
step "ipa checks"
if [ "$DRY" = 1 ]; then
  echo "  Info.plist ITSAppUsesNonExemptEncryption=NO; $SHIP_TEAM.$SHIP_BUNDLE* signatures by Apple Distribution;"
  echo "  app group ${SHIP_APP_GROUP:-none}"
else
  pb() { /usr/libexec/PlistBuddy -c "Print :$1" "$2"; }
  UNZ="$OUT/ipa"; rm -rf "$UNZ"; mkdir -p "$UNZ"; unzip -q "$IPA" -d "$UNZ"
  APPB="$UNZ/Payload/$SHIP_APP"
  APPEX="$APPB/PlugIns/$SHIP_APPEX"
  [ -d "$APPEX" ] || { echo "no $SHIP_APPEX inside $SHIP_APP" >&2; exit 1; }
  [ "$(pb ITSAppUsesNonExemptEncryption "$APPB/Info.plist")" = "false" ] \
    || { echo "container Info.plist lacks ITSAppUsesNonExemptEncryption=NO" >&2; exit 1; }
  for b in "$APPB" "$APPEX"; do
    # Captured, then grepped: `codesign | grep -q` under pipefail fails on the
    # SIGPIPE codesign takes when grep stops reading early.
    ENT=$(codesign -d --entitlements - --xml "$b" 2>/dev/null | plutil -convert xml1 -o - -)
    SIG=$(codesign -dvv "$b" 2>&1)
    grep -qF "<string>$SHIP_TEAM.$SHIP_BUNDLE" <<<"$ENT" || { echo "unexpected signature on $b" >&2; exit 1; }
    GROUPS_NOW=$(python3 -c 'import plistlib,sys; print(" ".join(plistlib.loads(sys.stdin.buffer.read()).get("com.apple.security.application-groups", [])))' <<<"$ENT")
    [ "$GROUPS_NOW" = "$SHIP_APP_GROUP" ] || {
      echo "$(basename "$b") is signed with app groups [${GROUPS_NOW:-none}], expected [${SHIP_APP_GROUP:-none}]" >&2; exit 1; }
    grep -q "Authority=Apple Distribution" <<<"$SIG" || { echo "$b is not signed with Apple Distribution" >&2; exit 1; }
  done
  echo "ipa ok: $IPA ($(du -h "$IPA" | cut -f1))"
fi

# ---- release strings: no dev files, no em dashes, no forbidden frameworks ---
step "release strings"
RS_ARGS=()
for fw in $SHIP_FORBID_FRAMEWORKS; do RS_ARGS+=(--forbid-framework "$fw"); done
if [ "$DRY" = 1 ]; then run "$ROOT/shared/tools/release_strings.sh" "$EXP/<name>.ipa" ${RS_ARGS[@]+"${RS_ARGS[@]}"}
else "$ROOT/shared/tools/release_strings.sh" "$IPA" ${RS_ARGS[@]+"${RS_ARGS[@]}"} || exit 1
fi

if [ "$UPLOAD" = 0 ] || [ "$DRY" = 1 ]; then
  [ "$DRY" = 1 ] && run xcrun altool --upload-app -f "$EXP/<name>.ipa" -t ios --apiKey "$ASC_KEY_ID" --apiIssuer "$ASC_ISSUER_ID"
  echo "stopping before the upload"; exit 0
fi

# ---- upload -----------------------------------------------------------------
step "upload $VERSION($BUILD)"
UP=$(xcrun altool --upload-app -f "$IPA" -t ios --apiKey "$ASC_KEY_ID" --apiIssuer "$ASC_ISSUER_ID" 2>&1) || true
echo "$UP" | tail -5
echo "$UP" | grep -q "UPLOAD SUCCEEDED" || { echo "UPLOAD FAILED" >&2; exit 1; }

# ---- wait for processing (bounded: 45 minutes) ------------------------------
step "waiting for App Store Connect to process $VERSION($BUILD)"
for i in $(seq 1 45); do
  STATE=$(asc '
st, js = call("GET", "/v1/builds?filter[app]='"$SHIP_APP_ID"'&filter[version]='"$BUILD"'&fields[builds]=version,processingState")
d = js.get("data") if st == 200 else None
print(d[0]["attributes"]["processingState"] + " " + d[0]["id"] if d else "NOT_LISTED")')
  echo "  $(date +%H:%M) $STATE"
  case "$STATE" in
    VALID*) echo "build ${STATE#VALID } is VALID"; exit 0 ;;
    INVALID*|FAILED*) echo "processing failed - check the email App Store Connect sent" >&2; exit 1 ;;
  esac
  sleep 60
done
echo "still not VALID after 45 minutes; re-check later, the upload itself succeeded" >&2
exit 1
