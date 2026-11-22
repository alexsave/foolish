#!/bin/sh
# The sound timed on the Game Boy (README "Sound"): every game and sound script
# against the profiling ROM (Makefile durak_sndprof.gb), then what it measured.
# TIMA and DIV count in steps of 256 clocks (61 us, 64 M-cycles). A tick lost
# would show as fewer ticks than the frames run make: 4096 / 68 ticks a second
# against 4194304 / 70224 frames, 1.00852 ticks a frame. The profiling ROM's
# own timing differs a little, so a script may play a different game here;
# only the numbers are read.
cd "$(dirname "$0")/.." || exit 2
for s in scripts/game_*.txt scripts/sound_*.txt; do
    sed '/ shot /d; / audio save /d' "$s" > build/sndprof/run.txt
    for v in late_max late_off_max tick_max wait_max vbl_max; do echo "+0   peek _snd_prof_$v" >> build/sndprof/run.txt; done
    for v in ticks wait_sum thinks late_at; do echo "+0   peek _snd_prof_$v 2" >> build/sndprof/run.txt; done
    build/gbrun build/durak_sndprof.gb build/sndprof/run.txt build/durak_sndprof.noi > build/sndprof/out.txt 2>&1
    python3 -I - "$s" build/sndprof/out.txt <<'PY'
import sys, re
v = {}; last = 0
for l in open(sys.argv[2]):
    m = re.match(r'frame (\d+): peek _snd_prof_(\w+) = ([0-9a-f ]+)', l)
    if m:
        last = int(m[1]); b = [int(x, 16) for x in m[3].split()]
        v[m[2]] = sum(x << (8 * i) for i, x in enumerate(b))
frames = last + 1
print("%-28s %6d frames %6d ticks (%+d)  late max %.2f ms (sys_time %d), LCD off %.2f ms  tick max %4d M-cycles  VBlank late %d lines  thinks %d, waited max %d, sum %d frames"
      % (sys.argv[1], frames, v["ticks"], v["ticks"] - round(frames * 1.00852), v["late_max"] * 0.061035, v["late_at"], v["late_off_max"] * 0.061035, v["tick_max"] * 64, v["vbl_max"],
         v["thinks"], v["wait_max"], v["wait_sum"]))
PY
done
