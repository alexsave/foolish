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
//   +N   audio start            start keeping the sound (mGBA's audio, 48 kHz,
//                               left and right mixed to mono) and HRAM as it
//                               was at the end of every frame
//   +N   audio save PATH        write the sound kept as a 16-bit WAV
//   +N   mark [SYM K]           this step's frame is the mark (frame 0 below);
//                               with SYM, run on to the frame in which the
//                               16-bit sys_time reaches the 16-bit value at SYM
//                               plus K, and mark that one (a flight's landing:
//                               mark _fly_t0 12); if it already has, the first
//                               frame since audio start that had
//   +N   onsets A B GATE T...   find the onsets in the sound kept from frame A
//                               to frame B (from the mark; run on to B first):
//                               a 1 ms window 6 dB over the quietest of the
//                               20 ms before it, within GATE dB of the loudest
//                               window from A to B, over -60 dBFS, 40 ms after
//                               the last; fail unless there is exactly one for
//                               each T, at frame T or up to 2 frames later (a
//                               sound may trail its picture, never lead it)
//   +N   changes A B SYM T...   the frames from A to B (from the mark, B
//                               included) in which the HRAM byte SYM went up,
//                               once for each step it went up (a counter): fail
//                               unless there is exactly one for each T, at
//                               frame T or up to 2 frames later
//   +N   end                    stop
//   repeat COUNT ... endrepeat  the lines between, COUNT times (may nest)
//
// "@N" is frame N; "+N" is N frames after the previous step's frame. Every
// step but press acts after its frame ran; a press holds its keys while its
// first frame runs. So a press and a check on the same frame see the press.
// A symbol is looked up in the .noi file the linker writes ("DEF _name 0xADDR").
#define _DEFAULT_SOURCE 1      // strdup and PATH_MAX (libmgba's headers) under -std=c11 on glibc
#include <limits.h>
#include <mgba/core/core.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba-util/png-io.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNTIL_MAX 200000

enum { ST_PRESS, ST_SHOT, ST_PEEK, ST_EXPECT, ST_POKE, ST_UNTIL, ST_PIXEL, ST_PIXSPR, ST_NOTBOTH, ST_END,
       ST_AUDIO_START, ST_AUDIO_SAVE, ST_ONSETS, ST_CHANGES, ST_MARK };

typedef struct {
    int absolute, n;        // @n or +n
    int kind, len, line;
    uint32_t keys;
    char arg[256];
    unsigned char bytes[16];
    int nbytes;
    double t[16];           // onsets: the frames expected
    int nt;
    double gate;            // onsets: dB under the loudest window that still counts
    int from;               // onsets, changes: the first frame, from the mark (len: the last)
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
    } else if (!strcmp(cmd, "audio") && n >= 2 && !strcmp(a1, "start")) {
        s->kind = ST_AUDIO_START;
    } else if (!strcmp(cmd, "audio") && n >= 3 && !strcmp(a1, "save")) {
        s->kind = ST_AUDIO_SAVE;
        snprintf(s->arg, sizeof s->arg, "%s", a2);
    } else if (!strcmp(cmd, "mark")) {
        s->kind = ST_MARK;
        if (n >= 3) { snprintf(s->arg, sizeof s->arg, "%s", a1); s->len = atoi(a2); s->nbytes = 1; }
    } else if ((!strcmp(cmd, "onsets") || !strcmp(cmd, "changes")) && n >= 3) {
        const int on = !strcmp(cmd, "onsets");
        char *p = strstr(l, cmd) + strlen(cmd);
        int used;
        double v;
        s->kind = on ? ST_ONSETS : ST_CHANGES;
        if (on ? sscanf(p, " %d %d %lf%n", &s->from, &s->len, &s->gate, &used) != 3
               : sscanf(p, " %d %d %255s%n", &s->from, &s->len, s->arg, &used) != 3) {
            fprintf(stderr, "script line %d: %s\n", i + 1, on ? "onsets A B GATE T..." : "changes A B SYM T...");
            exit(2);
        }
        if (s->len <= s->from) { fprintf(stderr, "script line %d: B must be after A\n", i + 1); exit(2); }
        p += used;
        while (s->nt < 16 && sscanf(p, " %lf%n", &v, &used) == 1) { s->t[s->nt++] = v; p += used; }
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

// ---- the sound ------------------------------------------------------------------------
// mGBA mixes the APU into two blip buffers (left, right); every frame they are
// drained, so they never fill, and while keeping, the samples are kept (mono)
// with the sample count at the end of each frame, which turns a sample into a
// frame.
#define AUD_RATE 48000
static struct blip_t *aud_l, *aud_r;
static int16_t *aud;
static size_t aud_n, aud_cap;
static int aud_on, aud_frame0;          // keeping since the start of frame aud_frame0
static size_t *aud_end;                 // aud_end[k]: samples kept by the end of frame aud_frame0 + k
static size_t aud_nend, aud_capend;
static uint8_t *aud_hram;               // HRAM at the end of each kept frame, 127 bytes a frame
static uint16_t *aud_clock;             // sys_time at the end of each kept frame
static uint32_t clock_at;               // sys_time's address

static void audio_drain(void) {
    static int16_t l[4096], r[4096];
    int avail;
    while ((avail = blip_samples_avail(aud_l)) > 0) {
        if (avail > 4096) avail = 4096;
        const int nl = blip_read_samples(aud_l, l, avail, 0);
        const int nr = blip_read_samples(aud_r, r, nl, 0);
        if (!aud_on) continue;
        if (aud_n + (size_t)nl > aud_cap) {
            aud_cap = (aud_cap ? aud_cap * 2 : (size_t)AUD_RATE * 16) + (size_t)nl;
            aud = realloc(aud, aud_cap * sizeof *aud);
            if (!aud) { perror("realloc"); exit(2); }
        }
        for (int k = 0; k < nl; k++) aud[aud_n++] = (int16_t)(((int)l[k] + (k < nr ? r[k] : 0)) / 2);
    }
    if (!aud_on) return;
    if (aud_nend == aud_capend) {
        aud_capend = aud_capend ? aud_capend * 2 : 4096;
        aud_end = realloc(aud_end, aud_capend * sizeof *aud_end);
        if (!aud_end) { perror("realloc"); exit(2); }
    }
    aud_end[aud_nend] = aud_n;
    aud_hram = realloc(aud_hram, aud_capend * 127);
    if (!aud_hram) { perror("realloc"); exit(2); }
    for (int k = 0; k < 127; k++) aud_hram[aud_nend * 127 + (size_t)k] = (uint8_t)core->busRead8(core, 0xFF80u + (uint32_t)k);
    aud_clock = realloc(aud_clock, aud_capend * sizeof *aud_clock);
    if (!aud_clock) { perror("realloc"); exit(2); }
    aud_clock[aud_nend] = (uint16_t)((core->busRead8(core, clock_at) & 0xFF) | (core->busRead8(core, clock_at + 1) & 0xFF) << 8);
    aud_nend++;
}

static void audio_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    const uint32_t bytes = (uint32_t)(aud_n * 2), rate = AUD_RATE, brate = AUD_RATE * 2;
    const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    const uint32_t riff = 36 + bytes, fmt = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&brate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f); fwrite(aud, 2, aud_n, f);
    fclose(f);
}

// The frame (from aud_frame0, with a fraction) a kept sample falls in.
static double sample_frame(size_t i) {
    size_t k = 0;
    while (k < aud_nend && aud_end[k] <= i) k++;
    const size_t a = k ? aud_end[k - 1] : 0, b = k < aud_nend ? aud_end[k] : a + 1;
    return (double)(aud_frame0 + (int)k) + (double)(i - a) / (double)(b > a ? b - a : 1);
}

// Onsets in the kept sound from sample `from` to `to`, in 1 ms windows (a
// 5 ms window put an onset up to 0.3 frames before the sound began): a window
// is an onset when it is at least 6 dB over the quietest of the 20 before it
// (the history reaches back before `from`), within `gate` dB of the loudest
// window of the range and over -60 dBFS, and 40 ms have passed since the last
// onset (every row of a noise take retriggers, 17 ms apart; two cards are a
// stagger, 50 ms, apart). The gate keeps a quiet cue (a lift's slide) and a
// take's own tail from counting beside a loud one. Writes their frames.
#define ONSET_WIN (AUD_RATE / 1000)
static double win_db(size_t w) {
    double e = 0;
    for (size_t k = w; k < w + ONSET_WIN; k++) e += (double)aud[k] * aud[k];
    return 10.0 * log10(e / ONSET_WIN / (32768.0 * 32768.0) + 1e-12);
}
static int find_onsets(size_t from, size_t to, double gate, double *out, int max) {
    double top = -120.0;
    int n = 0, since = 1000;
    for (size_t w = from; w + ONSET_WIN <= to; w += ONSET_WIN) { const double d = win_db(w); if (d > top) top = d; }
    for (size_t w = from; w + ONSET_WIN <= to; w += ONSET_WIN) {
        const double db = win_db(w);
        double lo = 1e9;
        for (size_t k = 1; k <= 20; k++) { const double p = w >= k * ONSET_WIN ? win_db(w - k * ONSET_WIN) : -120.0; if (p < lo) lo = p; }
        since++;
        if (db > -60.0 && db >= top - gate && db - lo >= 6.0 && since >= 40) {
            if (n < max) out[n] = sample_frame(w);
            n++;
            since = 0;
        }
    }
    return n;
}

static void run_frame(void) {
    uint32_t keys = 0;
    for (int i = 0; i < nheld; i++)
        if (frame_next >= held[i].from && frame_next < held[i].to) keys |= held[i].keys;
    core->setKeys(core, keys);
    core->runFrame(core);
    audio_drain();
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

static int show(const char *what, int frame, const Step *s, int ok) {
    uint32_t a = resolve(s->arg);
    printf("frame %d: %s %s =", frame, what, s->arg);
    for (int k = 0; k < s->len; k++) printf(" %02x", core->busRead8(core, a + (uint32_t)k) & 0xFF);
    if (ok < 0) printf("\n"); else printf(ok ? "  ok\n" : "  FAILED\n");
    return ok;
}

int main(int argc, char **argv) {
    mLogSetDefaultLogger(&logger);
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
    mCoreConfigSetIntValue(&core->config, "volume", 0x100);   // full; the bare config leaves it 0 (silent)
    mCoreLoadConfig(core);
    unsigned w, h;
    core->desiredVideoDimensions(core, &w, &h);
    video = calloc((size_t)w * h, sizeof(color_t));
    stride = w;
    core->setVideoBuffer(core, video, w);
    if (!mCoreLoadFile(core, argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }
    // mGBA ends a blip frame every 4096 clocks (1 ms), and drops samples only
    // when this many wait unread: drained every frame, none ever do, and a
    // frame's samples are the ones drained after it (to 1 ms)
    core->setAudioBufferSize(core, 4096);
    aud_l = core->getAudioChannel(core, 0);
    aud_r = core->getAudioChannel(core, 1);
    blip_set_rates(aud_l, core->frequency(core), AUD_RATE);
    blip_set_rates(aud_r, core->frequency(core), AUD_RATE);
    core->reset(core);

    int fail = 0, now = 0, last_press_end = 0, mark = -1;
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
        } else if (s->kind == ST_PIXSPR) {
            const uint32_t oam = 0xFE00u + 4u * s->bytes[0];
            const int sy = (int)(core->busRead8(core, oam) & 0xFF) - 16, sx = (int)(core->busRead8(core, oam + 1) & 0xFF) - 8;
            const int x = sx + s->bytes[1], y = sy + s->bytes[2];
            const int ok = x >= 0 && x < 160 && y >= 0 && y < 144;
            const int got = ok ? shade_at((unsigned)x, (unsigned)y) : -1;
            const int pass = ok && got == s->bytes[3];
            printf("frame %d: pixelspr %s at (%d, %d) = %d%s\n", f, s->arg, x, y, got, pass ? "  ok" : "  FAILED");
            if (!pass) fail = 1;
        } else if (s->kind == ST_AUDIO_START) {
            clock_at = resolve("_sys_time");
            aud_on = 1; aud_n = 0; aud_nend = 0; aud_frame0 = frame_next;
            printf("frame %d: audio start\n", f);
        } else if (s->kind == ST_AUDIO_SAVE) {
            audio_save(s->arg);
            printf("frame %d: audio save %s (%.2f s)\n", f, s->arg, (double)aud_n / AUD_RATE);
        } else if (s->kind == ST_MARK) {
            if (s->nbytes) {
                const uint32_t a = resolve(s->arg), st = resolve("_sys_time");
                const unsigned want = ((core->busRead8(core, a) & 0xFF) | (core->busRead8(core, a + 1) & 0xFF) << 8) + (unsigned)s->len;
                const unsigned cur = (core->busRead8(core, st) & 0xFF) | (core->busRead8(core, st + 1) & 0xFF) << 8;
                if (!((cur - want) & 0x8000u) && aud_on) {
                    // already reached: the first kept frame that had reached it
                    size_t k = aud_nend;
                    while (k > 0 && !(((unsigned)aud_clock[k - 1] - want) & 0x8000u)) k--;
                    if (k == 0) { fprintf(stderr, "script line %d: mark before audio start\n", s->line); return 2; }
                    mark = aud_frame0 + (int)k;     // the steps after go on from now
                    printf("frame %d: mark (frame %d)\n", now, mark);
                    continue;
                } else {
                    int waited = 0;
                    while ((((core->busRead8(core, st) & 0xFF) | (core->busRead8(core, st + 1) & 0xFF) << 8) - want) & 0x8000u) {
                        run_frame();
                        if (++waited > UNTIL_MAX) { printf("frame %d: mark %s+%d never came  FAILED\n", frame_next - 1, s->arg, s->len); fail = 1; break; }
                    }
                    now = frame_next - 1;
                }
            }
            mark = now;
            printf("frame %d: mark\n", mark);
        } else if (s->kind == ST_ONSETS || s->kind == ST_CHANGES) {
            if (!aud_on || mark < 0) { fprintf(stderr, "script line %d: needs audio start and a mark first\n", s->line); return 2; }
            const int fa = mark + s->from, fb = mark + s->len;
            if (fa <= aud_frame0) { fprintf(stderr, "script line %d: frame %d is before audio start\n", s->line, fa); return 2; }
            while (frame_next <= fb) run_frame();
            now = frame_next - 1;
            double got[64];
            int ng = 0;
            if (s->kind == ST_ONSETS) {
                ng = find_onsets(aud_end[fa - aud_frame0 - 1], aud_end[fb - aud_frame0], s->gate, got, 64);
                for (int k = 0; k < ng && k < 64; k++) got[k] -= mark;
            } else {
                const uint32_t a = resolve(s->arg);
                if (a < 0xFF80 || a > 0xFFFE) { fprintf(stderr, "script line %d: changes watches HRAM only\n", s->line); return 2; }
                for (int fr = fa; fr <= fb; fr++) {
                    const uint8_t was = aud_hram[(size_t)(fr - 1 - aud_frame0) * 127 + (a - 0xFF80)];
                    const uint8_t is = aud_hram[(size_t)(fr - aud_frame0) * 127 + (a - 0xFF80)];
                    for (uint8_t d = (uint8_t)(is - was); d && ng < 64; d--) got[ng++] = fr - mark;
                }
            }
            int ok = ng == s->nt;
            printf("frame %d: %s %d to %d from %d%s%s, at:", now, s->kind == ST_ONSETS ? "onsets" : "changes", s->from, s->len, mark,
                   s->kind == ST_CHANGES ? " of " : "", s->kind == ST_CHANGES ? s->arg : "");
            for (int k = 0; k < ng && k < 64; k++) {
                const int in = k < s->nt && got[k] >= s->t[k] && got[k] < s->t[k] + 2.0;
                if (!in) ok = 0;
                printf(s->kind == ST_ONSETS ? " %.2f%s" : " %.0f%s", got[k], k < s->nt ? (in ? "" : "(!)") : "(+)");
            }
            printf("  expected");
            for (int k = 0; k < s->nt; k++) printf(" %g", s->t[k]);
            printf(ok ? "  ok\n" : "  FAILED\n");
            if (!ok) fail = 1;
        } else if (s->kind == ST_PEEK) {
            show("peek", f, s, -1);
        } else if (s->kind == ST_EXPECT) {
            if (!show("expect", f, s, mem_is(resolve(s->arg), s->bytes, s->len))) fail = 1;
        } else if (s->kind == ST_POKE) {
            uint32_t a = resolve(s->arg);
            for (int k = 0; k < s->len; k++) core->busWrite8(core, a + (uint32_t)k, s->bytes[k]);
        } else if (s->kind == ST_UNTIL) {
            const uint32_t a = resolve(s->arg);
            int waited = 0;
            while (!mem_is(a, s->bytes, s->len) && waited < UNTIL_MAX) { run_frame(); waited++; }
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
