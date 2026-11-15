#!/usr/bin/env python
"""Render the ALT-CELLO geometry from geomdump text output.

Usage:  render_geometry.py [OUT_DIR]
Runs ../geomdump (build it with ../build_geom.sh first) and writes
geometry_overview.png, geometry_closeup.png, geometry_sections.png.
Everything is drawn from the same C description that the FDTD voxelises."""
import os
import subprocess
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import BoundaryNorm, ListedColormap
from matplotlib.lines import Line2D
from matplotlib.patches import Patch, Rectangle
from mpl_toolkits.mplot3d import proj3d
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

HERE = os.path.dirname(os.path.abspath(__file__))
GEOMDUMP = os.path.join(HERE, "..", "geomdump")
OUT = sys.argv[1] if len(sys.argv) > 1 else "/Users/alex/Music/foolish_cities_v5/alt_cello/images"
DPI = 110

COLORS = {
    "BODY_TOP": "#d9a15a", "BODY_BACK": "#8f5b2c", "RIBS": "#b87a3e", "FHOLE": "#0b0b0b",
    "BRIDGE": "#f2e2b3", "NECK": "#6a4326", "FINGERBOARD": "#2a2a2e", "SCROLL": "#9a6234",
    "STRING_C": "#c0392b", "STRING_G": "#e08a00", "STRING_D": "#2e8b3d", "STRING_A": "#2f6fd0",
    "BOW_STICK": "#5a2d12", "BOW_HAIR": "#f4f1e4", "FINGER": "#f0a58c", "MIC": "#5f7f9f",
    "TAILPIECE": "#3b2c26", "ENDPIN": "#8a8f94",
}
PRETTY = {
    "BODY_TOP": "body top plate", "BODY_BACK": "body back plate", "RIBS": "ribs", "FHOLE": "f-holes (open)",
    "BRIDGE": "bridge", "NECK": "neck and pegbox", "FINGERBOARD": "fingerboard and nut", "SCROLL": "scroll and pegs",
    "STRING_C": "string C2", "STRING_G": "string G2", "STRING_D": "string D3", "STRING_A": "string A3",
    "BOW_STICK": "bow stick", "BOW_HAIR": "bow hair", "FINGER": "finger", "MIC": "microphone and stand",
    "TAILPIECE": "tailpiece and gut", "ENDPIN": "endpin",
}
ORDER = ["BODY_BACK", "RIBS", "BODY_TOP", "FHOLE", "TAILPIECE", "ENDPIN", "NECK", "FINGERBOARD", "SCROLL",
         "BRIDGE", "STRING_C", "STRING_G", "STRING_D", "STRING_A", "BOW_STICK", "BOW_HAIR", "FINGER", "MIC"]


def run(*args):
    out = subprocess.run([GEOMDUMP, *args], check=True, capture_output=True, text=True).stdout
    return out.splitlines()


def parse_mesh(lines):
    names, tris, parts, pts, boxes, params = {}, [], [], {}, {}, {}
    for ln in lines:
        w = ln.split()
        if not w or w[0] == "#":
            continue
        if w[0] == "part":
            names[int(w[1])] = w[2]
        elif w[0] == "tri":
            tris.append([float(v) for v in w[1:10]])
            parts.append(int(w[10]))
        elif w[0] == "pt":
            pts[w[1]] = np.array([float(v) for v in w[2:5]])
        elif w[0] == "box":
            boxes[w[1]] = ([float(v) for v in w[2:8]], " ".join(w[8:]))
        elif w[0] == "param":
            params[w[1]] = float(w[2])
    return names, np.array(tris).reshape(-1, 3, 3), np.array(parts), pts, boxes, params


def shade(tri, base, light=np.array([-0.45, -0.75, 0.5])):
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    ln = np.linalg.norm(n, axis=1)
    ln[ln == 0] = 1
    n = n / ln[:, None]
    lam = np.abs(n @ (light / np.linalg.norm(light)))
    f = 0.38 + 0.62 * lam
    rgb = np.array(matplotlib.colors.to_rgb(base))
    return np.clip(rgb[None, :] * f[:, None], 0, 1)


def clip_poly(poly, lo, hi):
    """Sutherland-Hodgman clip of one convex polygon (list of 3-vectors) to the box [lo, hi]."""
    for ax in range(3):
        for bound, keep_ge in ((lo[ax], True), (hi[ax], False)):
            out = []
            n = len(poly)
            for i in range(n):
                a, b = poly[i], poly[(i + 1) % n]
                ina = a[ax] >= bound if keep_ge else a[ax] <= bound
                inb = b[ax] >= bound if keep_ge else b[ax] <= bound
                if ina:
                    out.append(a)
                if ina != inb:
                    t = (bound - a[ax]) / (b[ax] - a[ax])
                    out.append(a + (b - a) * t)
            poly = out
            if not poly:
                return []
    return poly


def clip_tris(t, window):
    """Triangles (n,3,3) clipped to the window (3,2); returns polygons and the source index of each."""
    lo, hi = window[:, 0], window[:, 1]
    tmin, tmax = t.min(axis=1), t.max(axis=1)
    outside = np.any((tmax < lo) | (tmin > hi), axis=1)
    inside = np.all((tmin >= lo) & (tmax <= hi), axis=1)
    polys, idx = [], []
    for i in range(len(t)):
        if outside[i]:
            continue
        if inside[i]:
            polys.append(t[i]); idx.append(i)
        else:
            c = clip_poly([t[i][0], t[i][1], t[i][2]], lo, hi)
            if len(c) >= 3:
                polys.append(np.array(c)); idx.append(i)
    return polys, np.array(idx, dtype=int)


def draw_mesh(ax, names, tris, parts, window=None, edge_parts=("STRING_C", "STRING_G", "STRING_D", "STRING_A", "BOW_HAIR", "FHOLE")):
    z = 0
    for nm in ORDER:
        pid = [k for k, v in names.items() if v == nm][0]
        t = tris[parts == pid]
        if len(t) == 0:
            continue
        col = shade(t, COLORS[nm])
        if window is not None:
            polys, idx = clip_tris(t, window)
            if not polys:
                continue
            col = col[idx]
        else:
            polys = t
        pc = Poly3DCollection(polys, facecolors=col, edgecolors=col, linewidths=0.6 if nm in edge_parts else 0.25)
        pc.set_zorder(z)
        z += 1
        ax.add_collection3d(pc)


def box_wire(ax, lo, hi, color, ls, lw, label=None):
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    c = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    edges = [(0, 1), (2, 3), (4, 5), (6, 7), (0, 2), (1, 3), (4, 6), (5, 7), (0, 4), (1, 5), (2, 6), (3, 7)]
    for k, (a, b) in enumerate(edges):
        ax.plot([c[a][0], c[b][0]], [c[a][1], c[b][1]], [c[a][2], c[b][2]], color=color, ls=ls, lw=lw,
                alpha=0.8, label=label if k == 0 else None, zorder=0)


def legend_handles(names_used):
    return [Patch(facecolor=COLORS[n], edgecolor="#333", label=PRETTY[n]) for n in ORDER if n in names_used]


def setup3d(ax, lim, aspect, elev, azim, ortho=True, labels=True):
    ax.computed_zorder = False
    ax.set_xlim(*lim[0]); ax.set_ylim(*lim[1]); ax.set_zlim(*lim[2])
    ax.set_box_aspect(aspect)
    ax.view_init(elev=elev, azim=azim)
    if ortho:
        ax.set_proj_type("ortho")
    else:
        ax.set_proj_type("persp", focal_length=0.45)
    if labels:
        ax.set_xlabel("x (m)", labelpad=6); ax.set_ylabel("y (m)", labelpad=6); ax.set_zlabel("z (m)", labelpad=4)
    ax.xaxis.pane.set_facecolor((0.96, 0.96, 0.97, 0.6)); ax.yaxis.pane.set_facecolor((0.96, 0.96, 0.97, 0.6))
    ax.zaxis.pane.set_facecolor((0.96, 0.96, 0.97, 0.6))


def proj2d(ax, p):
    x, y, _ = proj3d.proj_transform(p[0], p[1], p[2], ax.get_proj())
    return x, y


def label(ax, text, p, off, color="#111", fs=10, ha="left"):
    """off: offset in points, or a pair of axes fractions given as ('f', fx, fy)."""
    x, y = proj2d(ax, p)
    tc = "offset points"
    if len(off) == 3:
        tc, off = "axes fraction", (off[1], off[2])
    ax.annotate(text, xy=(x, y), xycoords="data", xytext=off, textcoords=tc, fontsize=fs, color=color,
                ha=ha, va="center", zorder=100,
                bbox=dict(boxstyle="round,pad=0.2", fc="white", ec=color, lw=0.8, alpha=0.92),
                arrowprops=dict(arrowstyle="-|>", color=color, lw=1.1, shrinkA=0, shrinkB=1))


# --------------------------------------------------------------------------------------------
def overview(room_lines, path):
    names, tris, parts, pts, boxes, par = parse_mesh(room_lines)
    fig = plt.figure(figsize=(16, 11), dpi=DPI)
    fig.suptitle("ALT-CELLO - the model: cello body, neck, strings, bow, finger, microphone and room",
                 fontsize=19, fontweight="bold", y=0.975)
    ax = fig.add_axes([0.0, 0.17, 0.56, 0.74], projection="3d")
    rlo, rhi = boxes["room"][0][:3], boxes["room"][0][3:]
    flo, fhi = boxes["fdtd"][0][:3], boxes["fdtd"][0][3:]
    setup3d(ax, [(rlo[0], rhi[0]), (rlo[1], rhi[1]), (rlo[2], rhi[2])], (6, 5, 3.2), 20, -58, ortho=True)
    ax.set_box_aspect((6, 5, 3.2), zoom=0.80)
    box_wire(ax, rlo, rhi, "#445566", "-", 0.9)
    box_wire(ax, flo, fhi, "#c0392b", (0, (4, 3)), 1.4)
    draw_mesh(ax, names, tris, parts)
    gx = [rlo[0], rhi[0], rhi[0], rlo[0]]; gy = [rlo[1], rlo[1], rhi[1], rhi[1]]
    fl = Poly3DCollection([list(zip(gx, gy, [0] * 4))], facecolors=(0.5, 0.6, 0.7, 0.13), edgecolors="none")
    fl.set_zorder(-1)
    ax.add_collection3d(fl)
    fig.canvas.draw()
    label(ax, "microphone, 1 m in front of the top plate", pts["mic"], (-30, 85), "#1f4e79", 10, "right")
    label(ax, "room 6 x 5 x 3.2 m (wireframe)", np.array([rhi[0], flo[1], rhi[2]]), (-20, 105), "#445566", 10, "right")
    label(ax, "3D FDTD domain (dx 2.5 cm)", np.array([fhi[0], flo[1], fhi[2]]), (-110, 25), "#c0392b", 10, "right")
    ax.set_title("room, FDTD domain and the cello standing on its endpin", fontsize=12, pad=-2)

    ax2 = fig.add_axes([0.50, 0.17, 0.50, 0.74], projection="3d")
    setup3d(ax2, [(-0.5, 0.5), (-0.65, 0.85), (0.0, 1.6)], (1.0, 1.5, 1.6), 14, -58, ortho=True)
    draw_mesh(ax2, names, tris, parts)
    fig.canvas.draw()
    label(ax2, "bow on the D string, beta 0.1", pts["bow_contact"], (-115, 75), "#5a2d12", 10)
    label(ax2, "finger on the A string (E4)", pts["finger"], (70, 55), "#a0522d", 10)
    label(ax2, "microphone", pts["mic"], (-30, 60), "#1f4e79", 10, "right")
    label(ax2, "f-hole", pts["fhole"], (75, -30), "#000", 10)
    label(ax2, "bridge", pts["bridge"], (-85, -45), "#7a6a30", 10)
    label(ax2, "endpin", np.array([0, 0.5 + 0.0, 0.1]), (60, -25), "#555", 10)
    ax2.set_title("the cello and the microphone", fontsize=12, pad=-2)

    handles = legend_handles(set(names.values()))
    handles += [Line2D([0], [0], color="#445566", lw=1, label="room box"),
                Line2D([0], [0], color="#c0392b", lw=1.4, ls=(0, (4, 3)), label="FDTD box")]
    fig.legend(handles=handles, loc="lower right", bbox_to_anchor=(0.995, 0.012), ncol=3, fontsize=10,
               title="parts (by name)", frameon=True)
    txt = ("key dimensions\n"
           f"body length   {par['body_length']:.3f} m\n"
           f"upper bout    {par['w_upper']*100:.1f} cm\n"
           f"C-bout        {par['w_waist']*100:.1f} cm\n"
           f"lower bout    {par['w_lower']*100:.1f} cm\n"
           f"string length {par['string_length']:.2f} m (nut to bridge)\n"
           f"bow-bridge distance beta = {par['bow_beta']:.1f}\n"
           f"   = {par['bow_beta']*par['string_length']*100:.1f} cm along the D string\n"
           f"stopped: A string, {par['finger_dist']*100:.1f} cm from nut (E4)\n"
           f"mic distance  {par['mic_distance']:.2f} m (bridge to capsule)\n"
           "units: metres; room Z up, top plate faces -Y")
    fig.text(0.012, 0.012, txt, family="monospace", fontsize=9.5, va="bottom",
             bbox=dict(boxstyle="round,pad=0.5", fc="#fbfbf4", ec="#888"))
    fig.savefig(path, dpi=DPI)
    plt.close(fig)


def closeup(room_lines, path):
    names, tris, parts, pts, boxes, par = parse_mesh(room_lines)
    fig = plt.figure(figsize=(16, 11), dpi=DPI)
    fig.suptitle("ALT-CELLO - bowing and stopping: bow hair on the D string at beta 0.1, finger stopping the A string at E4",
                 fontsize=17, fontweight="bold", y=0.975)

    def lerp(a, b, t):
        return a + (b - a) * t

    def strpt(s, t):  # t from the bridge toward the nut
        return lerp(pts[f"string_{s}_bridge"], pts[f"string_{s}_nut"], t)

    wA = np.array([[-0.13, 0.13], [0.30, 0.52], [0.50, 0.86]])
    axA = fig.add_axes([0.0, 0.07, 0.5, 0.84], projection="3d")
    setup3d(axA, [tuple(w) for w in wA], (0.26, 0.22, 0.36), 20, -38)
    draw_mesh(axA, names, tris, parts, window=wA)
    axA.set_title("bridge, strings and the bow hair", fontsize=13, pad=-8)
    wB = np.array([[-0.07, 0.07], [0.26, 0.42], [0.90, 1.24]])
    axB = fig.add_axes([0.5, 0.07, 0.5, 0.84], projection="3d")
    setup3d(axB, [tuple(w) for w in wB], (0.14, 0.16, 0.34), 20, -38)
    draw_mesh(axB, names, tris, parts, window=wB)
    axA.set_xticks([-0.1, 0, 0.1]); axA.set_yticks([0.3, 0.4, 0.5])
    axB.set_xticks([-0.05, 0, 0.05]); axB.set_yticks([0.28, 0.34, 0.40])
    axB.set_title("the finger stopping the A string at E4 (22.9 cm from the nut)", fontsize=13, pad=-8)
    fig.canvas.draw()

    cols = {"C": "#c0392b", "G": "#b86e00", "D": "#2e8b3d", "A": "#2f6fd0"}
    nm = {"C": "C2", "G": "G2", "D": "D3", "A": "A3"}
    for k, s in enumerate("CGDA"):
        label(axA, f"string {nm[s]}", strpt(s, 0.26), ("f", 0.02, 0.92 - 0.065 * k), cols[s], 10, "left")
    label(axA, "bridge", pts["bridge"], ("f", 0.02, 0.18), "#7a6a30", 11, "left")
    label(axA, "f-hole", pts["fhole"], ("f", 0.80, 0.14), "#000", 11)
    label(axA, "bow hair (62 cm long, 10 mm wide)", pts["bow_contact"] + np.array([0.07, 0, 0.0]), ("f", 0.52, 0.80), "#4a4a40", 11, "left")
    label(axA, "bow contact point (beta 0.1:\n6.9 cm from the bridge, on the D string)", pts["bow_contact"], ("f", 0.02, 0.50), "#a01010", 11, "left")
    for k, s in enumerate("CGDA"):
        label(axB, nm[s], strpt(s, 0.72), ("f", 0.02, 0.92 - 0.065 * k), cols[s], 10, "left")
    label(axB, "finger (tip 16 x 12 x 12 mm)", pts["finger"] + np.array([0, -0.03, -0.004]), ("f", 0.40, 0.88), "#a0522d", 11, "left")
    label(axB, "stopped point on the A string", pts["finger"], ("f", 0.40, 0.22), "#2f6fd0", 11, "left")
    label(axB, "fingerboard", strpt("G", 0.76) + np.array([0, 0.012, 0.0]), ("f", 0.40, 0.08), "#333", 11, "left")

    for ax, w in ((axA, wA), (axB, wB)):
        p0 = np.array([w[0][0] + 0.006, w[1][0] + 0.006, w[2][0] + 0.02])
        ax.plot([p0[0], p0[0]], [p0[1], p0[1]], [p0[2], p0[2] + 0.05], color="k", lw=4, zorder=200)
        x, y = proj2d(ax, p0 + np.array([0, 0, 0.025]))
        ax.annotate("5 cm", xy=(x, y), xycoords="data", xytext=(8, 0), textcoords="offset points", fontsize=11,
                    fontweight="bold", va="center", zorder=200)
    handles = legend_handles(set(names.values()))
    fig.legend(handles=handles, loc="lower center", ncol=9, fontsize=9.5, frameon=True, bbox_to_anchor=(0.5, 0.0))
    fig.savefig(path, dpi=DPI)
    plt.close(fig)


def parse_sections(lines):
    names, slices, params = {}, {}, {}
    i = 0
    while i < len(lines):
        w = lines[i].split()
        i += 1
        if not w or w[0] == "#":
            continue
        if w[0] == "part":
            names[int(w[1])] = w[2]
        elif w[0] == "param":
            params[w[1]] = float(w[2])
        elif w[0] == "slice":
            nm = w[1]
            fa, fv, aa, a0, da, na, ba, b0, db, nb, dx = (int(w[2]), float(w[3]), int(w[4]), float(w[5]), float(w[6]),
                                                           int(w[7]), int(w[8]), float(w[9]), float(w[10]), int(w[11]), float(w[12]))
            rows = [[int(v) for v in lines[i + j].split()] for j in range(nb)]
            i += nb
            slices[nm] = dict(fa=fa, fv=fv, aa=aa, ba=ba, a0=a0, b0=b0, dx=dx, na=na, nb=nb, g=np.array(rows))
    return names, slices, params


def sections(lines, path):
    names, sl, par = parse_sections(lines)
    AX = "xyz"
    colors = ["#eef2f6", "#9fc8e8", "#e0201c", "#000000"] + [COLORS[names[p]] for p in range(1, 19)]
    cmap = ListedColormap(colors)
    norm = BoundaryNorm(np.arange(-0.5, len(colors) + 0.5, 1), cmap.N)
    fig = plt.figure(figsize=(16, 11), dpi=DPI)
    fig.suptitle("ALT-CELLO - FDTD voxelisation at dx 2.5 cm (what the wave simulation sees)", fontsize=19,
                 fontweight="bold", y=0.975)
    pos = {"sagittal": [0.05, 0.68, 0.56, 0.22], "cross": [0.68, 0.68, 0.30, 0.22],
           "plan_mid": [0.05, 0.20, 0.20, 0.40], "plan": [0.30, 0.20, 0.20, 0.40],
           "plan_inset": [0.56, 0.20, 0.16, 0.40], "cross_inset": [0.79, 0.20, 0.19, 0.40]}
    titles = {
        "sagittal": "a) sagittal slice x = 0.0125 m, dx 2.5 cm (strings thickened to one cell)",
        "cross": "b) cross section y = 0.37 m through the f-holes, dx 2.5 cm",
        "plan_mid": "c) plan slice z = -0.06 m (mid depth), dx 2.5 cm",
        "plan": "d) plan slice z = 0.012 m (arch level), dx 2.5 cm",
        "plan_inset": "e) inset of d: right f-hole, dx 5 mm",
        "cross_inset": "f) inset of b: right f-hole, dx 5 mm",
    }
    for nm, p in pos.items():
        s = sl[nm]
        ax = fig.add_axes(p)
        ext = [s["a0"], s["a0"] + s["na"] * s["dx"], s["b0"], s["b0"] + s["nb"] * s["dx"]]
        ax.imshow(s["g"], origin="lower", extent=ext, cmap=cmap, norm=norm, interpolation="nearest", aspect="equal")
        ax.set_xlabel(f"{AX[s['aa']]} (m)"); ax.set_ylabel(f"{AX[s['ba']]} (m)")
        ax.set_title(titles[nm], fontsize=10)
        ax.tick_params(labelsize=8)
        if nm in ("plan", "plan_mid"):
            ax.add_patch(Rectangle((0.02, 0.29), 0.11, 0.16, fill=False, ec="k", lw=1.2, ls="--"))
        if nm == "cross":
            ax.add_patch(Rectangle((0.02, -0.02), 0.11, 0.08, fill=False, ec="k", lw=1.2, ls="--"))
    present = sorted({int(v) for k in sl for v in np.unique(sl[k]["g"])})
    hs = []
    for c in present:
        if c == 0: hs.append(Patch(fc=colors[0], ec="#777", label="outside air"))
        elif c == 1: hs.append(Patch(fc=colors[1], ec="#777", label="cavity air (inside the body)"))
        elif c == 2: hs.append(Patch(fc=colors[2], ec="#777", label="f-hole opening (open cells)"))
        else: hs.append(Patch(fc=colors[c], ec="#333", label=PRETTY[names[c - 3]]))
    fig.legend(handles=hs, loc="lower center", ncol=6, fontsize=10, frameon=True, title="cell classes (axes in metres)",
               bbox_to_anchor=(0.5, 0.0))
    fig.text(0.05, 0.145, "rule: a cell is solid if any ~2 mm sample in it is solid, so the 2-4 mm plates and ribs become one cell thick; "
             "a cell is an open f-hole when at least a quarter of its plate column is slot.", fontsize=9.5, color="#444")
    fig.savefig(path, dpi=DPI)
    plt.close(fig)


def main():
    os.makedirs(OUT, exist_ok=True)
    room = run("mesh", "frame=room", "finger2_string=-1")
    overview(room, os.path.join(OUT, "geometry_overview.png"))
    closeup(room, os.path.join(OUT, "geometry_closeup.png"))
    sections(run("sections"), os.path.join(OUT, "geometry_sections.png"))
    print("wrote 3 images to", OUT)


if __name__ == "__main__":
    main()
