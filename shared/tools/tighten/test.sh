#!/bin/sh
# tighten's self test. No fixture: the source it works on is generated here, by
# the same ffmpeg the tool shells out to.
#
# The synthetic source is 25 seconds of 320x240 at 30fps, three flat colours
# with two bursts of testsrc2 spliced in. Nothing moves except during
#
#     5.0s -> 8.0s      and      15.0s -> 19.0s
#
# so every number the scan prints is a number this script chose, and an
# assertion here is a statement about tighten rather than about a recording
# somebody made once.
#
# Every assertion is a number out of ffprobe or off tighten's own stdout, never
# just an exit status: a render that silently produces the wrong shot still
# exits 0.

set -u

here=$(cd "$(dirname "$0")" && pwd)
bin="$here/build/tighten"
work=$(mktemp -d "${TMPDIR:-/tmp}/tighten-test-XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

fails=0
ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }

# near NAME ACTUAL WANT TOLERANCE
near() {
	if awk -v a="$2" -v w="$3" -v t="$4" \
	       'BEGIN { d = a - w; if (d < 0) d = -d; exit !(d <= t) }'
	then ok "$1 = $2"
	else bad "$1 = $2, wanted $3 +- $4"
	fi
}

# exact NAME ACTUAL WANT
exact() {
	if [ "$2" = "$3" ]; then ok "$1 = $2"; else bad "$1 = $2, wanted $3"; fi
}

# greater NAME A B  - A must be the larger
greater() {
	if awk -v a="$2" -v b="$3" 'BEGIN { exit !(a > b) }'
	then ok "$1 ($2 > $3)"
	else bad "$1: $2 is not greater than $3"
	fi
}

duration() { ffprobe -v error -show_entries format=duration -of csv=p=0 "$1"; }
height()   { ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$1"; }
width()    { ffprobe -v error -select_streams v:0 -show_entries stream=width  -of csv=p=0 "$1"; }

# channel FILE SECONDS INDEX - one 0-255 channel of the frame at SECONDS,
# averaged over the whole picture by scaling it to a single pixel. Which colour
# a frame is, is how this test knows which shot it is looking at.
channel() {
	ffmpeg -v error -ss "$2" -i "$1" -frames:v 1 -vf scale=1:1 \
	       -f rawvideo -pix_fmt rgb24 - 2>/dev/null |
	od -An -tu1 | awk -v i="$3" '{ print $(i + 1); exit }'
}

command -v ffmpeg  >/dev/null || { echo "test: no ffmpeg on PATH";  exit 1; }
command -v ffprobe >/dev/null || { echo "test: no ffprobe on PATH"; exit 1; }
make -C "$here" >/dev/null || exit 1

src="$work/src.mp4"
echo "building the source ..."
ffmpeg -v error -y \
	-f lavfi -i "color=c=navy:s=320x240:r=30:d=5"      \
	-f lavfi -i "testsrc2=s=320x240:r=30:d=3"          \
	-f lavfi -i "color=c=darkgreen:s=320x240:r=30:d=7" \
	-f lavfi -i "testsrc2=s=320x240:r=30:d=4"          \
	-f lavfi -i "color=c=maroon:s=320x240:r=30:d=6"    \
	-filter_complex "[0:v][1:v][2:v][3:v][4:v]concat=n=5:v=1:a=0,format=yuv420p[v]" \
	-map "[v]" -c:v libx264 -crf 18 -r 30 "$src" || exit 1
near "source duration" "$(duration "$src")" 25 0.05

# ---- scan ------------------------------------------------------------------
#
# At the defaults the two bursts come back padded by 0.60s on each side, so
# 5.0-8.0 is reported as 4.4-8.6 and 15.0-19.0 as 14.4-19.6. The 7 second still
# between them is far longer than --gap, so they stay two segments.
echo
echo "scan"
scan="$work/scan.txt"
"$bin" scan "$src" -o "$work/scan.edl" > "$scan" || exit 1
sed 's/^/    | /' "$scan"

exact "segments found" "$(awk '/segments,/ { print $1 }' "$scan")" 2
near "segment 0 in"  "$(awk '$1 == "0" { split($2, t, ":"); print t[1] * 60 + t[2] }' "$scan")" 4.4 0.25
near "segment 0 out" "$(awk '$1 == "0" { split($4, t, ":"); print t[1] * 60 + t[2] }' "$scan")" 8.6 0.25
near "segment 1 in"  "$(awk '$1 == "1" { split($2, t, ":"); print t[1] * 60 + t[2] }' "$scan")" 14.4 0.25
near "segment 1 out" "$(awk '$1 == "1" { split($4, t, ":"); print t[1] * 60 + t[2] }' "$scan")" 19.6 0.25
exact "edl data lines" "$(grep -cv '^ *\(#.*\)\?$' "$work/scan.edl")" 2

# ---- cut, straight through -------------------------------------------------
#
# 4.2s + 5.2s of kept motion, rendered in source order.
echo
echo "cut (scan)"
"$bin" cut "$src" -o "$work/scan.mp4" >/dev/null || exit 1
near "scanned cut duration" "$(duration "$work/scan.mp4")" 9.4 0.15

# ---- cut, from an EDL ------------------------------------------------------
#
# Two stills, two seconds each, named in the reverse of the order they occur in
# the source. The maroon one is last in the recording and first in the EDL, and
# the cut has to agree with the EDL: that is the whole reason the EDL exists.
echo
echo "cut (edl)"
cat > "$work/order.edl" <<'EDL'
# out of source order on purpose
   21.00    23.00
    1.00     3.00
EDL
"$bin" cut "$src" --edl "$work/order.edl" -o "$work/order.mp4" >/dev/null || exit 1
near "edl cut duration" "$(duration "$work/order.mp4")" 4.0 0.1

first_r=$(channel "$work/order.mp4" 0.5 0); first_b=$(channel "$work/order.mp4" 0.5 2)
last_r=$(channel "$work/order.mp4" 3.5 0);  last_b=$(channel "$work/order.mp4" 3.5 2)
greater "first shot is the maroon one (edl line 1)" "$first_r" "$first_b"
greater "last shot is the navy one (edl line 2)"    "$last_b"  "$last_r"

# ---- --crop-top ------------------------------------------------------------
#
# A tenth off a 240 high picture is 216, and the width is untouched.
echo
echo "crop-top"
"$bin" cut "$src" --edl "$work/order.edl" --crop-top 0.1 -o "$work/crop.mp4" >/dev/null || exit 1
exact "crop-top 0.1 height" "$(height "$work/crop.mp4")" 216
exact "crop-top 0.1 width"  "$(width  "$work/crop.mp4")" 320
exact "uncropped height"    "$(height "$work/order.mp4")" 240

# 0.0125 off 240 is 237, and an odd height is not a picture any 4:2:0 encoder
# will take. A 4:2:0 source would not prove the rounding is tighten's doing -
# ffmpeg's crop filter silently aligns its window to the chroma grid there - so
# the same crop is asked for on a 4:4:4 source, where 237 is perfectly legal
# right up until the encoder sees it.
src444="$work/src444.mp4"
ffmpeg -v error -y -f lavfi -i "testsrc2=s=320x240:r=30:d=3" \
	-c:v libx264 -crf 18 -pix_fmt yuv444p "$src444" || exit 1
printf '    0.50     2.50\n' > "$work/444.edl"
"$bin" cut "$src444" --edl "$work/444.edl" --crop-top 0.0125 -o "$work/odd.mp4" >/dev/null || exit 1
exact "crop-top 0.0125 rounds to even" "$(height "$work/odd.mp4")" 236

# ---- --tail ----------------------------------------------------------------
echo
echo "tail"
"$bin" cut "$src" --edl "$work/order.edl" --tail 1.5 -o "$work/tail.mp4" >/dev/null || exit 1
near "tailed cut duration" "$(duration "$work/tail.mp4")" 5.5 0.1
near "tail grew the cut by" \
     "$(awk -v a="$(duration "$work/tail.mp4")" -v b="$(duration "$work/order.mp4")" \
	    'BEGIN { print a - b }')" 1.5 0.1

# The source ends at 25.0, so a 5 second tail on a segment that ends at 24.5 has
# only half a second to give. Clamping is invisible to ffprobe - asking ffmpeg
# for more than the file holds just stops at the end either way - so the thing
# to check is that tighten knows, and says so.
tailout="$work/tail-clamp.txt"
printf '   23.00    24.50\n' > "$work/end.edl"
"$bin" cut "$src" --edl "$work/end.edl" --tail 5 -o "$work/end.mp4" > "$tailout" || exit 1
near "clamped tail duration" "$(duration "$work/end.mp4")" 2.0 0.1
if grep -q 'tail: only 0.50s of the requested 5.00s' "$tailout"
then ok "tail clamped to the end of the source, and said so"
else bad "no clamp notice: $(tr '\r' '\n' < "$tailout" | grep tail || echo '(nothing)')"
fi

echo
if [ "$fails" -eq 0 ]; then
	echo "all assertions passed"
	exit 0
fi
echo "$fails assertion(s) failed"
exit 1
