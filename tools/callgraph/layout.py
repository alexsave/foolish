#!/usr/bin/env python3
"""Two-level layout: force-directed on the file graph, phyllotaxis inside it.

7,195 function nodes laid out directly as one force sim is both slow and
unreadable. Instead the source FILES are laid out by force (edge weight =
number of call edges between the two files, plus an extra pull toward the
centroid of the file's module), and each file's functions are packed into a
disc around it, highest fan-in nearest the middle. The result is a map where
neighbourhoods mean something: files that call each other sit together, and
directories stay recognisable.

Run twice: once over everything, once with the test tree removed. Hiding tests
by filtering a single layout leaves craters where a third of the repo used to
be, so the test-free view gets its own honest layout instead.
"""
import json, math, os, sys, collections
import numpy as np

WORK = os.path.abspath(sys.argv[1])
g = json.load(open(os.path.join(WORK, "graph.json")))
ALL_NODES, ALL_EDGES = g["nodes"], g["edges"]
by_id = {n["id"]: n for n in ALL_NODES}


def lay_out(nodes, edges, label):
    files = sorted({n.get("file", "?") for n in nodes})
    fidx = {f: i for i, f in enumerate(files)}
    N = len(files)

    # --- file-level weighted graph ----------------------------------------
    w = collections.Counter()
    for e in edges:
        a, b = by_id[e["s"]]["file"], by_id[e["t"]]["file"]
        if a != b:
            w[(min(a, b), max(a, b))] += 1

    # A platform bucket ("(platform) Rust std" and friends) is one pseudo-file
    # holding every symbol that language brings with it. Left pulling in the sim
    # it wins the centre of the map on sheer degree, so its edges do not pull
    # and it is parked on the belt with the other unplaceable files.
    wf = {k: v for k, v in w.items() if not (k[0].startswith("(") or k[1].startswith("("))}
    ei = np.array([fidx[a] for a, b in wf], dtype=np.int32)
    ej = np.array([fidx[b] for a, b in wf], dtype=np.int32)
    ew = np.array([math.log1p(v) for v in wf.values()], dtype=np.float64)

    count = collections.Counter(n.get("file", "?") for n in nodes)
    size = np.array([math.sqrt(count[f]) for f in files])
    mods = ["/".join(f.split("/")[:-1]) if "/" in f else f for f in files]
    mset = sorted(set(mods))
    midx = np.array([mset.index(m) for m in mods], dtype=np.int32)

    # Files with no cross-file call edge at all have nothing but repulsion
    # acting on them, so a plain sim launches them to infinity. Hold them out
    # and park them in a belt around the finished core instead.
    deg = np.zeros(N)
    np.add.at(deg, ei, 1)
    np.add.at(deg, ej, 1)
    isolated = deg == 0
    for f in files:
        if f.startswith("("):
            isolated[fidx[f]] = True

    rng = np.random.default_rng(7)
    ang = np.array([2 * math.pi * mset.index(m) / len(mset) for m in mods])
    pos = np.stack([np.cos(ang), np.sin(ang)], 1) * 900 + rng.normal(0, 60, (N, 2))

    AREA = 2600.0
    k = AREA / math.sqrt(N)
    ITERS = 700
    for it in range(ITERS):
        t = (1 - it / ITERS)
        temp = 260 * t * t + 2

        d = pos[:, None, :] - pos[None, :, :]
        dist2 = (d ** 2).sum(-1) + 1e-6
        np.fill_diagonal(dist2, 1e9)
        rep = (d / dist2[..., None]) * (k * k)
        rep *= (size[None, :, None] * 0.55 + 1.0)
        disp = rep.sum(1)

        de = pos[ei] - pos[ej]
        dl = np.sqrt((de ** 2).sum(-1)) + 1e-6
        f = (de / dl[:, None]) * ((dl ** 2 / k)[:, None] * ew[:, None] * 0.55)
        np.add.at(disp, ei, -f)
        np.add.at(disp, ej, f)

        cent = np.zeros((len(mset), 2))
        cnt = np.zeros(len(mset))
        np.add.at(cent, midx, pos)
        np.add.at(cnt, midx, 1)
        cent /= cnt[:, None]
        disp += (cent[midx] - pos) * 0.09

        disp -= pos * 0.004
        disp[isolated] = 0

        dl = np.sqrt((disp ** 2).sum(-1))[:, None] + 1e-9
        pos += (disp / dl) * np.minimum(dl, temp)

    pos -= pos.mean(0)

    # --- normalise scale, then relax disc overlaps -------------------------
    R = 7.0 * np.sqrt(np.array([count[f] for f in files])) + 6.0
    d = np.sqrt(((pos[:, None, :] - pos[None, :, :]) ** 2).sum(-1))
    np.fill_diagonal(d, 1e9)
    d[isolated] = 1e9
    d[:, isolated] = 1e9
    nn = d.min(1)
    nn[isolated] = 1e9
    need = (R[:, None] + R[None, :])
    np.fill_diagonal(need, 0)
    # Parked files neither move nor shove: they are placed on the belt after
    # this loop, so letting their discs push the core around just inflates it.
    need[isolated, :] = 0
    need[:, isolated] = 0
    nn_need = need[np.arange(N), d.argmin(1)]
    ratio = (nn_need + 34.0) / np.maximum(nn, 1e-6)
    pos *= float(np.median(ratio[~isolated]))

    # Overlap push plus a real pull toward the centre: together they compact the
    # sim's output into a dense disc packing, which is what makes the map read
    # at a glance. Scale alone cannot do it - a force layout leaves long thin
    # chains that a uniform rescale keeps just as long.
    # Two phases. First overlap push against a pull toward the centre, which
    # compacts the sim's output into a dense packing - a uniform rescale cannot
    # do that, it keeps the long thin chains a force layout leaves behind.
    # Then pure separation, so the compaction never ends up with discs on top
    # of one another.
    def relax(iters, gravity, strength):
        nonlocal pos
        for _ in range(iters):
            dv = pos[:, None, :] - pos[None, :, :]
            dd = np.sqrt((dv ** 2).sum(-1)) + 1e-9
            np.fill_diagonal(dd, 1e9)
            over = np.maximum(0.0, (need + 20.0) - dd)
            push = (dv / dd[..., None]) * over[..., None] * strength
            step = push.sum(1)
            step[isolated] = 0
            pos += step
            pos -= pos * gravity

    relax(420, 0.008, 0.35)
    relax(260, 0.0, 0.5)
    pos[~isolated] -= pos[~isolated].mean(0)

    core = np.sqrt((pos[~isolated] ** 2).sum(-1)).max()
    iso = np.where(isolated)[0]
    iso = iso[np.argsort([mods[i] for i in iso], kind="stable")]
    belt = core * 1.16
    for j, i in enumerate(iso):
        a = 2 * math.pi * j / len(iso)
        ring = belt + (60 if j % 2 else 0)
        pos[i] = (ring * math.cos(a), ring * math.sin(a))

    # --- pack each file's functions into its disc --------------------------
    GOLD = math.pi * (3 - math.sqrt(5))
    per_file = collections.defaultdict(list)
    for n in nodes:
        per_file[n.get("file", "?")].append(n)

    xy, file_meta = {}, {}
    for f, group in per_file.items():
        group.sort(key=lambda n: -(n["in"] * 2 + n["out"]))
        cx, cy = pos[fidx[f]]
        m = len(group)
        rad = 7.0 * math.sqrt(m) + 6.0
        for i, n in enumerate(group):
            r = rad * math.sqrt((i + 0.5) / m)
            a = i * GOLD
            xy[n["id"]] = (round(cx + r * math.cos(a), 1), round(cy + r * math.sin(a), 1))
        file_meta[f] = {"x": round(float(cx), 1), "y": round(float(cy), 1),
                        "r": round(rad, 1), "n": m}

    span = pos.max(0) - pos.min(0)
    print("%-9s %5d nodes / %3d file clusters / extent %.0f x %.0f"
          % (label, len(nodes), N, span[0], span[1]))
    return xy, file_meta


# --- one layout per toggle combination -------------------------------------
# Tests are a quarter of the tree and the platform buckets another sixth, so
# filtering a single layout would leave craters where they used to be. Each
# combination the UI can show gets its own honest layout instead.
ONLY = sys.argv[2].split(",") if len(sys.argv) > 2 else None
VARIANTS = [
    ("full",   lambda n: True),
    ("noTest", lambda n: n["cat"] != "test"),
    ("noPlat", lambda n: n["cat"] != "platform"),
    ("noBoth", lambda n: n["cat"] != "test" and n["cat"] != "platform"),
]

g["layouts"] = {}
for key, keep_fn in VARIANTS:
    if ONLY and key not in ONLY:
        continue
    keep = {n["id"] for n in ALL_NODES if keep_fn(n)}
    edges = [e for e in ALL_EDGES if e["s"] in keep and e["t"] in keep]
    # A platform symbol that only ever appeared because a test called it has no
    # place in a test-free view; drop the now-unreferenced stubs.
    used = set()
    for e in edges:
        used.add(e["s"])
        used.add(e["t"])
    keep = {i for i in keep if by_id[i]["cat"] != "platform" or i in used}
    edges = [e for e in edges if e["s"] in keep and e["t"] in keep]
    nodes = [n for n in ALL_NODES if n["id"] in keep]
    xy, file_meta = lay_out(nodes, edges, key)
    g["layouts"][key] = {
        "xy": {i: xy[i] for i in keep},
        "files": file_meta,
        "nodes": len(nodes),
        "edges": len(edges),
    }

json.dump(g, open(os.path.join(WORK, "graph_laid.json"), "w"), separators=(",", ":"))
