# sat2_rss.py: RSS of the cbase D server per sat2 run, over the play window (t_go..t_done, the load box's clock).
# Input: out/raw/srv/rss_LABEL.log (epoch VmRSS VmHWM RssAnon, kB, every 0.2 s), out/raw/load/LABEL/t_go and t_done.
import sys, statistics
raw = sys.argv[1]
def pc(v, q): v = sorted(v); return v[min(len(v)-1, int(q*(len(v)-1)+.5))]
for lab in ["s1_D", "s2_D", "s3_D", "s4_D"]:
    go = float(open(f"{raw}/load/{lab}/t_go").read().split()[0])
    done = float(open(f"{raw}/load/{lab}/t_done").read().split()[0])
    rows = []
    for line in open(f"{raw}/srv/rss_{lab}.log"):
        p = line.split()
        if len(p) == 4:
            t, r, h, a = float(p[0]), int(p[1]), int(p[2]), int(p[3])
            rows.append((t, r, h, a))
    win = [x for x in rows if go <= x[0] <= done]
    rss = [x[1] for x in win]; hwm = max(x[2] for x in rows); anon = [x[3] for x in win]
    print(f"{lab} samples_in_window={len(win)} RSS_p50_MiB={pc(rss,.5)/1024:.1f} RSS_max_MiB={max(rss)/1024:.1f} "
          f"VmHWM_MiB={hwm/1024:.1f} RssAnon_p50_kB={pc(anon,.5)} RssAnon_max_kB={max(anon)}")
