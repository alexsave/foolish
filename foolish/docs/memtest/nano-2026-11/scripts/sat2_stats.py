import sys, glob, os
# per run: server CPU over the go..done window (libproc cumulative CPU, 1 s samples), mean and peak CPU %, RSS
base = "/Users/alex/Dev/foolish-bench-results/raw/sat2"
for n in (1, 2, 3):
    tsv = f"{base}/sat2_r{n}.server.tsv"
    rows = [l.split() for l in open(tsv) if l[0] != "#" and not l.startswith("# ")]
    rows = [(float(a), float(b), int(c), int(d)) for a, b, c, d in rows]
    tgo = float(open(f"{base}/sat2_r{n}/t_go").read()); tdone = float(open(f"{base}/sat2_r{n}/t_done").read())
    win = [r for r in rows if tgo - 1.5 <= r[0] <= tdone + 1.5]
    # cumulative CPU at the window edges, interpolated from the nearest samples
    def at(t):
        best = min(rows, key=lambda r: abs(r[0] - t)); return best
    a = at(tgo); b = at(tdone)
    dcpu = b[1] - a[1]; dt = b[0] - a[0]
    moves = int(open(f"{base}/sat2_r{n}.out").read().split("moves=")[1].split()[0])
    peak = max(((win[i][1] - win[i-1][1]) / (win[i][0] - win[i-1][0]) * 100) for i in range(1, len(win)))
    print(f"run {n}: window_s={dt:.2f} server_cpu_s={dcpu:.3f} mean_cpu_pct={dcpu/dt*100:.1f} peak_1s_cpu_pct={peak:.1f} "
          f"moves={moves} us_per_move={dcpu/moves*1e6:.1f} rss_kb_before={a[2]} rss_kb_max={max(r[2] for r in win)} rss_kb_end={b[2]} samples={len(win)}")
