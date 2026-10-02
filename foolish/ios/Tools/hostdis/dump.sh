#!/bin/bash
# Re-create the disassembly dumps behind docs/IMESSAGE_LIVE_ARRIVAL_HOST.md
# for one simulator runtime, so the report can be re-checked on a new iOS.
#
#   ios/Tools/hostdis/dump.sh <RuntimeRoot> <out dir> [tag]
#
# <RuntimeRoot> is ".../iOS NN.N.simruntime/Contents/Resources/RuntimeRoot"
# (list runtimes with `xcrun simctl runtime list -v`; this script itself never
# touches a simulator). For each binary it writes <tag>.<name>.text.txt
# (`otool -tV`, with objc stub calls named, see symbolize.sh) and
# <tag>.<name>.objc.txt (`xcrun dyld_info -objc`: class, method, address).
# The dumps are large (ChatKit is ~110 MB) and are never committed.
#
# A binary that is missing on disk lives in the runtime's dyld shared cache
# (iOS 27 moved Messages.framework and ChatKit there); it is reported and
# skipped. Extracting it is phase-2 work.
set -euo pipefail
root=${1:?RuntimeRoot}; out=${2:?out dir}; tag=${3:-rt}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
stubs="$out/objc_stubs"
cc -O2 -o "$stubs" "$here/objc_stubs.c"
plutil -p "$root/System/Library/CoreServices/SystemVersion.plist" > "$out/$tag.SystemVersion.txt"
while read -r name rel; do
    bin="$root/$rel"
    if [ ! -f "$bin" ]; then echo "skip $name: not on disk ($rel)"; continue; fi
    echo "$name $(stat -f %z "$bin") $(shasum -a 256 "$bin" | cut -d' ' -f1)" >> "$out/$tag.binaries.txt"
    "$stubs" "$bin" > "$out/$tag.$name.stubs.txt"
    otool -tV "$bin" | "$here/symbolize.sh" "$out/$tag.$name.stubs.txt" > "$out/$tag.$name.text.txt"
    xcrun dyld_info -objc "$bin" > "$out/$tag.$name.objc.txt"
    echo "dumped $name"
done <<'EOF'
messages System/Library/Frameworks/Messages.framework/Messages
plugin System/Library/Messages/iMessageBalloons/MSMessageExtensionBalloonPlugin.bundle/MSMessageExtensionBalloonPlugin
chatkit System/Library/PrivateFrameworks/ChatKit.framework/ChatKit
imcore System/Library/PrivateFrameworks/IMCore.framework/IMCore
imessageapps System/Library/PrivateFrameworks/iMessageApps.framework/iMessageApps
EOF
