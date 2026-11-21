#!/usr/bin/env python3
# Export the Game Boy art for GBDK: indexed PNGs, png2asset C sources, the
# tile index header, and a check that png2asset's bytes decode back to the
# exact tiles the generator drew.  Run after gen_art.py:  python3 export.py

import os
import re
import subprocess
import sys

from PIL import Image

import gen_art as A
import screens_full
import wins
import travel

HERE = os.path.dirname(os.path.abspath(__file__))
PNG = os.path.join(HERE, "png")
OUT = os.path.join(HERE, "gbdk")
GBDK = os.environ.get("GBDK", "/Users/alex/Dev/gb-hello/toolchain/dl/gbdk")
PNG2ASSET = os.path.join(GBDK, "bin", "png2asset")

# VRAM: one 256-tile space at 0x8000 shared by the background, the window
# and the sprites (LCDC bit 4 = 1, GBDK's default).  Card tiles double as
# sprite tiles for a flying card; font tiles double as sprite 'chips'.
BASE = {"cards": 0, "ui": 72, "sprites": 140}
PALETTE = [A.SHADE[i] for i in (A.T, A.W, A.D, A.K)]   # index order


def indexed(cv_tiles, w_tiles):
    """Tiles -> an indexed PNG image, w_tiles wide, palette in index order."""
    h_tiles = (len(cv_tiles) + w_tiles - 1) // w_tiles
    im = Image.new("P", (w_tiles * 8, h_tiles * 8), 0)
    pal = []
    for c in PALETTE:
        pal += list(c)
    im.putpalette(pal + [0] * (768 - len(pal)))
    px = im.load()
    for k, t in enumerate(cv_tiles):
        ox, oy = (k % w_tiles) * 8, (k // w_tiles) * 8
        for j in range(8):
            for i in range(8):
                px[ox + i, oy + j] = t[j * 8 + i]
    return im


def screen_png(cv):
    im = Image.new("P", (160, 144), 0)
    pal = []
    for c in PALETTE:
        pal += list(c)
    im.putpalette(pal + [0] * (768 - len(pal)))
    px = im.load()
    for y in range(144):
        for x in range(160):
            v = cv.px[y][x]
            px[x, y] = A.T if v is None else v
    return im


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("failed: %s\n%s%s" % (" ".join(args), r.stdout, r.stderr))


def c_bytes(path, name):
    """The bytes of `const unsigned char name[] = {...};` in a png2asset .c file."""
    src = open(path).read()
    m = re.search(r"%s\[\d*\]\s*=\s*\{(.*?)\};" % re.escape(name), src, re.S)
    assert m, (path, name)
    return [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]


def decode_2bpp(b):
    tiles = []
    for k in range(0, len(b), 16):
        t = []
        for j in range(8):
            lo, hi = b[k + 2 * j], b[k + 2 * j + 1]
            for i in range(8):
                bit = 7 - i
                t.append(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1))
        tiles.append(tuple(t))
    return tiles


def ident(name):
    out = name.upper().replace(" ", "SPACE")
    out = re.sub(r"[^A-Z0-9_]", lambda m: "_%02X" % ord(m.group(0)), out)
    return out


def main():
    os.makedirs(PNG, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    banks = {"cards": A.card_bank(), "ui": A.ui_bank(), "sprites": A.sprite_bank()}
    for b, bank in banks.items():
        tiles = list(bank.values())
        assert len(set(tiles)) == len(tiles), "%s bank has duplicate tiles" % b
    assert BASE["ui"] >= len(banks["cards"]) and BASE["sprites"] >= BASE["ui"] + len(banks["ui"])
    assert BASE["sprites"] + len(banks["sprites"]) <= 256

    # tile banks: 1 tile wide strips, so png2asset keeps the order
    for b, bank in banks.items():
        p = os.path.join(PNG, "%s.png" % b)
        indexed(list(bank.values()), 1).save(p)
        indexed(list(bank.values()), 16).save(os.path.join(PNG, "%s_sheet.png" % b))
        run([PNG2ASSET, p, "-o", os.path.join(OUT, "%s.c" % b), "-map", "-tiles_only",
             "-keep_duplicate_tiles", "-noflip", "-keep_palette_order", "-no_palettes"])
        got = decode_2bpp(c_bytes(os.path.join(OUT, "%s.c" % b), "%s_tiles" % b))
        assert got == list(bank.values()), "%s: png2asset bytes differ from the drawn tiles" % b
        print("%-8s %3d tiles at VRAM tile %3d" % (b, len(bank), BASE[b]))

    # the lacquer font (white on black) for names on the dashboard-style screens
    lq = A.lacquer_font_bank()
    p = os.path.join(PNG, "lacquer.png")
    indexed(list(lq.values()), 1).save(p)
    indexed(list(lq.values()), 16).save(os.path.join(PNG, "lacquer_sheet.png"))
    run([PNG2ASSET, p, "-o", os.path.join(OUT, "lacquer.c"), "-map", "-tiles_only",
         "-keep_duplicate_tiles", "-noflip", "-keep_palette_order", "-no_palettes"])
    assert decode_2bpp(c_bytes(os.path.join(OUT, "lacquer.c"), "lacquer_tiles")) == list(lq.values())
    print("lacquer  %3d tiles (loaded at tile %d on a win screen)" % (len(lq), wins.LACQUER_BASE))

    # full screens: png2asset dedupes them into a tileset and a 20x18 map
    sp = banks["sprites"]
    full = list(zip(screens_full.screens_full(sp, bake=False), ("dashboard", "dashboard_new", "lose")))
    full += [(s, "win_" + c.lower().replace(" ", "_")) for s, c in zip(wins.win_screens(bake=False), wins.CITIES)]
    for s, fname in full:
        p = os.path.join(PNG, fname + ".png")
        screen_png(s.bg).save(p)
        run([PNG2ASSET, p, "-o", os.path.join(OUT, fname + ".c"), "-map", "-noflip",
             "-keep_palette_order", "-no_palettes"])
        tiles = decode_2bpp(c_bytes(os.path.join(OUT, fname + ".c"), fname + "_tiles"))
        mp = c_bytes(os.path.join(OUT, fname + ".c"), fname + "_map")
        for ty in range(18):
            for tx in range(20):
                want = tuple(A.T if v is None else v for v in s.bg.tile(tx, ty))
                assert tiles[mp[ty * 20 + tx]] == want, (fname, tx, ty)
        if fname.startswith("win_"):
            assert len(tiles) <= wins.TILESET_MAX, (fname, len(tiles))
            assert wins.LACQUER_BASE + wins.LACQUER_WIN_COUNT <= wins.TRAIN_BASE
        if fname.startswith("dashboard"):
            assert len(tiles) <= travel.TILESET_MAX, (fname, len(tiles))
        print("%-14s %3d unique tiles, map 20x18 checked" % (fname, len(tiles)))

    # fireworks sprite tiles (a win screen loads them at wins.FW_BASE)
    fw = wins.firework_bank()
    p = os.path.join(PNG, "fireworks.png")
    indexed(list(fw.values()), 1).save(p)
    run([PNG2ASSET, p, "-o", os.path.join(OUT, "fireworks.c"), "-map", "-tiles_only",
         "-keep_duplicate_tiles", "-noflip", "-keep_palette_order", "-no_palettes"])
    assert decode_2bpp(c_bytes(os.path.join(OUT, "fireworks.c"), "fireworks_tiles")) == list(fw.values())
    tr = wins.train_bank()
    p = os.path.join(PNG, "train.png")
    indexed(list(tr.values()), 1).save(p)
    run([PNG2ASSET, p, "-o", os.path.join(OUT, "train.c"), "-map", "-tiles_only",
         "-keep_duplicate_tiles", "-noflip", "-keep_palette_order", "-no_palettes"])
    assert decode_2bpp(c_bytes(os.path.join(OUT, "train.c"), "train_tiles")) == list(tr.values())

    # the boarding-pass font (black on white) and the plane sprite, dashboard only
    pp = travel.paper_font_bank()
    extra = {"paper": pp, "plane": travel.plane_bank()}
    for name, bank in extra.items():
        p = os.path.join(PNG, name + ".png")
        indexed(list(bank.values()), 1).save(p)
        run([PNG2ASSET, p, "-o", os.path.join(OUT, name + ".c"), "-map", "-tiles_only",
             "-keep_duplicate_tiles", "-noflip", "-keep_palette_order", "-no_palettes"])
        assert decode_2bpp(c_bytes(os.path.join(OUT, name + ".c"), name + "_tiles")) == list(bank.values())

    # the shipped maps + the names the code prints == the mockups
    ui, lq = banks["ui"], A.lacquer_font_bank()
    for blank, baked in zip(screens_full.screens_full(sp, bake=False), screens_full.screens_full(sp)):
        cv = blank.bg
        if baked.name == "d1_dashboard":
            for i, ch in enumerate("SEOUL"):
                cv.set_tile(travel.TICKET_CITY_COL + i, travel.TICKET_ROW, pp["PP_" + ch])
        assert all(cv.tile(x, y) == baked.bg.tile(x, y) for y in range(18) for x in range(20)), baked.name
    for blank, baked, city in zip(wins.win_screens(bake=False), wins.win_screens(), wins.CITIES):
        cv = blank.bg
        for i, ch in enumerate(city):
            cv.set_tile((20 - len(city)) // 2 + i, wins.NAME_ROW, lq["LQ_" + ch])
        assert all(cv.tile(x, y) == baked.bg.tile(x, y) for y in range(18) for x in range(20)), baked.name
    print("printed names reproduce the mockups")

    # the tile index header
    h = ["/* GENERATED by gameboy/art/export.py - do not edit. */",
         "/* VRAM tile indices of every art tile (0x8000 addressing, LCDC bit 4 = 1). */",
         "#ifndef ART_TILES_H", "#define ART_TILES_H", "",
         "#define ART_BGP  0xE1u  /* index 0 table->light, 1 paper->white, 2->dark, 3->black */",
         "#define ART_OBP0 0xE1u  /* same mapping; sprite index 0 is transparent */",
         "#define ART_OBP1 0xD0u  /* flying club cards: index 1 white, 2 light grey (the seams), 3 black */", ""]
    for b, bank in banks.items():
        h.append("#define ART_%s_BASE %du" % (b.upper(), BASE[b]))
        h.append("#define ART_%s_COUNT %du" % (b.upper(), len(bank)))
    h.append("")
    for b, bank in banks.items():
        for k, name in enumerate(bank):
            h.append("#define T_%s %du" % (ident(name), BASE[b] + k))
        h.append("")
    h.append("/* the rail font: character c is at ART_UI_BASE + ART_FONT_FIRST + index of c in ART_FONT_CHARS */")
    h.append("#define ART_LACQUER_COUNT %du  /* lacquer.c: the same characters, white on black */" % len(lq))
    h.append("#define ART_LACQUER_BASE %du  /* where a win screen loads it (its tileset is at most this many tiles) */" % wins.LACQUER_BASE)
    h.append("#define ART_WIN_NAME_ROW %du  /* win screens: the bot's name, columns (20 - len) / 2 */" % wins.NAME_ROW)
    h.append("#define ART_FW_BASE %du  /* fireworks.c: the 4 firework sprite tiles on a win screen */" % wins.FW_BASE)
    for k, name in enumerate(fw):
        h.append("#define T_%s %du" % (name, wins.FW_BASE + k))
    h.append("#define ART_LACQUER_WIN_COUNT %du  /* a win screen loads only space and A..Z of lacquer.c */" % wins.LACQUER_WIN_COUNT)
    h.append("/* firework burst centres per win screen, screen pixels {cx, cy, ...}; see the README for the sprite placement */")
    for city, pts in wins.BURSTS.items():
        key = city.replace(" ", "_")
        h.append("#define ART_WIN_BURSTS_%s_N %du" % (key, len(pts)))
        h.append("#define ART_WIN_BURSTS_%s {%s}" % (key, ", ".join("%d, %d" % p for p in pts)))
    h.append("#define ART_TRAIN_BASE %du  /* train.c: the New York subway sprites */" % wins.TRAIN_BASE)
    for k, name in enumerate(tr):
        h.append("#define T_%s %du" % (name, wins.TRAIN_BASE + k))
    h.append("#define ART_DASH_CITY_ROW %du  /* dashboard: the CONTINUE city on the boarding pass, paper font */" % travel.TICKET_ROW)
    h.append("#define ART_DASH_CITY_COL %du" % travel.TICKET_CITY_COL)
    h.append("#define ART_PAPER_BASE %du  /* paper.c on the dashboard (its tileset is at most this many tiles) */" % travel.PAPER_BASE)
    h.append("#define ART_PAPER_COUNT %du" % len(pp))
    h.append("#define ART_DASH_POINTER_TILE %du  /* copy SP_POINTER here on the dashboard */" % travel.POINTER_TILE)
    h.append("#define ART_DASH_PLANE_TILE %du  /* plane.c: the plane on the flight route */" % travel.PLANE_TILE)
    h.append("#define ART_DASH_ROUTE_Y %du  /* the plane's centre y; x per city: see travel.CITY_X */" % travel.ROUTE_Y)
    h.append('#define ART_FONT_CHARS "%s"' % A.FONT_CHARS.replace("'", "\\'"))
    h.append("#define ART_FONT_FIRST %du" % list(banks["ui"]).index("CH_ "))
    h += ["", "#endif", ""]
    open(os.path.join(OUT, "art_tiles.h"), "w").write("\n".join(h))
    print("wrote", os.path.join(OUT, "art_tiles.h"))
    write_check_screens(banks)


def write_check_screens(banks):
    """The table mockups as data for the check ROM: a 20x18 map of VRAM tile
    indices and a sprite list (x, y, tile, flags) per screen."""
    lookup = {}
    for b, bank in banks.items():
        for k, t in enumerate(bank.values()):
            lookup.setdefault(t, BASE[b] + k)
    ui, sp = banks["ui"], banks["sprites"]
    scrs = A.screens_table(ui, sp)       # the motion frames are on the mockup sheets; the ROM has 32 KB
    c = ["/* GENERATED by gameboy/art/export.py - do not edit. */",
         "/* The art mockups as VRAM maps + sprite lists, for check.c. */",
         "#include <stdint.h>", '#include "check_screens.h"', ""]
    hdr = ["/* GENERATED by gameboy/art/export.py - do not edit. */",
           "#ifndef CHECK_SCREENS_H", "#define CHECK_SCREENS_H", "#include <stdint.h>",
           "typedef struct { uint8_t x, y, tile, flags; } check_sprite;",
           "typedef struct { const uint8_t *map; const check_sprite *spr; uint8_t nspr; } check_screen;",
           "#define CHECK_SCREENS %d" % len(scrs),
           "extern const check_screen check_screens[CHECK_SCREENS];", "#endif", ""]
    rows = []
    for s in scrs:
        mp = [lookup[s.bg.tile(tx, ty)] for ty in range(18) for tx in range(20)]
        c.append("static const uint8_t %s_map[360] = {" % s.name)
        for k in range(0, 360, 20):
            c.append("    " + ",".join("%d" % v for v in mp[k:k + 20]) + ",")
        c.append("};")
        sprs = []
        for (x, y, t) in s.sprites:
            for fx in (0, 1):
                for fy in (0, 1):
                    f = A.flip(t, fx, fy)
                    if f in lookup:
                        sprs.append((x, y, lookup[f], (0x20 if fx else 0) | (0x40 if fy else 0)))
                        break
                else:
                    continue
                break
        assert len(sprs) == len(s.sprites), s.name
        c.append("static const check_sprite %s_spr[%d] = {" % (s.name, max(1, len(sprs))))
        for (x, y, t, f) in sprs or [(0, 0, 0, 0)]:
            c.append("    {%d, %d, %d, 0x%02x}," % (x + 8, y + 16, t, f))
        c.append("};")
        rows.append("    {%s_map, %s_spr, %d}," % (s.name, s.name, len(sprs)))
    c.append("const check_screen check_screens[CHECK_SCREENS] = {")
    c += rows
    c += ["};", ""]
    d = os.path.join(OUT, "check")
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "check_screens.c"), "w").write("\n".join(c))
    open(os.path.join(d, "check_screens.h"), "w").write("\n".join(hdr))
    print("wrote check screens:", len(scrs))


if __name__ == "__main__":
    main()
