// gbrun - a headless Game Boy runner over libmgba.
//
//   gbrun ROM SCRIPT [SYMBOLS.noi]
//
// Loads the ROM as a DMG and plays a script against it, frame by frame: holds
// buttons, writes PNG screenshots, reads, checks and writes memory, and waits
// for memory to reach a value. The script is plain text, one step per line,
// run in order:
//
//   # a comment
//   @N   press KEYS [FRAMES]    hold KEYS (A, B, SELECT, START, UP, DOWN, LEFT,
//                               RIGHT, joined by +) from frame N for FRAMES
//                               frames (default 2)
//   +N   shot PATH [SCALE]      write the screen as a PNG, SCALE times larger
//                               (nearest neighbour, default 1)
//   +N   peek SYM [LEN]         print LEN (default 1) bytes at a symbol or 0xADDR,
//                               either one optionally +OFFSET ("_G+74")
//   +N   expect SYM HEX...      fail (exit 1) unless those bytes are there
//   +N   poke SYM HEX...        write those bytes there
//   +N   until SYM HEX...       run on, a frame at a time, until those bytes are
//                               there (fail after UNTIL_MAX frames); the next
//                               "+N" counts from the frame it stopped on
//   +N   pixel X Y SHADE        fail unless the screen pixel at (X, Y) is that
//                               shade: 0 white, 1 light grey, 2 dark grey, 3 black
//   +N   pixelspr N DX DY SHADE the same, at (DX, DY) from sprite N's top left as
//                               the hardware OAM places it (what the frame drew)
//   +N   notboth F X1 Y1 S1 X2 Y2 S2
//                               run on F frames, failing if any of them shows
//                               shade S1 at (X1, Y1) and S2 at (X2, Y2) at once
//                               (two states that must never share a frame); the
//                               next "+N" counts from the last of them
//   +N   atleast SYM LEN MIN    fail unless the LEN-byte little-endian number at the
//                               symbol is at least MIN (a headroom, a count)
//   +N   same PATH              fail unless every screen pixel is the shade of the
//                               same pixel of the PNG at PATH (a shot, any SCALE)
//   +N   end                    stop
//   repeat COUNT ... endrepeat  the lines between, COUNT times (may nest)
//
// "@N" is frame N; "+N" is N frames after the previous step's frame. Every
// step but press acts after its frame ran; a press holds its keys while its
// first frame runs. So a press and a check on the same frame see the press.
// A symbol is looked up in the .noi file the linker writes ("DEF _name 0xADDR").
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba-util/png-io.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNTIL_MAX 200000      // frames an until waits; GBRUN_UNTIL_MAX overrides (slow stress runs)
static long until_max = UNTIL_MAX;

enum { ST_PRESS, ST_SHOT, ST_PEEK, ST_EXPECT, ST_POKE, ST_UNTIL, ST_PIXEL, ST_PIXSPR, ST_NOTBOTH, ST_ATLEAST, ST_SAME, ST_END };

typedef struct {
    int absolute, n;        // @n or +n
    int kind, len, line;
    uint32_t keys;
    char arg[256];
    unsigned char bytes[16];
    int nbytes;
} Step;

static Step *steps;
static int nsteps, capsteps;

static Step *step_new(void) {
    if (nsteps == capsteps) {
        capsteps = capsteps ? capsteps * 2 : 256;
        steps = realloc(steps, sizeof(Step) * (size_t)capsteps);
        if (!steps) { perror("realloc"); exit(2); }
    }
    memset(&steps[nsteps], 0, sizeof(Step));
    return &steps[nsteps++];
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

// ---- the script, with repeats expanded --------------------------------------------
static char **lines;
static int nlines;

static void parse_line(int i) {
    char *l = lines[i], cmd[32] = "", a1[256] = "", a2[64] = "";
    while (*l == ' ' || *l == '\t') l++;
    if (!*l || *l == '#' || *l == '\n') return;
    Step *s = step_new();
    s->line = i + 1;
    if (*l == '@') s->absolute = 1;
    else if (*l != '+') { fprintf(stderr, "script line %d: a step starts with @N or +N\n", i + 1); exit(2); }
    s->n = atoi(l + 1);
    while (*l && *l != ' ' && *l != '\t') l++;
    int n = sscanf(l, "%31s %255s %63s", cmd, a1, a2);
    if (!strcmp(cmd, "press") && n >= 2) {
        s->kind = ST_PRESS;
        s->keys = parse_keys(a1, i + 1);
        s->len = n >= 3 ? atoi(a2) : 2;
    } else if (!strcmp(cmd, "shot") && n >= 2) {
        s->kind = ST_SHOT;
        snprintf(s->arg, sizeof s->arg, "%s", a1);
        s->len = n >= 3 ? atoi(a2) : 1;
        if (s->len < 1 || s->len > 8) { fprintf(stderr, "script line %d: scale 1..8\n", i + 1); exit(2); }
    } else if (!strcmp(cmd, "peek") && n >= 2) {
        s->kind = ST_PEEK;
        snprintf(s->arg, sizeof s->arg, "%s", a1);
        s->len = n >= 3 ? atoi(a2) : 1;
    } else if ((!strcmp(cmd, "expect") || !strcmp(cmd, "poke") || !strcmp(cmd, "until")) && n >= 3) {
        s->kind = !strcmp(cmd, "poke") ? ST_POKE : !strcmp(cmd, "until") ? ST_UNTIL : ST_EXPECT;
        snprintf(s->arg, sizeof s->arg, "%s", a1);
        char *p = strstr(l, a1) + strlen(a1);
        unsigned v;
        int used;
        while (s->nbytes < 16 && sscanf(p, " %x%n", &v, &used) == 1) { s->bytes[s->nbytes++] = (unsigned char)v; p += used; }
        s->len = s->nbytes;
    } else if (!strcmp(cmd, "pixel") && n >= 3) {
        unsigned x, y, shade;
        s->kind = ST_PIXEL;
        if (sscanf(l, "%*s %u %u %u", &x, &y, &shade) != 3 || x >= 160 || y >= 144 || shade > 3) {
            fprintf(stderr, "script line %d: pixel X Y SHADE (0..159, 0..143, 0..3)\n", i + 1);
            exit(2);
        }
        s->bytes[0] = (unsigned char)x; s->bytes[1] = (unsigned char)y; s->bytes[2] = (unsigned char)shade;
        snprintf(s->arg, sizeof s->arg, "%u %u", x, y);
    } else if (!strcmp(cmd, "pixelspr") && n >= 3) {
        unsigned sp, dx, dy, shade;
        s->kind = ST_PIXSPR;
        if (sscanf(l, "%*s %u %u %u %u", &sp, &dx, &dy, &shade) != 4 || sp >= 40 || dx >= 16 || dy >= 32 || shade > 3) {
            fprintf(stderr, "script line %d: pixelspr N DX DY SHADE (0..39, 0..15, 0..31, 0..3)\n", i + 1);
            exit(2);
        }
        s->bytes[0] = (unsigned char)sp; s->bytes[1] = (unsigned char)dx; s->bytes[2] = (unsigned char)dy; s->bytes[3] = (unsigned char)shade;
        snprintf(s->arg, sizeof s->arg, "%u +%u +%u", sp, dx, dy);
    } else if (!strcmp(cmd, "notboth") && n >= 3) {
        unsigned v[7];
        s->kind = ST_NOTBOTH;
        if (sscanf(l, "%*s %u %u %u %u %u %u %u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7 || !v[0] || v[0] > 255
            || v[1] >= 160 || v[2] >= 144 || v[3] > 3 || v[4] >= 160 || v[5] >= 144 || v[6] > 3) {
            fprintf(stderr, "script line %d: notboth FRAMES X1 Y1 S1 X2 Y2 S2\n", i + 1);
            exit(2);
        }
        for (int k = 0; k < 7; k++) s->bytes[k] = (unsigned char)v[k];
        snprintf(s->arg, sizeof s->arg, "(%u, %u) = %u and (%u, %u) = %u", v[1], v[2], v[3], v[4], v[5], v[6]);
    } else if (!strcmp(cmd, "atleast") && n >= 2) {
        unsigned len, min;
        s->kind = ST_ATLEAST;
        if (sscanf(l, "%*s %*s %u %u", &len, &min) != 2 || len < 1 || len > 4) {
            fprintf(stderr, "script line %d: atleast SYM LEN(1..4) MIN\n", i + 1);
            exit(2);
        }
        snprintf(s->arg, sizeof s->arg, "%s", a1);
        s->len = (int)len;
        s->keys = min;
    } else if (!strcmp(cmd, "same") && n >= 2) {
        s->kind = ST_SAME;
        snprintf(s->arg, sizeof s->arg, "%s", a1);
    } else if (!strcmp(cmd, "end")) {
        s->kind = ST_END;
    } else {
        fprintf(stderr, "script line %d: unknown command '%s'\n", i + 1, cmd);
        exit(2);
    }
}

static void expand(int from, int to) {
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
            for (int k = 0; k < count; k++) expand(i + 1, j - 1);
            i = j;
        } else if (!strncmp(l, "endrepeat", 9)) {
            fprintf(stderr, "script line %d: endrepeat without repeat\n", i + 1);
            exit(2);
        } else {
            parse_line(i++);
        }
    }
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

// A symbol or 0xADDR, optionally +N (decimal): "_G+74".
static uint32_t resolve(const char *arg) {
    char s[256];
    uint32_t off = 0;
    snprintf(s, sizeof s, "%s", arg);
    char *plus = strchr(s, '+');
    if (plus) { *plus = 0; off = (uint32_t)strtoul(plus + 1, 0, 10); }
    if (!strncmp(s, "0x", 2)) return (uint32_t)strtoul(s, 0, 16) + off;
    for (int i = 0; i < nsyms; i++) if (!strcmp(syms[i], s)) return symaddr[i] + off;
    fprintf(stderr, "unknown symbol %s (pass the .noi file)\n", arg);
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
    struct VFile *vf = VFileOpen(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!vf) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    png_structp png = PNGWriteOpen(vf);
    png_infop info = PNGWriteHeader(png, GB_W * (unsigned)scale, GB_H * (unsigned)scale);
    if (!PNGWritePixels(png, GB_W * (unsigned)scale, GB_H * (unsigned)scale, GB_W * (unsigned)scale, px)) {
        fprintf(stderr, "png write failed: %s\n", path);
        exit(2);
    }
    PNGWriteClose(png, info);
    vf->close(vf);
}

// The emulator's own log goes to stderr, so a script's output is only its own
// lines (mGBA notes, for one, that GBDK's start-up writes an MBC1 RAM bank on a
// cart with no RAM).
static void log_stderr(struct mLogger *l, int cat, enum mLogLevel level, const char *fmt, va_list ap) {
    (void)l; (void)cat;
    if (level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN)) { vfprintf(stderr, fmt, ap); fputc('\n', stderr); }
}
static struct mLogger logger = { .log = log_stderr };

// ---- running ----------------------------------------------------------------------------
static struct mCore *core;
static color_t *video;
static unsigned stride;
static int frame_next;                  // the next frame to run
static struct { int from, to; uint32_t keys; } held[256];
static int nheld;

static void run_frame(void) {
    uint32_t keys = 0;
    for (int i = 0; i < nheld; i++)
        if (frame_next >= held[i].from && frame_next < held[i].to) keys |= held[i].keys;
    core->setKeys(core, keys);
    core->runFrame(core);
    frame_next++;
    // forget presses that are over
    int k = 0;
    for (int i = 0; i < nheld; i++) if (held[i].to > frame_next) held[k++] = held[i];
    nheld = k;
}

static int mem_is(uint32_t a, const unsigned char *b, int n) {
    for (int k = 0; k < n; k++) if ((core->busRead8(core, a + (uint32_t)k) & 0xFF) != b[k]) return 0;
    return 1;
}

// Print a step's bytes; ok < 0 for a plain peek.
// A screen pixel as a DMG shade (0 white .. 3 black), by its brightness: the
// core's DMG palette is four evenly spaced greys.
static int shade_at(unsigned x, unsigned y) {
    const color_t c = video[y * stride + x];
    unsigned r, g, b;
    if (sizeof(color_t) == 2) { r = M_R8(c); g = M_G8(c); b = M_B8(c); }
    else { r = (unsigned)c & 0xFF; g = ((unsigned)c >> 8) & 0xFF; b = ((unsigned)c >> 16) & 0xFF; }
    const unsigned lum = (r + g + b) / 3;
    return lum >= 192 ? 0 : lum >= 128 ? 1 : lum >= 64 ? 2 : 3;
}

// The whole screen against a PNG shot of it (scaled by any whole factor), shade
// by shade: the number of pixels that differ, and the first, in reading order.
static int same_as(const char *path, int *fx, int *fy) {
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path)) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    img.format = PNG_FORMAT_RGB;
    const unsigned sc = img.width / GB_W;
    if (!sc || img.width != GB_W * sc || img.height != GB_H * sc) { fprintf(stderr, "%s is not a screen shot\n", path); exit(2); }
    unsigned char *px = malloc(PNG_IMAGE_SIZE(img));
    if (!px || !png_image_finish_read(&img, NULL, px, 0, NULL)) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    int diff = 0;
    *fx = *fy = -1;
    for (unsigned y = 0; y < GB_H; y++)
        for (unsigned x = 0; x < GB_W; x++) {
            const unsigned char *p = px + 3u * ((y * sc) * img.width + x * sc);
            const unsigned lum = (p[0] + p[1] + p[2]) / 3u;
            const int want = lum >= 192 ? 0 : lum >= 128 ? 1 : lum >= 64 ? 2 : 3;
            if (shade_at(x, y) != want) { if (!diff) { *fx = (int)x; *fy = (int)y; } diff++; }
        }
    free(px);
    return diff;
}

static int show(const char *what, int frame, const Step *s, int ok) {
    uint32_t a = resolve(s->arg);
    printf("frame %d: %s %s =", frame, what, s->arg);
    for (int k = 0; k < s->len; k++) printf(" %02x", core->busRead8(core, a + (uint32_t)k) & 0xFF);
    if (ok < 0) printf("\n"); else printf(ok ? "  ok\n" : "  FAILED\n");
    return ok;
}

int main(int argc, char **argv) {
    mLogSetDefaultLogger(&logger);
    if (getenv("GBRUN_UNTIL_MAX")) until_max = strtol(getenv("GBRUN_UNTIL_MAX"), 0, 10);
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
    expand(0, nlines);

    core = mCoreFind(argv[1]);
    if (!core || !core->init(core)) { fprintf(stderr, "no core for %s\n", argv[1]); return 2; }
    mCoreInitConfig(core, NULL);
    mCoreConfigSetValue(&core->config, "gb.model", "DMG");
    mCoreConfigSetValue(&core->config, "useBios", "0");
    mCoreLoadConfig(core);
    unsigned w, h;
    core->desiredVideoDimensions(core, &w, &h);
    video = calloc((size_t)w * h, sizeof(color_t));
    stride = w;
    core->setVideoBuffer(core, video, w);
    if (!mCoreLoadFile(core, argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }
    core->reset(core);

    int fail = 0, now = 0, last_press_end = 0;
    for (int i = 0; i < nsteps; i++) {
        const Step *s = &steps[i];
        const int f = s->absolute ? s->n : now + s->n;
        // a press may start on the frame about to run; anything else may act
        // on the frame that just ran, but not earlier
        if (f < frame_next - (s->kind == ST_PRESS ? 0 : 1)) {
            fprintf(stderr, "script line %d: frame %d is in the past (at %d)\n", s->line, f, frame_next - 1);
            return 2;
        }
        now = f;
        while (frame_next < f) run_frame();
        if (s->kind == ST_PRESS) {
            if (nheld == 256) { fprintf(stderr, "too many presses at once\n"); return 2; }
            held[nheld].from = f; held[nheld].to = f + s->len; held[nheld].keys = s->keys;
            nheld++;
            if (f + s->len > last_press_end) last_press_end = f + s->len;
            continue;
        }
        if (frame_next == f) run_frame();          // the step acts after its frame ran
        if (s->kind == ST_END) break;
        if (s->kind == ST_SHOT) {
            write_png(s->arg, video, stride, s->len);
            printf("frame %d: shot %s\n", f, s->arg);
        } else if (s->kind == ST_PIXEL) {
            const int got = shade_at(s->bytes[0], s->bytes[1]);
            const int ok = got == s->bytes[2];
            printf("frame %d: pixel %s = %d%s\n", f, s->arg, got, ok ? "  ok" : "  FAILED");
            if (!ok) fail = 1;
        } else if (s->kind == ST_NOTBOTH) {
            int bad = -1;
            for (int k = 0; k < s->bytes[0]; k++) {
                if (k) run_frame();
                if (shade_at(s->bytes[1], s->bytes[2]) == s->bytes[3] && shade_at(s->bytes[4], s->bytes[5]) == s->bytes[6]) {
                    bad = frame_next - 1;
                    break;
                }
            }
            now = frame_next - 1;
            if (bad >= 0) { printf("frame %d: notboth %s  FAILED (both shown)\n", bad, s->arg); fail = 1; }
            else printf("frame %d: notboth %s over %d frames  ok\n", now, s->arg, s->bytes[0]);
        } else if (s->kind == ST_SAME) {
            int x, y;
            const int d = same_as(s->arg, &x, &y);
            if (d) { printf("frame %d: same %s  FAILED (%d pixels differ, the first at (%d, %d))\n", f, s->arg, d, x, y); fail = 1; }
            else printf("frame %d: same %s  ok\n", f, s->arg);
        } else if (s->kind == ST_PIXSPR) {
            const uint32_t oam = 0xFE00u + 4u * s->bytes[0];
            const int sy = (int)(core->busRead8(core, oam) & 0xFF) - 16, sx = (int)(core->busRead8(core, oam + 1) & 0xFF) - 8;
            const int x = sx + s->bytes[1], y = sy + s->bytes[2];
            const int ok = x >= 0 && x < 160 && y >= 0 && y < 144;
            const int got = ok ? shade_at((unsigned)x, (unsigned)y) : -1;
            const int pass = ok && got == s->bytes[3];
            printf("frame %d: pixelspr %s at (%d, %d) = %d%s\n", f, s->arg, x, y, got, pass ? "  ok" : "  FAILED");
            if (!pass) fail = 1;
        } else if (s->kind == ST_PEEK) {
            show("peek", f, s, -1);
        } else if (s->kind == ST_EXPECT) {
            if (!show("expect", f, s, mem_is(resolve(s->arg), s->bytes, s->len))) fail = 1;
        } else if (s->kind == ST_ATLEAST) {
            const uint32_t a = resolve(s->arg);
            uint32_t v = 0;
            for (int k = s->len - 1; k >= 0; k--) v = v << 8 | (core->busRead8(core, a + (uint32_t)k) & 0xFFu);
            printf("frame %d: atleast %s = %u (at least %u)%s\n", f, s->arg, (unsigned)v, (unsigned)s->keys, v >= s->keys ? "  ok" : "  FAILED");
            if (v < s->keys) fail = 1;
        } else if (s->kind == ST_POKE) {
            uint32_t a = resolve(s->arg);
            for (int k = 0; k < s->len; k++) core->busWrite8(core, a + (uint32_t)k, s->bytes[k]);
        } else if (s->kind == ST_UNTIL) {
            const uint32_t a = resolve(s->arg);
            int waited = 0;
            while (!mem_is(a, s->bytes, s->len) && waited < until_max) { run_frame(); waited++; }
            now = frame_next - 1;
            if (!show("until", now, s, mem_is(a, s->bytes, s->len))) { fail = 1; break; }
        }
    }
    // let the last press finish
    while (frame_next < last_press_end) run_frame();
    core->deinit(core);
    free(video);
    return fail;
}
