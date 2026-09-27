#!/bin/bash
# Draw every icon the app ships, each at ITS OWN size.
#
# NOT resampled from one big one. The pen's grain and its minimum stroke are
# in points, so a 1024-wide drawing shrunk to 54 loses the ink and a 54-wide
# one blown up loses the paper. Each line below is the same hand drawing the
# same thing on a smaller piece of paper.
#
#   make -C uttt/c icons
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=./build/uttt_icon
IOS=../ios

# px_w px_h scale outfile ... scale is Apple's @2x/@3x, and the icon is drawn
# at px/scale POINTS so the ink is the right weight for the device.
draw() {
  local w=$1 h=$2 s=$3 out=$4
  mkdir -p "$(dirname "$out")"
  "$BIN" "$w" "$h" "$s" > /tmp/uttt_icon.ppm
  sips -s format png /tmp/uttt_icon.ppm --out "$out" >/dev/null
  echo "  $out  ${w}x${h}"
}

EXT="$IOS/UtttMessages/Assets.xcassets/iMessage App Icon.stickersiconset"
echo "the extension's drawer icon:"
draw   58   58 2 "$EXT/sq-58.png"
draw   87   87 3 "$EXT/sq-87.png"
draw  120   90 2 "$EXT/msg-120x90.png"
draw  180  135 3 "$EXT/msg-180x135.png"
draw  134  100 2 "$EXT/msg-134x100.png"
draw  148  110 2 "$EXT/msg-148x110.png"
draw   54   40 2 "$EXT/msg-54x40.png"
draw   81   60 3 "$EXT/msg-81x60.png"
draw   64   48 2 "$EXT/msg-64x48.png"
draw   96   72 3 "$EXT/msg-96x72.png"
draw 1024  768 1 "$EXT/msg-1024x768.png"

APP="$IOS/UtttMessagesApp/Assets.xcassets/AppIcon.appiconset"
echo "the container's app icon:"
draw 1024 1024 1 "$APP/AppIcon-1024.png"

WEB=../web/app
echo "uttt.live's favicon and home-screen icon (Next picks them up by name):"
draw   64   64 2 "$WEB/icon.png"
draw  180  180 3 "$WEB/apple-icon.png"
