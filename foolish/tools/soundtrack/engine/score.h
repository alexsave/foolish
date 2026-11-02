// score.h: tracks, notes, tempo map, a small text melody language, step patterns.
// A city's score is plain C that calls these. Times are written in beats; the tempo
// map turns them into seconds when the piece is rendered.
#pragma once
#include "dsp.h"
#include <ctype.h>

#define MAXTR 96
#define MAXNOTE 200000
#define MAXCC 200000

typedef struct Note { int tr; double beat, len, pitch, vel; double p[4]; int k; uint64_t seed; double t, dur; } Note;
typedef struct Cc { int tr; double beat; int num, val; int k; double t; } Cc;

struct Track;
typedef void (*SynthFn)(struct Track *tr, const Note *n, float *L, float *R, long nframes);

typedef struct { int type; double f, q, g; } EqBand;   // type: 1 lp 2 hp 3 peak 4 lshelf 5 hshelf
typedef struct Track {
    char name[32];
    int gm, prog, msb, lsb;           // gm=1: rendered by gmrender (Apple DLS)
    SynthFn fn; double sp[24];        // procedural synth and its parameters
    // timing feel
    double human_ms, human_vel, swing; // swing: delay of off 16ths as a fraction of a 16th
    double swing8;                     // MPC "8ths" swing: delay of off 8ths as a fraction of an 8th
    double delay_ms;                   // constant offset (behind the beat > 0)
    // mix
    double gain_db, pan, width;
    EqBand eq[8]; int neq;
    double drive, drive_mix;           // tanh saturation
    double cthr, cratio, catt, crel;   // compressor (cthr 0 = off)
    int duck_src; double duck_db, duck_rel, duck_att; // sidechain: duck under another track's note onsets
    double send[4];                    // linear send levels to buses 0..3
    int mute;
    double chorus_mix, chorus_ms, chorus_depth_ms, chorus_hz; // stereo chorus insert (0 = off)
    double haas_ms;                    // widen: delay one side by a few ms (0 = off)
    double crush_sr, crush_bits;       // sampler character: sample-and-hold rate (Hz) and bit depth (0 = off)
    int group;                         // 1..4 = summed into a group bus (drum bus etc.) before the master
    float *L, *R;                      // rendered dry buffer
    void *user;                        // instrument data (e.g. a sample set)
} Track;

typedef struct { double beat, bpm; } TempoPt;

// ---------------- global score state ----------------
static Track g_tr[MAXTR]; static int g_ntr;
static Note *g_note; static int g_nnote;
static Cc *g_cc; static int g_ncc;
static TempoPt g_tempo[4096]; static int g_ntempo;
static int g_k;            // loop pass index (0 in full mode)
static double g_period_s;  // loop period in seconds (integer samples) in loop mode, 0 in full mode
static long g_total;       // frames
static int g_mode;         // 0 full, 1 loop
static double g_body_beats;

static int track(const char *name) { if (g_ntr >= MAXTR) { fprintf(stderr, "too many tracks (MAXTR %d)\n", MAXTR); exit(1); } Track *t = &g_tr[g_ntr]; memset(t, 0, sizeof *t); snprintf(t->name, 32, "%s", name);
    t->width = 1; t->duck_src = -1; t->drive_mix = 1; return g_ntr++; }
static int gm_track(const char *name, int prog, int drums) { int i = track(name); g_tr[i].gm = 1; g_tr[i].prog = prog; g_tr[i].msb = drums ? 120 : 121; return i; }
static int syn_track(const char *name, SynthFn fn) { int i = track(name); g_tr[i].fn = fn; return i; }
static void eq(int tr, int type, double f, double q, double g) { Track *t = &g_tr[tr]; t->eq[t->neq++] = (EqBand){type, f, q, g}; }

static void tempo(double beat, double bpm) { if (g_k) return; g_tempo[g_ntempo++] = (TempoPt){beat, bpm}; }

static Note *note(int tr, double beat, double len, double pitch, double vel) {
    if (!g_note) g_note = calloc(MAXNOTE, sizeof(Note));
    if (g_nnote >= MAXNOTE) { fprintf(stderr, "too many notes\n"); exit(1); }
    Note *n = &g_note[g_nnote++]; memset(n, 0, sizeof *n);
    n->tr = tr; n->beat = beat; n->len = len; n->pitch = pitch; n->vel = vel; n->k = g_k;
    // seed from what the note IS, not when it was written, so every loop pass is identical
    n->seed = mix64((uint64_t)(tr + 1) * 1000003ULL ^ (uint64_t)llround(beat * 960) * 7919ULL ^ (uint64_t)llround(pitch * 100) * 104729ULL);
    return n;
}
static void cc(int tr, double beat, int num, int val) {
    if (!g_cc) g_cc = calloc(MAXCC, sizeof(Cc));
    g_cc[g_ncc++] = (Cc){tr, beat, num, val < 0 ? 0 : val > 127 ? 127 : val, g_k, 0};
}
static void ccramp(int tr, double b0, double b1, int num, int v0, int v1) {
    int steps = (int)fmax(2, (b1 - b0) * 16);
    for (int i = 0; i <= steps; i++) cc(tr, b0 + (b1 - b0) * i / steps, num, (int)lround(v0 + (v1 - v0) * (double)i / steps));
}

// ---------------- pitch names ----------------
// "C4" = 60. Accepts C, C#, Db, Bb, E#, Cb, x (double sharp) not needed.
static double pname(const char **s) {
    const char *p = *s; static const int base[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    char c = toupper(*p++); if (c < 'A' || c > 'G') { fprintf(stderr, "bad pitch at '%s'\n", *s); exit(1); }
    int v = base[c - 'A'];
    while (*p == '#' || *p == 'b') { v += *p == '#' ? 1 : -1; p++; }
    int sign = 1; if (*p == '-') { sign = -1; p++; }
    int oct = 0; while (isdigit((unsigned char)*p)) oct = oct * 10 + (*p++ - '0');
    *s = p; return v + 12 * (sign * oct + 1);
}
static double P(const char *s) { return pname(&s); }

// ---------------- melody language ----------------
// tokens separated by spaces: PITCH[+PITCH...][:len][@vel][~]   or r:len (rest)
//   len in beats (default: previous), vel 0..1 (default: previous), '~' = legato (len * 1.0 instead of gate)
//   '>' prefix = accent (+0.15 vel), '.' suffix on len = staccato (gate 0.5)
// returns the beat after the last token. gate: fraction of len a note sounds.
static double g_gate = 0.92;
static double mel(int tr, double beat, const char *src, double vel0, double transpose) {
    const char *s = src; double len = 1, vel = vel0;
    while (*s) {
        while (*s == ' ' || *s == '\n' || *s == '|') s++;
        if (!*s) break;
        int acc = 0; if (*s == '>') { acc = 1; s++; }
        double ps[8]; int np = 0; int rest = 0;
        if (*s == 'r') { rest = 1; s++; }
        else { for (;;) { ps[np++] = pname(&s); if (*s == '+') { s++; continue; } break; } }
        double gate = g_gate;
        if (*s == ':') { s++; len = strtod(s, (char **)&s); if (*s == '.') { gate = 0.5; s++; } }
        if (*s == '@') { s++; vel = strtod(s, (char **)&s); }
        if (*s == '~') { gate = 1.0; s++; }
        if (!rest) for (int i = 0; i < np; i++) note(tr, beat, len * gate, ps[i] + transpose, fmin(1, vel + acc * 0.15));
        beat += len;
        while (*s && *s != ' ' && *s != '\n') s++;
    }
    return beat;
}

// ---------------- step patterns ----------------
// one char per step of `step` beats: '.' rest, 'x' hit (vel), 'X' accent (vel*1.25), 'o' ghost (vel*0.45), '-' tie (extends previous)
static void pat(int tr, double beat, double step, const char *p, double pitch, double vel) {
    int n = (int)strlen(p);
    for (int i = 0; i < n; i++) {
        char c = p[i]; if (c == '.' || c == '-' || c == ' ' || c == '|') continue;
        double v = c == 'X' ? vel * 1.25 : c == 'o' ? vel * 0.45 : vel;
        int j = i + 1; while (j < n && p[j] == '-') j++;
        note(tr, beat + i * step, (j - i) * step * 0.95, pitch, fmin(1, v));
    }
}

// ---------------- chords ----------------
// "Am7" style is error-prone; chords are written as pitch lists "A3+C4+E4+G4".
static int chord_pitches(const char *s, double *out) { int n = 0; for (;;) { out[n++] = pname(&s); if (*s == '+') { s++; continue; } break; } return n; }

// ---------------- tempo map ----------------
static int tcmp(const void *a, const void *b) { double d = ((TempoPt *)a)->beat - ((TempoPt *)b)->beat; return d < 0 ? -1 : d > 0; }
// tempo is linear between points (in beats); seconds = integral of 60/bpm
static double b2t_raw(double beat) {
    if (g_ntempo == 0) { fprintf(stderr, "no tempo\n"); exit(1); }
    double t = 0, b = 0, bpm = g_tempo[0].bpm;
    if (beat <= g_tempo[0].beat) return beat * 60 / g_tempo[0].bpm;
    t = g_tempo[0].beat * 60 / g_tempo[0].bpm; b = g_tempo[0].beat;
    for (int i = 0; i + 1 < g_ntempo; i++) {
        double b0 = g_tempo[i].beat, b1 = g_tempo[i + 1].beat, v0 = g_tempo[i].bpm, v1 = g_tempo[i + 1].bpm;
        double end = fmin(beat, b1);
        if (end > b0) {
            // integral of 60/(v0 + (v1-v0) x/(b1-b0)) dx from 0 to end-b0
            double L = b1 - b0, x = end - b0;
            if (fabs(v1 - v0) < 1e-9) t += 60 * x / v0;
            else { double k = (v1 - v0) / L; t += 60 / k * log((v0 + k * x) / v0); }
        }
        if (beat <= b1) return t;
        b = b1; bpm = v1;
    }
    return t + (beat - b) * 60 / bpm;
}
static double g_tscale = 1;
static double b2t(double beat) { return b2t_raw(beat) * g_tscale; }
static double bpm_at(double beat) { // for synths that want the local beat length
    if (g_ntempo == 1 || beat <= g_tempo[0].beat) return g_tempo[0].bpm;
    for (int i = 0; i + 1 < g_ntempo; i++) if (beat < g_tempo[i + 1].beat) {
        double x = (beat - g_tempo[i].beat) / (g_tempo[i + 1].beat - g_tempo[i].beat); return g_tempo[i].bpm + x * (g_tempo[i + 1].bpm - g_tempo[i].bpm); }
    return g_tempo[g_ntempo - 1].bpm;
}
