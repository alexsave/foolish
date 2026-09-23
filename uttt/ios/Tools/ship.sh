#!/bin/bash
# Ship the UTTT iMessage app to TestFlight: kernel library, project, archive,
# signed .ipa, upload, and wait until App Store Connect says VALID.
#
#   uttt/ios/Tools/ship.sh [--build N] [--no-upload]
#
#   --build N     the CFBundleVersion to ship. Default: one above the highest
#                 build App Store Connect has for the app. A number can be
#                 uploaded ONCE, ever, even if its processing fails.
#   --no-upload   stop after the export and the .ipa checks.
#
# Credentials come from the environment, never from this file:
#   ASC_KEY_ID, ASC_ISSUER_ID     App Store Connect API key; the .p8 must be at
#                                 ~/.appstoreconnect/private_keys/AuthKey_<id>.p8
#                                 (where altool looks for it)
#   SIGNING_KEYCHAIN_PASSWORD     the keychain holding the Apple Distribution key
#   SIGNING_KEYCHAIN              default ~/Library/Keychains/foolish-signing.keychain-db
#   ASC_PY                        API client, default ~/.appstoreconnect/asc.py
#                                 (python3 + openssl only; exposes call(method, path, body))
#
# Everything it writes lands in <repo>/build/ship (gitignored), including its own
# DerivedData, so it never collides with a development build in the same tree.
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
# - The scheme is UtttMessagesApp. The archive's bundle id is checked before
#   exporting, because a wrong product fails export with capability errors
#   that read like a profile problem.
# - Release asks for no App Group (see project.yml); the store profiles have
#   none. The signed .ipa is checked for it so a regression fails HERE, not
#   in Apple's processing half an hour later.
set -euo pipefail

APP_ID=6815039449
BUNDLE=cards.uttt.msg
EXT_BUNDLE=cards.uttt.msg.MessagesExtension
TEAM=8N2Z544SB4
APP_PROFILE="Uttt Msg Store 1"
EXT_PROFILE="Uttt MsgExt Store 1"
SCHEME=UtttMessagesApp

BUILD="" UPLOAD=1
while [ $# -gt 0 ]; do
  case "$1" in
    --build) BUILD="$2"; shift 2 ;;
    --no-upload) UPLOAD=0; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

: "${ASC_KEY_ID:?set ASC_KEY_ID}"
: "${ASC_ISSUER_ID:?set ASC_ISSUER_ID}"
: "${SIGNING_KEYCHAIN_PASSWORD:?set SIGNING_KEYCHAIN_PASSWORD}"
KEYCHAIN="${SIGNING_KEYCHAIN:-$HOME/Library/Keychains/foolish-signing.keychain-db}"
ASC_PY="${ASC_PY:-$HOME/.appstoreconnect/asc.py}"
KEY_PATH="$HOME/.appstoreconnect/private_keys/AuthKey_${ASC_KEY_ID}.p8"
[ -f "$KEY_PATH" ] || { echo "no API key at $KEY_PATH" >&2; exit 2; }
[ -f "$ASC_PY" ] || { echo "no API client at $ASC_PY" >&2; exit 2; }
export ASC_KEY_ID ASC_ISSUER="$ASC_ISSUER_ID"   # asc.py reads ASC_ISSUER

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
IOS="$ROOT/uttt/ios"
OUT="$ROOT/build/ship"
DD="$OUT/dd"
ARCH="$OUT/Uttt.xcarchive"
EXP="$OUT/export"
PROFILES_DIR="$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"
mkdir -p "$OUT" "$PROFILES_DIR"

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

# ---- build number -----------------------------------------------------------
if [ -z "$BUILD" ]; then
  BUILD=$(asc '
st, js = call("GET", "/v1/builds?filter[app]='"$APP_ID"'&fields[builds]=version&limit=200")
assert st == 200, (st, js)
print(max([int(b["attributes"]["version"]) for b in js["data"]] or [0]) + 1)')
fi
[[ "$BUILD" =~ ^[0-9]+$ ]] || { echo "build number must be an integer, got $BUILD" >&2; exit 2; }
VERSION=$(sed -n 's/^ *MARKETING_VERSION: "\(.*\)"/\1/p' "$IOS/project.yml")
echo "shipping $BUNDLE $VERSION($BUILD)"

# ---- kernel library + project -----------------------------------------------
step "kernel xcframework"
make -C "$ROOT/uttt/c" ios-lib > "$OUT/ios-lib.log" 2>&1 \
  || { tail -20 "$OUT/ios-lib.log" >&2; exit 1; }
step "xcodegen"
(cd "$IOS" && xcodegen generate -q)

# ---- store profiles, fresh from the API -------------------------------------
step "store profiles"
asc '
import base64, os, plistlib, subprocess
want = {"'"$APP_PROFILE"'": "'"$BUNDLE"'", "'"$EXT_PROFILE"'": "'"$EXT_BUNDLE"'"}
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
    assert ent["application-identifier"] == "'"$TEAM"'." + bundle, ent["application-identifier"]
    groups = ent.get("com.apple.security.application-groups")
    print("  %s  %s  expires %s  app-groups=%s"
          % (name, a["uuid"], a["expirationDate"][:10], groups))
'

# ---- archive (automatic signing) --------------------------------------------
step "archive (log: $ARCH.log)"
rm -rf "$ARCH"
xcodebuild archive -project "$IOS/Uttt.xcodeproj" -scheme "$SCHEME" \
  -destination 'generic/platform=iOS' -archivePath "$ARCH" -derivedDataPath "$DD" \
  -allowProvisioningUpdates \
  -authenticationKeyPath "$KEY_PATH" -authenticationKeyID "$ASC_KEY_ID" \
  -authenticationKeyIssuerID "$ASC_ISSUER_ID" \
  CODE_SIGN_STYLE=Automatic DEVELOPMENT_TEAM="$TEAM" \
  CURRENT_PROJECT_VERSION="$BUILD" > "$ARCH.log" 2>&1 \
  || { grep -E "error:" "$ARCH.log" | head -20 >&2; tail -5 "$ARCH.log" >&2; exit 1; }

pb() { /usr/libexec/PlistBuddy -c "Print :$1" "$2"; }
ID=$(pb ApplicationProperties:CFBundleIdentifier "$ARCH/Info.plist")
AV=$(pb ApplicationProperties:CFBundleShortVersionString "$ARCH/Info.plist")
AB=$(pb ApplicationProperties:CFBundleVersion "$ARCH/Info.plist")
[ "$ID" = "$BUNDLE" ] || { echo "WRONG PRODUCT: archive is $ID, expected $BUNDLE" >&2; exit 1; }
[ "$AV" = "$VERSION" ] && [ "$AB" = "$BUILD" ] \
  || { echo "archive says $AV($AB), expected $VERSION($BUILD)" >&2; exit 1; }
if ls "$ARCH/Products/Applications" | grep -qv '^UtttMessagesApp.app$'; then
  echo "archive carries more than the container app:" >&2; ls "$ARCH/Products/Applications" >&2; exit 1
fi
echo "archive ok: $ID $AV($AB)"

# ---- export (manual signing, local only) ------------------------------------
step "export (log: $EXP.log)"
OPTS="$OUT/ExportOptions.plist"
cat > "$OPTS" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>method</key><string>app-store-connect</string>
  <key>teamID</key><string>$TEAM</string>
  <key>signingStyle</key><string>manual</string>
  <key>signingCertificate</key><string>Apple Distribution</string>
  <key>provisioningProfiles</key>
  <dict>
    <key>$BUNDLE</key><string>$APP_PROFILE</string>
    <key>$EXT_BUNDLE</key><string>$EXT_PROFILE</string>
  </dict>
  <key>uploadSymbols</key><true/>
  <key>manageAppVersionAndBuildNumber</key><false/>
</dict>
</plist>
EOF
security unlock-keychain -p "$SIGNING_KEYCHAIN_PASSWORD" "$KEYCHAIN"
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$SIGNING_KEYCHAIN_PASSWORD" "$KEYCHAIN" > /dev/null
rm -rf "$EXP"
xcodebuild -exportArchive -archivePath "$ARCH" -exportOptionsPlist "$OPTS" -exportPath "$EXP" \
  > "$EXP.log" 2>&1 || {
  grep -q errSecInternalComponent "$EXP.log" && echo "errSecInternalComponent: the signing keychain is locked or codesign is not in its partition list" >&2
  grep -E "error" "$EXP.log" | head -20 >&2; exit 1; }
IPA=$(ls "$EXP"/*.ipa | head -1)

# ---- what is actually in the .ipa -------------------------------------------
step "ipa checks"
UNZ="$OUT/ipa"; rm -rf "$UNZ"; mkdir -p "$UNZ"; unzip -q "$IPA" -d "$UNZ"
APPB="$UNZ/Payload/UtttMessagesApp.app"
APPEX="$APPB/PlugIns/UtttMessages.appex"
[ "$(pb ITSAppUsesNonExemptEncryption "$APPB/Info.plist")" = "false" ] \
  || { echo "container Info.plist lacks ITSAppUsesNonExemptEncryption=NO" >&2; exit 1; }
for b in "$APPB" "$APPEX"; do
  # Captured, then grepped: `codesign | grep -q` under pipefail fails on the
  # SIGPIPE codesign takes when grep stops reading early.
  ENT=$(codesign -d --entitlements - --xml "$b" 2>/dev/null | plutil -convert xml1 -o - -)
  SIG=$(codesign -dvv "$b" 2>&1)
  grep -q "<string>$TEAM\.cards\.uttt\.msg" <<<"$ENT" || { echo "unexpected signature on $b" >&2; exit 1; }
  if grep -q "application-groups" <<<"$ENT"; then
    echo "$(basename "$b") is signed WITH an app group - Release must not ask for one (project.yml)" >&2; exit 1
  fi
  grep -q "Authority=Apple Distribution" <<<"$SIG" || { echo "$b is not signed with Apple Distribution" >&2; exit 1; }
done
echo "ipa ok: $IPA ($(du -h "$IPA" | cut -f1))"

[ "$UPLOAD" = 1 ] || { echo "--no-upload: stopping before the upload"; exit 0; }

# ---- upload -----------------------------------------------------------------
step "upload $VERSION($BUILD)"
UP=$(xcrun altool --upload-app -f "$IPA" -t ios --apiKey "$ASC_KEY_ID" --apiIssuer "$ASC_ISSUER_ID" 2>&1) || true
echo "$UP" | tail -5
echo "$UP" | grep -q "UPLOAD SUCCEEDED" || { echo "UPLOAD FAILED" >&2; exit 1; }

# ---- wait for processing (bounded: 45 minutes) ------------------------------
step "waiting for App Store Connect to process $VERSION($BUILD)"
for i in $(seq 1 45); do
  STATE=$(asc '
st, js = call("GET", "/v1/builds?filter[app]='"$APP_ID"'&filter[version]='"$BUILD"'&fields[builds]=version,processingState")
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
