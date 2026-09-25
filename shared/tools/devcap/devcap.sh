#!/bin/bash
# devcap.sh - film a USB iPhone for the motion tool, one command per step.
#
#   devcap.sh <product.env> devices              the phone as devicectl and AVFoundation see it
#   devcap.sh <product.env> install              a DEBUG build of the product (ruler compiled in) onto the phone
#   devcap.sh <product.env> ruler on|off         the ruler's dev file in the phone's App Group
#   devcap.sh <product.env> film NAME [SECONDS]  record (until ^C, or SECONDS), then track, score and chart
#
# The product is its ship.env (see shared/tools/ship/ship.sh): SHIP_XCPROJ,
# SHIP_SCHEME, SHIP_APP, SHIP_KERNEL_DIR, SHIP_IOS_DIR, SHIP_TEAM and
# DEV_APP_GROUP (the DEBUG build's App Group, where dev files live). DEVCAP_UDID
# picks a phone when more than one is connected. Takes land in
# $DEVCAP_OUT (default ~/devcap)/NAME/{take.mov,take.tbl,score.txt,chart.png}.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
ENV_FILE="${1:-}"
[ -f "$ENV_FILE" ] || { sed -n 2,13p "$0"; exit 2; }
shift
# shellcheck disable=SC1090
source "$ENV_FILE"
XCPROJ="$REPO/${SHIP_XCPROJ:?$ENV_FILE sets no SHIP_XCPROJ}"
SCHEME="${SHIP_SCHEME:?}"
APP="${SHIP_APP:?}"
GROUP="${DEV_APP_GROUP:?$ENV_FILE sets no DEV_APP_GROUP}"
TEAM="${SHIP_TEAM:?}"
OUT="${DEVCAP_OUT:-$HOME/devcap}"
DD="${DEVCAP_DD:-$REPO/build/devcap-dd}"
BIN="$HERE/build/devcap"
MOTION="$REPO/shared/tools/motion"
[ -x "$BIN" ] || make -s -C "$HERE" >/dev/null

udid() {
  [ -n "${DEVCAP_UDID:-}" ] && { echo "$DEVCAP_UDID"; return; }
  local j; j="$(mktemp)"
  xcrun devicectl list devices --json-output "$j" >/dev/null 2>&1 || true
  # devicectl only speaks JSON; this is a throwaway read of one field on the Mac, never a shipped path
  python3 -c "
import json,sys
d=json.load(open('$j'))['result']['devices']
ok=[x['hardwareProperties']['udid'] for x in d if x.get('connectionProperties',{}).get('transportType')=='wired']
print(ok[0] if ok else '')" ; rm -f "$j"
}

need_phone() { U="$(udid)"; [ -n "$U" ] || { echo "devcap: no iPhone on USB (unlock it and trust this Mac)" >&2; exit 1; }; }

case "${1:-}" in
  devices)
    xcrun devicectl list devices 2>/dev/null || true
    "$BIN" list --wait 3 ;;
  install)
    need_phone
    make -C "$REPO/${SHIP_KERNEL_DIR:?}" ios-lib >/dev/null
    (cd "$REPO/${SHIP_IOS_DIR:?}" && xcodegen generate >/dev/null)
    (cd "$REPO" && git checkout -- $(git ls-files -- '*.entitlements'))   # xcodegen blanks them
    xcodebuild -project "$XCPROJ" -scheme "$SCHEME" -configuration Debug -destination "id=$U" \
      -allowProvisioningUpdates CODE_SIGN_STYLE=Automatic DEVELOPMENT_TEAM="$TEAM" \
      CODE_SIGN_IDENTITY="Apple Development" -derivedDataPath "$DD" build | tail -3
    xcrun devicectl device install app --device "$U" "$DD/Build/Products/Debug-iphoneos/$APP" ;;
  ruler)
    need_phone
    f="$(mktemp -d)/dev.ruler"; : > "$f"
    if [ "${2:-on}" = on ]; then
      xcrun devicectl device copy to --device "$U" --domain-type appGroupDataContainer \
        --domain-identifier "$GROUP" --source "$f" --destination dev.ruler
    else
      xcrun devicectl device file delete --device "$U" --domain-type appGroupDataContainer \
        --domain-identifier "$GROUP" dev.ruler 2>/dev/null \
        || echo "devcap: remove dev.ruler by reinstalling, or leave it (Release builds never read it)"
    fi ;;
  film)
    NAME="${2:?name}"; SECS="${3:-0}"
    d="$OUT/$NAME"; mkdir -p "$d"
    if [ "$SECS" = 0 ]; then "$BIN" record "$d/take.mov"; else "$BIN" record "$d/take.mov" --seconds "$SECS"; fi
    bash "$MOTION/motion_take.sh" "$d/take.mov" "$d/take.tbl"
    # the drawer's bottom is the screen's: its green bar where the take starts
    B="$(awk '!/^#/ && $4 != "-" && NR > 2 {print $4; exit}' "$d/take.tbl")"
    "$MOTION/build/motion" score --span 1.5 --bottom first "$d/take.tbl" | tee "$d/score.txt"
    python3 "$REPO/shared/rig/lib/motionplot.py" "$d/take.tbl" "$d/chart.png" --span 1.5 ${B:+--bottom $B} --title "$NAME"
    # with no ruler in the build (TestFlight), the board's own grid lines
    bash "$MOTION/motion_grid.sh" "$d/take.mov" "$d/take.grid" || true ;;
  *) sed -n 2,13p "$0"; exit 2 ;;
esac
