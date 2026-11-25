# per_move.py: per-move totals from the cachegrind outputs. Play = full run minus setup run (boot, sign-ups with bcrypt,
# and a short play window), divided by the play moves the load client counted (full moves minus setup moves).
import re, sys, json
OUT = "/Users/alex/Dev/foolish-bench-results/cachesim/out"
EV = "Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw Bc Bcm Bi Bim".split()
def summary(path):
    for line in open(path):
        if line.startswith("summary:"):
            return dict(zip(EV, map(int, line.split()[1:])))
    raise SystemExit("no summary in " + path)
def moves(path):
    for line in open(path):
        m = re.search(r"moves=(\d+)", line)
        if m: return int(m.group(1))
    raise SystemExit("no moves in " + path)
def row(tag, full, setup, mf, ms):
    play = {k: full[k] - setup[k] for k in EV}
    n = mf - ms
    res = {"tag": tag, "full_moves": mf, "setup_moves": ms, "play_moves": n,
           "per_move": {k: play[k] / n for k in EV}, "play_total": play, "full": full, "setup": setup}
    pm = res["per_move"]
    pm["D1_miss_rate_rd"] = play["D1mr"] / play["Dr"]; pm["DLmr_rate"] = play["DLmr"] / play["Dr"]
    pm["D1_miss_rate_all"] = (play["D1mr"] + play["D1mw"]) / (play["Dr"] + play["Dw"])
    pm["LL_miss_rate_all"] = (play["DLmr"] + play["DLmw"]) / (play["Dr"] + play["Dw"])
    pm["I1_miss_rate"] = play["I1mr"] / play["Ir"]
    pm["Mispred_rate"] = play["Bcm"] / play["Bc"]
    return res
runs = {}
for tag, f, s, mf, ms in [
    ("N1", "cg_n1.out", "cg_n1_setup.out", "load_n1.txt", "load_n1_setup.txt"),
    ("N1_instr", "cg_n1i.out", "cg_n1i_setup.out", "load_n1i.txt", "load_n1i_setup.txt"),
    ("M1", "cg_m1.out", "cg_m1_setup.out", "load_m1.txt", "load_m1_setup.txt")]:
    try:
        runs[tag] = row(tag, summary(f"{OUT}/{f}"), summary(f"{OUT}/{s}"), moves(f"{OUT}/{mf}"), moves(f"{OUT}/{ms}"))
    except (SystemExit, FileNotFoundError) as e:
        print("skip", tag, e)
runs["idle_N1"] = {"idle_summary": summary(f"{OUT}/cg_n1_idle.out")}
json.dump(runs, open(f"{OUT}/per_move.json", "w"), indent=1)
for tag, r in runs.items():
    if "per_move" not in r: continue
    pm = r["per_move"]
    print(f"== {tag}: play moves {r['play_moves']} (full {r['full_moves']}, setup {r['setup_moves']})")
    for k in EV: print(f"  {k:6s} per move {pm[k]:14.1f}   play total {r['play_total'][k]:>16,}")
    for k in ["D1_miss_rate_rd", "DLmr_rate", "D1_miss_rate_all", "LL_miss_rate_all", "I1_miss_rate", "Mispred_rate"]:
        print(f"  {k:18s} {pm[k]*100:7.3f} %")
