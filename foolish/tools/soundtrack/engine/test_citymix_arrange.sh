#!/bin/bash
# test_citymix_arrange.sh: citymix_arrange.py places a blend, a breakdown, a loop and a final hit as the spec says.
# One PASS/FAIL line per check, non-zero exit on a failure. ARRANGE=/path/copy.py tests a (mutated) copy.
set -u
D="$(cd "$(dirname "$0")" && pwd)"; A="${ARRANGE:-$D/citymix_arrange.py}"; T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
printf 'a\t/x/a.mp3\t/x/sa\t0.5\t1.0\t4\nb\t/x/b.mp3\t/x/sb\t0.51\t0.2\t4\nc\t/x/c.mp3\t-\t0.4\t0.1\t4\n' > "$T/src.tsv"
printf 'scene a 0 4 plan=f,o\nblend 2\nscene b 0 4\nbreak 2\nscene c 0 2 hit=1\n' > "$T/s.txt"
python3 "$A" "$T/s.txt" "$T/src.tsv" "$T/o.cue" || { echo "FAIL runs"; exit 1; }
fail=0
chk() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
L=$(grep -c '^CLIP a ' "$T/o.cue"); chk "loop gives one clip per pass of the mix" "[ $L -eq 1 ] && grep -q '^CLIP a_o ' '$T/o.cue'"
chk "blend starts 2 bars early at the matched rate" "grep -q '^CLIP b 0.195 8.355 RATE 1.02000 GAP -4.000000 ' '$T/o.cue'"
chk "blend thins the outgoing pass" "grep '^CLIP a_o ' '$T/o.cue' | grep -q 'RIDE 4.9950:0,8.9950:-6'"
chk "breakdown waits two beats of the outgoing tempo" "grep '^CLIP c 0.095 3.295 ' '$T/o.cue' | grep -q 'GAP 1.000000'"
chk "a loop pass is marked as one" "grep '^CLIP a_o ' '$T/o.cue' | grep -q 'JOIN loop_pass_2'"
chk "final hit is the first beat of bar A" "grep -q '^CLIP c 0.095 0.495 .*final_hit' '$T/o.cue'"
exit $fail
