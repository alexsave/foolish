#!/bin/bash
# mkvid.sh CITY_DIR TAG -> videos/<city>_<tag>.mp4: out/full.wav on a still of out/an_full.png (or black), AAC 256k, phone-playable
set -e
city=$1; tag=${2:-r2}; D=$(cd "$(dirname "$0")/.." && pwd); name=$(basename "$city")
img="$D/$city/out/an_full.png"; out="$D/videos/${name}_${tag}.mp4"
if [ -f "$img" ] && [ "$img" -nt "$D/$city/out/full.wav" ]; then
  ffmpeg -v error -y -loop 1 -i "$img" -i "$D/$city/out/full.wav" -c:v libx264 -tune stillimage -pix_fmt yuv420p -vf "scale=1280:-2" -r 2 -c:a aac -b:a 256k -shortest -movflags +faststart "$out"
else
  ffmpeg -v error -y -f lavfi -i color=c=black:s=1280x720:r=2 -i "$D/$city/out/full.wav" -c:v libx264 -pix_fmt yuv420p -c:a aac -b:a 256k -shortest -movflags +faststart "$out"
fi
echo "$out"
