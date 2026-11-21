// gbrun - a headless Game Boy runner over libmgba.
//
//   gbrun ROM SCRIPT [SYMBOLS.noi]
//
// Loads the ROM as a DMG, runs it frame by frame, holds buttons on the frames
// the script says, writes PNG screenshots and reads memory on set frames. The
// script is plain text, one command per line:
//
//   # a comment
//   @N   press KEYS [FRAMES]    hold KEYS (A, B, SELECT, START, UP, DOWN, LEFT,
//                               RIGHT, joined by +) for FRAMES frames (default 2)
//   +N   shot PATH [SCALE]      write the screen after frame N as a PNG, SCALE
//                               times larger (nearest neighbour, default 1)
//   +N   peek SYM [LEN]         print LEN (default 1) bytes at a symbol or 0xADDR
//   +N   expect SYM HEX...      fail (exit 1) unless those bytes are there
//   +N   end                    stop after this frame
//   repeat COUNT ... endrepeat  the lines between, COUNT times (may nest)
//
// "@N" is frame N; "+N" is N frames after the previous command's frame. So a
// script reads as a sequence of steps. A symbol is looked up in the .noi file
// the linker writes (lines "DEF _name 0xADDR").
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba-util/png-io.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EV_PRESS, EV_SHOT, EV_PEEK, EV_EXPECT, EV_END };

typedef struct {
    int frame, kind, len, order;
    uint32_t keys;
    char arg[256];
    unsigned char bytes[16];
    int nbytes;
} Ev;

static Ev *evs;
static int nev, capev;

static Ev *ev_new(void) {
    if (nev == capev) {
        capev = capev ? capev * 2 : 256;
        evs = realloc(evs, sizeof(Ev) * (size_t)capev);
        if (!evs) { perror("realloc"); exit(2); }
    }
    memset(&evs[nev], 0, sizeof(Ev));
    evs[nev].order = nev;
    return &evs[nev++];
}

static uint32_t parse_keys(const char *s, int line) {
    static const char *names[8] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN" };
    uint32_t k = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+")) {
        int found = 0;
        for (int i = 0; i < 8; i++) if (!strcmp(t, names[i])) { k |= 1u << i; found = 1; }
        if (!found) { fprintf(stderr, "script line %d: unknown key %s\n", line, t); exit(2); }
    }
    return k;
}

// ---- the script, with repeats expanded ------------------------------------------
static char **lines;
static int nlines;

static int run_lines(int from, int to, int *now, int depth);

static int parse_line(int i, int *now) {
    char *l = lines[i], cmd[32] = "", a1[256] = "", a2[64] = "";
    int t;
    while (*l == ' ' || *l == '\t') l++;
    if (!*l || *l == '#' || *l == '\n') return i + 1;
    if (*l == '@') t = atoi(l + 1);
    else if (*l == '+') t = *now + atoi(l + 1);
    else { fprintf(stderr, "script line %d: a step starts with @N or +N\n", i + 1); exit(2); }
    *now = t;
    while (*l && *l != ' ' && *l != '\t') l++;
    int n = sscanf(l, "%31s %255s %63s", cmd, a1, a2);
    Ev *e = ev_new();
    e->frame = t;
    if (!strcmp(cmd, "press") && n >= 2) {
        e->kind = EV_PRESS;
        e->keys = parse_keys(a1, i + 1);
        e->len = n >= 3 ? atoi(a2) : 2;
    } else if (!strcmp(cmd, "shot") && n >= 2) {
        e->kind = EV_SHOT;
        snprintf(e->arg, sizeof e->arg, "%s", a1);
        e->len = n >= 3 ? atoi(a2) : 1;
        if (e->len < 1 || e->len > 8) { fprintf(stderr, "script line %d: scale 1..8\n", i + 1); exit(2); }
    } else if (!strcmp(cmd, "peek") && n >= 2) {
        e->kind = EV_PEEK;
        snprintf(e->arg, sizeof e->arg, "%s", a1);
        e->len = n >= 3 ? atoi(a2) : 1;
    } else if (!strcmp(cmd, "expect") && n >= 3) {
        e->kind = EV_EXPECT;
        snprintf(e->arg, sizeof e->arg, "%s", a1);
        char *p = strstr(l, a1) + strlen(a1);
        unsigned v;
        int used;
        while (e->nbytes < 16 && sscanf(p, " %x%n", &v, &used) == 1) { e->bytes[e->nbytes++] = (unsigned char)v; p += used; }
        e->len = e->nbytes;
    } else if (!strcmp(cmd, "end")) {
        e->kind = EV_END;
    } else {
        fprintf(stderr, "script line %d: unknown command '%s'\n", i + 1, cmd);
        exit(2);
    }
    return i + 1;
}

// Lines [from, to): expands "repeat N ... endrepeat".
static int run_lines(int from, int to, int *now, int depth) {
    int i = from;
    while (i < to) {
        char *l = lines[i];
        while (*l == ' ' || *l == '\t') l++;
        if (!strncmp(l, "repeat", 6)) {
            int count = atoi(l + 6), d = 1, j = i + 1;
            for (; j < to && d; j++) {
                char *m = lines[j];
                while (*m == ' ' || *m == '\t') m++;
                if (!strncmp(m, "repeat", 6)) d++;
                else if (!strncmp(m, "endrepeat", 9)) d--;
            }
            if (d) { fprintf(stderr, "script line %d: repeat without endrepeat\n", i + 1); exit(2); }
            for (int k = 0; k < count; k++) run_lines(i + 1, j - 1, now, depth + 1);
            i = j;
        } else if (!strncmp(l, "endrepeat", 9)) {
            fprintf(stderr, "script line %d: endrepeat without repeat\n", i + 1);
            exit(2);
        } else {
            i = parse_line(i, now);
        }
    }
    return i;
}

static int ev_cmp(const void *a, const void *b) {
    const Ev *x = a, *y = b;
    if (x->frame != y->frame) return x->frame < y->frame ? -1 : 1;
    return x->order - y->order;
}

// ---- symbols from the linker's .noi ------------------------------------------------
static char syms[4096][48];
static uint32_t symaddr[4096];
static int nsyms;

static void load_noi(const char *path) {
    FILE *f = fopen(path, "r");
    char name[64];
    unsigned addr;
    char line[256];
    if (!f) { perror(path); exit(2); }
    while (fgets(line, sizeof line, f) && nsyms < 4096)
        if (sscanf(line, "DEF %63s 0x%x", name, &addr) == 2) {
            snprintf(syms[nsyms], sizeof syms[0], "%s", name);
            symaddr[nsyms++] = addr;
        }
    fclose(f);
}

static uint32_t resolve(const char *s) {
    if (!strncmp(s, "0x", 2)) return (uint32_t)strtoul(s, 0, 16);
    for (int i = 0; i < nsyms; i++) if (!strcmp(syms[i], s)) return symaddr[i];
    fprintf(stderr, "unknown symbol %s (pass the .noi file)\n", s);
    exit(2);
}

// The buffer is laid out for the largest picture the core can make (a Super Game
// Boy border, 256x224); a DMG screen is its top-left 160x144.
#define GB_W 160
#define GB_H 144

static void write_png(const char *path, const color_t *src, unsigned stride, int scale) {
    static color_t px[GB_W * 8 * GB_H * 8];
    for (unsigned y = 0; y < GB_H * (unsigned)scale; y++)
        for (unsigned x = 0; x < GB_W * (unsigned)scale; x++)
            px[y * GB_W * (unsigned)scale + x] = src[(y / (unsigned)scale) * stride + x / (unsigned)scale];
    stride = GB_W * (unsigned)scale;
    struct VFile *vf = VFileOpen(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!vf) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    png_structp png = PNGWriteOpen(vf);
    png_infop info = PNGWriteHeader(png, GB_W * (unsigned)scale, GB_H * (unsigned)scale);
    if (!PNGWritePixels(png, GB_W * (unsigned)scale, GB_H * (unsigned)scale, stride, px)) { fprintf(stderr, "png write failed: %s\n", path); exit(2); }
    PNGWriteClose(png, info);
    vf->close(vf);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: gbrun ROM SCRIPT [SYMBOLS.noi]\n"); return 2; }
    FILE *sf = fopen(argv[2], "r");
    if (!sf) { perror(argv[2]); return 2; }
    char buf[512];
    while (fgets(buf, sizeof buf, sf)) {
        lines = realloc(lines, sizeof(char *) * (size_t)(nlines + 1));
        lines[nlines++] = strdup(buf);
    }
    fclose(sf);
    if (argc > 3) load_noi(argv[3]);
    int now = 0;
    run_lines(0, nlines, &now, 0);
    qsort(evs, (size_t)nev, sizeof(Ev), ev_cmp);
    int last = 0;
    for (int i = 0; i < nev; i++) {
        int e = evs[i].frame + (evs[i].kind == EV_PRESS ? evs[i].len : 0);
        if (e > last) last = e;
        if (evs[i].kind == EV_END) { last = evs[i].frame; break; }
    }

    struct mCore *core = mCoreFind(argv[1]);
    if (!core || !core->init(core)) { fprintf(stderr, "no core for %s\n", argv[1]); return 2; }
    mCoreInitConfig(core, NULL);
    mCoreConfigSetValue(&core->config, "gb.model", "DMG");
    mCoreConfigSetValue(&core->config, "useBios", "0");
    mCoreLoadConfig(core);
    unsigned w, h;
    core->desiredVideoDimensions(core, &w, &h);
    color_t *video = calloc((size_t)w * h, sizeof(color_t));
    core->setVideoBuffer(core, video, w);
    if (!mCoreLoadFile(core, argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }
    core->reset(core);

    int fail = 0, ei = 0;
    for (int frame = 0; frame <= last; frame++) {
        uint32_t keys = 0;
        for (int i = 0; i < nev; i++)
            if (evs[i].kind == EV_PRESS && frame >= evs[i].frame && frame < evs[i].frame + evs[i].len) keys |= evs[i].keys;
        core->setKeys(core, keys);
        core->runFrame(core);
        // Commands at frame N see the screen after frame N ran.
        while (ei < nev && evs[ei].frame <= frame) {
            Ev *e = &evs[ei++];
            if (e->kind == EV_SHOT) {
                write_png(e->arg, video, w, e->len);
                printf("frame %d: shot %s\n", frame, e->arg);
            } else if (e->kind == EV_PEEK || e->kind == EV_EXPECT) {
                uint32_t a = resolve(e->arg);
                int ok = 1;
                printf("frame %d: %s %s =", frame, e->kind == EV_PEEK ? "peek" : "expect", e->arg);
                for (int k = 0; k < e->len; k++) {
                    unsigned v = core->busRead8(core, a + (uint32_t)k) & 0xFF;
                    printf(" %02x", v);
                    if (e->kind == EV_EXPECT && v != e->bytes[k]) ok = 0;
                }
                if (e->kind == EV_EXPECT) printf(ok ? "  ok\n" : "  FAILED\n"); else printf("\n");
                if (!ok) fail = 1;
            }
        }
    }
    core->deinit(core);
    free(video);
    return fail;
}
