#!/bin/bash
# devlogs.sh - debug a device-only bug from this Mac: a RELEASE build on the
# phone with development signing, then the phone's own unified log.
#
#   devlogs.sh <product ship.env> devices                 phones devicectl sees
#   devlogs.sh <product ship.env> install [--dry-run]     Release build, development-signed, onto the phone
#   devlogs.sh <product ship.env> collect [MINUTES] [DIR] print the log collect line to run (needs root)
#   devlogs.sh <product ship.env> show ARCHIVE [PREDICATE-EXTRA]
#                                                         the archive, filtered to the product's subsystem
#
# WHY RELEASE: the simulator's timing hides bugs the phone shows (an insert in
# the gap before the real compact drawer is dropped with no callback), and a
# Debug build changes timing again. Release with DEVELOPMENT signing needs no
# TestFlight upload. The build is not the store build (no store profiles).
#
# WHY collect ONLY PRINTS: `log collect --device-udid` needs root, and an agent
# must not sudo. The owner runs the printed line (in Claude Code: prefix `!`).
# It names /usr/bin/log because zsh has a `log` builtin.
#
# The product is its ship.env (shared/tools/ship/ship.sh): SHIP_XCPROJ,
# SHIP_SCHEME, SHIP_APP, SHIP_KERNEL_DIR, SHIP_IOS_DIR, SHIP_TEAM and
# SHIP_LOG_SUBSYSTEM. DEVLOGS_UDID picks a phone when more than one is
# connected. Outputs land in <repo>/build/devlogs/<SHIP_NAME>.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ENV_FILE="${1:-}"
[ -f "$ENV_FILE" ] || { sed -n 2,9p "$0"; exit 2; }
shift
# shellcheck disable=SC1090
source "$ENV_FILE"
: "${SHIP_NAME:?}" "${SHIP_XCPROJ:?}" "${SHIP_SCHEME:?}" "${SHIP_APP:?}" "${SHIP_TEAM:?}"
: "${SHIP_KERNEL_DIR:?}" "${SHIP_IOS_DIR:?}" "${SHIP_LOG_SUBSYSTEM:?$ENV_FILE sets no SHIP_LOG_SUBSYSTEM}"
OUT="$REPO/build/devlogs/$SHIP_NAME"
DD="$OUT/dd"

udid() {
  [ -n "${DEVLOGS_UDID:-}" ] && { echo "$DEVLOGS_UDID"; return; }
  local j; j="$(mktemp)"
  xcrun devicectl list devices --json-output "$j" >/dev/null 2>&1 || true
  # devicectl only speaks JSON; a throwaway read of one field on the Mac, never a shipped path
  python3 -c "
import json
d=json.load(open('$j')).get('result',{}).get('devices',[])
# a PHONE that is reachable: devicectl lists booted simulators as connected too
ok=[x['hardwareProperties']['udid'] for x in d
    if x['hardwareProperties'].get('reality') == 'physical'
    and x.get('connectionProperties',{}).get('tunnelState') not in (None, 'unavailable')]
print(ok[0] if ok else '')" 2>/dev/null || true
  rm -f "$j"
}
need_phone() {
  U="$(udid)"
  [ -n "$U" ] && return
  if [ "${DRY:-0}" = 1 ]; then U="<udid>"; return; fi
  echo "devlogs: no iPhone found (plug it in, unlock it, trust this Mac; or set DEVLOGS_UDID)" >&2; exit 1
}
run() { if [ "${DRY:-0}" = 1 ]; then printf '  $'; printf ' %q' "$@"; echo; else "$@"; fi; }

case "${1:-}" in
  devices)
    xcrun devicectl list devices ;;
  install)
    DRY=0; [ "${2:-}" = --dry-run ] && DRY=1
    need_phone
    mkdir -p "$OUT"
    run make -C "$REPO/$SHIP_KERNEL_DIR" ios-lib
    # xcodegen blanks some tracked .entitlements; restore the ones that were clean
    CLEAN=()
    while IFS= read -r f; do git -C "$REPO" diff --quiet -- "$f" && CLEAN+=("$f"); done \
      < <(git -C "$REPO" ls-files -- "$SHIP_IOS_DIR/*.entitlements")
    (cd "$REPO/$SHIP_IOS_DIR" && run xcodegen generate -q)
    for f in ${CLEAN[@]+"${CLEAN[@]}"}; do git -C "$REPO" diff --quiet -- "$f" || run git -C "$REPO" checkout -- "$f"; done
    run xcodebuild -project "$REPO/$SHIP_XCPROJ" -scheme "$SHIP_SCHEME" -configuration Release \
      -destination "id=$U" -allowProvisioningUpdates CODE_SIGN_STYLE=Automatic \
      DEVELOPMENT_TEAM="$SHIP_TEAM" CODE_SIGN_IDENTITY="Apple Development" \
      -derivedDataPath "$DD" build
    run xcrun devicectl device install app --device "$U" "$DD/Build/Products/Release-iphoneos/$SHIP_APP"
    echo "installed. Reproduce on the phone, then: $0 $ENV_FILE collect" ;;
  collect)
    DRY=1; need_phone             # a placeholder UDID when no phone is attached
    MIN="${2:-5}"; DIR="${3:-$OUT}"
    mkdir -p "$DIR"
    A="$DIR/$(date +%Y%m%d-%H%M%S).logarchive"
    echo "Run this yourself (root required), right after reproducing:"
    echo
    echo "  sudo /usr/bin/log collect --device-udid $U --last ${MIN}m --output $A"
    echo
    echo "then: $0 $ENV_FILE show $A" ;;
  show)
    A="${2:?show needs a .logarchive}"
    P="subsystem BEGINSWITH \"$SHIP_LOG_SUBSYSTEM\""
    [ -n "${3:-}" ] && P="$P AND ($3)"
    /usr/bin/log show "$A" --predicate "$P" --style compact --info --debug ;;
  *) sed -n 2,9p "$0"; exit 2 ;;
esac
