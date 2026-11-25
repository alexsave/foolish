// glue: a tiny synth for bridging passages. It only renders a short score to a wav, which splice then places like any
// other source (it is not a second splicer).
//   glue score.txt out.wav
// One statement per line, '#' starts a comment; an unknown statement or a bad value is fatal and names the line.
//   TEMPO bpm                  required, and the first statement
//   LEN beats                  total length (default: the end of the last note); 4 more seconds are rendered for the reverb tail
//   REVERB db                  send level of a 2.4 s hall (default -10; 0 dB returns the wet as loud as the dry)
//   CUTOFF hz                  low-pass of the pads (default 1800)
//   SEED n                     the one seed of every random draw (default GLUE_SEED)
//   PAD  beat dur midi[,midi...] db   a warm pad chord: per note 7 saws detuned +-11 cents with random start phases spread
//                              over the stereo field, a 24 dB/oct low-pass at CUTOFF drifting +-15 percent at 0.13 Hz,
//                              attack 0.35 s, release 1.2 s after the note's end
//   KEYS beat dur midi[,midi...] db   a soft electric piano: two-operator FM (modulator at the carrier's frequency, index 1.6
//                              falling to 0.3 over 0.8 s) plus a quiet partial at 14f dying in 80 ms, attack 4 ms,
//                              exponential decay with a 2.2 s time constant while held, release 0.25 s, a 4.5 Hz tremolo
//                              of +-1.5 dB, opposite in left and right
//   SUB  beat dur midi db      a sine with a touch of tanh saturation, attack 15 ms, release 80 ms, mono
//   BRASS beat dur midi[,midi...] db  a warm analog brass-synth chord or lead: 5 detuned saws spread in stereo through a
//                              24 dB/oct low-pass with a brass filter envelope (opens from 350 Hz to 2400 Hz over 110 ms,
//                              settles to 1100 Hz), attack 35 ms, release 0.45 s
//   PIANO beat dur midi[,midi...] vel   a sampled grand piano note or chord (Salamander Grand Piano V3, 48 kHz 24 bit stereo,
//                              files <Note><octave>v<layer>.wav with Note one of A C D# F#, a sample every minor third A0..C8,
//                              layers 1 (softest) to 16, found by probing A4v1, A4v2, ... in the SAMPLES directory). Per note:
//                              the nearest sampled root, resampled with cubic interpolation to the (fractional) midi pitch, its
//                              leading silence trimmed (1.5 ms kept before the first sample over 0.3 percent of its peak). vel is
//                              1..127 (fractions allowed): layer = floor((vel - 1) * nlayers / 127) (1..127 spread evenly over the
//                              layers present, 127 -> the loudest), and a gain of -30 + 30 * vel / 127 dB on top of the sample's
//                              own recorded level (so 0 dB at vel 127). The note rings with its natural decay for dur beats, then a
//                              damper releases it over 0.30 s (a raised-cosine fade to zero), unless the pedal is down. A piano
//                              statement is not peak-normalised (dynamics come from vel, db plays no part); it goes through the
//                              same reverb send as the other voices. A sample that ends before the note does just ends.
//   PEDAL beat dur             the sustain pedal is down for [beat, beat + dur): a PIANO note that starts inside it, or is sounding
//                              when it goes down (starts before and its own end is at or after the pedal's start), does not release
//                              at its own end but rings with its natural decay until the span's end, then releases over 0.30 s
//   SAMPLES /abs/dir           the Salamander directory (default PIANO_SAMPLES_DEFAULT below); a missing directory or sample file is
//                              fatal and names the path and the line. Samples are read once, only those a score needs.
//   MANIFESTS /abs/dir         directory of sampler manifests (default MANIFESTS_DEFAULT below)
//   SAMPLEROOT /abs/dir        root directory for re-rooting ".../soundtrack/samples/" manifest paths (default $SAMPLES or SAMPLEROOT_DEFAULT)
//   INST beat dur midi[,midi...] vel manifest   a sampled instrument from a manifest ("root_midi layer rr path" lines, as sampler.h):
//                              manifest is either a name in MANIFESTS (with or without .txt) or an absolute path. Picks nearest root,
//                              velocity layer (vel 1..127, same gain curve as PIANO), and a round-robin from SEED. Sustained samples
//                              (whose late RMS is at least 15 percent of their body RMS) crossfade-loop 50-90% when held past their length;
//                              releases over 0.25 s with a raised cosine. Missing manifest or sample is fatal and names path and line.
// beat and dur are in beats at TEMPO and may be fractional anywhere (TEMPO 60 makes a beat a second, so a score can be written in
// seconds: PIANO 1.5 0.25 60 80 is a quarter of a second at 1.5 s), midi may be fractional, db is the peak level of that statement
// in dBFS before the reverb. Output: 48 kHz stereo 24 bit, scaled as a whole when its peak would pass -1 dBFS (the scale is printed on stderr).
// Every random draw comes from one Rng seeded by SEED, so two runs are byte-identical.
// mix.h and dsp.h hold helpers (limiter, loudness, wav reader, shelves) that a glue score does not need
#pragma clang diagnostic ignored "-Wunused-function"
#include "mix.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>

#define GLUE_SEED 0x61a7e5eedULL     // the default SEED
#define TAIL_S 4.0                   // seconds of reverb tail after LEN
#define PEAK_CEIL_DB (-1.0)
#define MAX_NOTES 16
#define MAX_STMT 4096
#define PAD_VOICES 7
#define PAD_DETUNE_CENTS 11.0
#define PAD_PAN_SPREAD 0.8
#define PAD_ATTACK_S 0.35
#define PAD_RELEASE_S 1.2
#define PAD_DRIFT 0.15
#define PAD_DRIFT_HZ 0.13
#define FILT_BLOCK 32                // samples between coefficient updates of the drifting low-pass
#define KEYS_ATTACK_S 0.004
#define KEYS_DECAY_S 2.2
#define KEYS_RELEASE_S 0.25
#define KEYS_IDX0 1.6
#define KEYS_IDX1 0.3
#define KEYS_IDX_S 0.8
#define KEYS_TINE_RATIO 14.0
#define KEYS_TINE_AMP 0.08
#define KEYS_TINE_S 0.08
#define KEYS_TREM_HZ 4.5
#define KEYS_TREM_DB 1.5
#define SUB_DRIVE 1.4
#define SUB_ATTACK_S 0.015
#define SUB_RELEASE_S 0.08
#define REVERB_RT 2.4
#define PIANO_SAMPLES_DEFAULT "/Users/alex/Dev/soundtrack-samples/salamander/SalamanderGrandPianoV3_48khz24bit/48khz24bit"
#define MANIFESTS_DEFAULT "/Users/alex/Dev/foolish-wt-city-mixes/foolish/tools/soundtrack/samples/manifests"
#define SAMPLEROOT_DEFAULT "/Users/alex/Dev/soundtrack-samples"
#define PIANO_ROOT0 21               // A0; roots every 3 semitones up to C8 (108)
#define PIANO_NROOTS 30
#define PIANO_MAXLAYERS 16
#define PIANO_RELEASE_S 0.30
#define INST_ATTACK_S 0.008
#define INST_SUS_ATTACK_S 0.025
#define INST_RELEASE_S 0.25
#define BRASS_VOICES 5
#define BRASS_DETUNE_CENTS 9.0
#define BRASS_ATTACK_S 0.035
#define BRASS_RELEASE_S 0.45
#define INST_MAXSETS 32
#define INST_MAXSMP 2048
#define PIANO_DB_RANGE 30.0
#define PIANO_TRIM_KEEP_S 0.0015
#define PIANO_TRIM_THRESH 0.003
#define MAX_PATH_LEN 1024

typedef enum { PAD, KEYS, SUBV, PIANO, INST, BRASS } Kind;
typedef struct { Kind kind; int line; double beat, dur, db, vel; int n; double midi[MAX_NOTES]; char inst[128]; } Stmt;
typedef struct { double beat, dur; } Span;

static void die(int line, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void die(int line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "glue: ");
    if (line > 0) fprintf(stderr, "line %d: ", line);
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
    exit(1);
}

static double num(int line, const char *s, const char *what, double lo, double hi) {
    char *e; errno = 0; double v = strtod(s, &e);
    if (e == s || *e || errno || !isfinite(v)) die(line, "bad %s '%s'", what, s);
    if (v < lo || v > hi) die(line, "%s %s is outside %g..%g", what, s, lo, hi);
    return v;
}

static Stmt g_st[MAX_STMT]; static int g_nst;
static Span g_ped[MAX_STMT]; static int g_nped;
static char g_samples[MAX_PATH_LEN] = PIANO_SAMPLES_DEFAULT;
static char g_manifests[MAX_PATH_LEN] = MANIFESTS_DEFAULT;
static char g_sampleroot[MAX_PATH_LEN] = "";
static double g_tempo, g_len = -1, g_rev_db = -10, g_cutoff = 1800; static uint64_t g_seed = GLUE_SEED;

static void parse(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(1); }
    char buf[4096]; int line = 0, seen_tempo = 0, seen_len = 0, seen_rev = 0, seen_cut = 0, seen_seed = 0, seen_samples = 0, seen_man = 0, seen_sr = 0;
    while (fgets(buf, sizeof buf, f)) {
        line++;
        if (!strchr(buf, '\n') && !feof(f)) die(line, "line too long");
        char *c = strchr(buf, '#'); if (c) *c = 0;
        char *tok[8]; int nt = 0; char *sp = NULL;
        for (char *t = strtok_r(buf, " \t\r\n", &sp); t; t = strtok_r(NULL, " \t\r\n", &sp)) {
            if (nt == 8) die(line, "too many words");
            tok[nt++] = t;
        }
        if (!nt) continue;
        const char *k = tok[0];
        if (!seen_tempo && strcmp(k, "TEMPO")) die(line, "the first statement must be TEMPO");
#define ARGS(n) do { if (nt != (n) + 1) die(line, "%s takes %d value%s", k, (n), (n) == 1 ? "" : "s"); } while (0)
#define ONCE(flag) do { if (flag) die(line, "%s given twice", k); flag = 1; } while (0)
        if (!strcmp(k, "TEMPO")) { ARGS(1); ONCE(seen_tempo); g_tempo = num(line, tok[1], "tempo", 20, 400); }
        else if (!strcmp(k, "LEN")) { ARGS(1); ONCE(seen_len); g_len = num(line, tok[1], "length", 0.001, 100000); }
        else if (!strcmp(k, "REVERB")) { ARGS(1); ONCE(seen_rev); g_rev_db = num(line, tok[1], "reverb level", -150, 24); }
        else if (!strcmp(k, "CUTOFF")) { ARGS(1); ONCE(seen_cut); g_cutoff = num(line, tok[1], "cutoff", 40, 20000); }
        else if (!strcmp(k, "SEED")) { ARGS(1); ONCE(seen_seed); g_seed = (uint64_t)num(line, tok[1], "seed", 0, 9007199254740992.0); }
        else if (!strcmp(k, "SAMPLES")) {
            ARGS(1); ONCE(seen_samples);
            if (strlen(tok[1]) >= sizeof g_samples) die(line, "samples path too long");
            snprintf(g_samples, sizeof g_samples, "%s", tok[1]);
        }
        else if (!strcmp(k, "MANIFESTS")) {
            ARGS(1); ONCE(seen_man);
            if (strlen(tok[1]) >= sizeof g_manifests) die(line, "manifests path too long");
            snprintf(g_manifests, sizeof g_manifests, "%s", tok[1]);
        }
        else if (!strcmp(k, "SAMPLEROOT")) {
            ARGS(1); ONCE(seen_sr);
            if (strlen(tok[1]) >= sizeof g_sampleroot) die(line, "sampleroot path too long");
            snprintf(g_sampleroot, sizeof g_sampleroot, "%s", tok[1]);
        }
        else if (!strcmp(k, "PEDAL")) {
            ARGS(2); if (g_nped == MAX_STMT) die(line, "too many pedal spans");
            g_ped[g_nped].beat = num(line, tok[1], "beat", 0, 1e6); g_ped[g_nped].dur = num(line, tok[2], "duration", 0.001, 1e5); g_nped++;
        }
        else if (!strcmp(k, "PAD") || !strcmp(k, "KEYS") || !strcmp(k, "SUB") || !strcmp(k, "PIANO") || !strcmp(k, "INST") || !strcmp(k, "BRASS")) {
            if (g_nst == MAX_STMT) die(line, "too many notes");
            Stmt *s = &g_st[g_nst++]; memset(s, 0, sizeof *s); s->line = line;
            s->kind = !strcmp(k, "PAD") ? PAD : !strcmp(k, "KEYS") ? KEYS : !strcmp(k, "PIANO") ? PIANO : !strcmp(k, "INST") ? INST : !strcmp(k, "BRASS") ? BRASS : SUBV;
            if (s->kind == INST) { ARGS(5); } else { ARGS(4); }
            s->beat = num(line, tok[1], "beat", 0, 1e6);
            s->dur = num(line, tok[2], "duration", 0.001, 1e5);
            { size_t ln = strlen(tok[3]); if (tok[3][0] == ',' || tok[3][ln - 1] == ',' || strstr(tok[3], ",,")) die(line, "bad note list '%s'", tok[3]); }
            char *sp2 = NULL; char *ml = tok[3];
            for (char *m = strtok_r(ml, ",", &sp2); m; m = strtok_r(NULL, ",", &sp2)) {
                if (s->n == MAX_NOTES) die(line, "more than %d notes", MAX_NOTES);
                s->midi[s->n++] = num(line, m, "midi note", 0, 127);
            }
            if (s->kind == SUBV && s->n != 1) die(line, "SUB takes one note");
            if (s->kind == PIANO || s->kind == INST) s->vel = num(line, tok[4], "velocity", 1, 127);
            else s->db = num(line, tok[4], "level", -150, 0);
            if (s->kind == INST) {
                if (strlen(tok[5]) >= sizeof s->inst) die(line, "manifest name too long");
                snprintf(s->inst, sizeof s->inst, "%s", tok[5]);
            }
        } else die(line, "unknown statement '%s'", k);
    }
    fclose(f);
    if (!seen_tempo) die(0, "no TEMPO");
}

// one rendered statement: stereo, from sample `at`
typedef struct { float *l, *r; long n, at; } Buf;
static Buf buf_new(long at, long n) { Buf b = {calloc(n, sizeof(float)), calloc(n, sizeof(float)), n, at}; if (!b.l || !b.r) die(0, "out of memory"); return b; }

static double polyblep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}
static void set_coef(Bq *f, Bq c) { f->b0 = c.b0; f->b1 = c.b1; f->b2 = c.b2; f->a1 = c.a1; f->a2 = c.a2; }

static void pad_note(Buf *o, double midi, double dur_s, Rng *rg) {
    double f0 = mtof(midi), ph[PAD_VOICES], dt[PAD_VOICES], gl[PAD_VOICES], gr[PAD_VOICES];
    for (int v = 0; v < PAD_VOICES; v++) {
        double cents = -PAD_DETUNE_CENTS + 2 * PAD_DETUNE_CENTS * v / (PAD_VOICES - 1);
        dt[v] = f0 * pow(2, cents / 1200) / SR; ph[v] = r01(rg);
        double pan = (v - (PAD_VOICES - 1) / 2.0) / ((PAD_VOICES - 1) / 2.0) * PAD_PAN_SPREAD, th = (pan + 1) * M_PI / 4;
        gl[v] = cos(th); gr[v] = sin(th);
    }
    double dph = r01(rg) * TAU;
    Bq fl[2], fr[2]; static const double Q[2] = {0.5411961, 1.3065630};
    for (int i = 0; i < 2; i++) { fl[i] = bq_lp(g_cutoff, Q[i]); fr[i] = fl[i]; }
    double lvl_end = fmin(1.0, dur_s / PAD_ATTACK_S);
    for (long i = 0; i < o->n; i++) {
        double t = (double)i / SR;
        if (i % FILT_BLOCK == 0) {
            double fc = g_cutoff * (1 + PAD_DRIFT * sin(TAU * PAD_DRIFT_HZ * t + dph));
            for (int j = 0; j < 2; j++) { Bq c = bq_lp(fc, Q[j]); set_coef(&fl[j], c); set_coef(&fr[j], c); }
        }
        double l = 0, r = 0;
        for (int v = 0; v < PAD_VOICES; v++) {
            double s = 2 * ph[v] - 1 - polyblep(ph[v], dt[v]);
            l += s * gl[v]; r += s * gr[v];
            ph[v] += dt[v]; if (ph[v] >= 1) ph[v] -= 1;
        }
        for (int j = 0; j < 2; j++) { l = bq(&fl[j], l); r = bq(&fr[j], r); }
        double env = t < dur_s ? fmin(1.0, t / PAD_ATTACK_S) : lvl_end * fmax(0.0, 1 - (t - dur_s) / PAD_RELEASE_S);
        o->l[i] += (float)(l * env / PAD_VOICES); o->r[i] += (float)(r * env / PAD_VOICES);
    }
}

static void keys_note(Buf *o, double midi, double dur_s) {
    double f = mtof(midi), th0 = TAU * f / SR;
    int tine = KEYS_TINE_RATIO * f < 0.45 * SR;
    double held_end = exp(-dur_s / KEYS_DECAY_S) * fmin(1.0, dur_s / KEYS_ATTACK_S);
    for (long i = 0; i < o->n; i++) {
        double t = (double)i / SR, th = th0 * i;
        double idx = KEYS_IDX0 + (KEYS_IDX1 - KEYS_IDX0) * fmin(1.0, t / KEYS_IDX_S);
        double y = sin(th + idx * sin(th));
        if (tine) y += KEYS_TINE_AMP * sin(KEYS_TINE_RATIO * th) * exp(-t / KEYS_TINE_S);
        double env = t < dur_s ? fmin(1.0, t / KEYS_ATTACK_S) * exp(-t / KEYS_DECAY_S) : held_end * fmax(0.0, 1 - (t - dur_s) / KEYS_RELEASE_S);
        double tr = KEYS_TREM_DB * sin(TAU * KEYS_TREM_HZ * t);
        o->l[i] += (float)(y * env * db2a(tr)); o->r[i] += (float)(y * env * db2a(-tr));
    }
}

static void sub_note(Buf *o, double midi, double dur_s) {
    double th0 = TAU * mtof(midi) / SR, norm = tanh(SUB_DRIVE);
    double lvl_end = fmin(1.0, dur_s / SUB_ATTACK_S);
    for (long i = 0; i < o->n; i++) {
        double t = (double)i / SR, y = tanh(SUB_DRIVE * sin(th0 * i)) / norm;
        double env = t < dur_s ? fmin(1.0, t / SUB_ATTACK_S) : lvl_end * fmax(0.0, 1 - (t - dur_s) / SUB_RELEASE_S);
        o->l[i] += (float)(y * env); o->r[i] += (float)(y * env);
    }
}

static void brass_note(Buf *o, double midi, double dur_s, Rng *rg) {
    double f0 = mtof(midi), ph[BRASS_VOICES], dt[BRASS_VOICES], gl[BRASS_VOICES], gr[BRASS_VOICES];
    for (int v = 0; v < BRASS_VOICES; v++) {
        double cents = -BRASS_DETUNE_CENTS + 2 * BRASS_DETUNE_CENTS * v / (BRASS_VOICES - 1);
        dt[v] = f0 * pow(2, cents / 1200) / SR; ph[v] = r01(rg);
        double pan = (v - (BRASS_VOICES - 1) / 2.0) / ((BRASS_VOICES - 1) / 2.0) * 0.55, th = (pan + 1) * M_PI / 4;
        gl[v] = cos(th); gr[v] = sin(th);
    }
    Bq fl[2], fr[2]; static const double Q[2] = {0.5411961, 1.3065630};
    for (int i = 0; i < 2; i++) { fl[i] = bq_lp(400.0, Q[i]); fr[i] = fl[i]; }
    double lvl_end = fmin(1.0, dur_s / BRASS_ATTACK_S);
    for (long i = 0; i < o->n; i++) {
        double t = (double)i / SR;
        if (i % FILT_BLOCK == 0) {
            double fc;
            if (t < 0.11) fc = 350.0 + (2400.0 - 350.0) * (t / 0.11);
            else if (t < dur_s) fc = 1100.0 + (2400.0 - 1100.0) * exp(-(t - 0.11) / 0.35);
            else fc = 1100.0 * exp(-(t - dur_s) / 0.25) + 280.0;
            fc = fmin(fmax(fc, 220.0), 12000.0);
            for (int j = 0; j < 2; j++) { Bq c = bq_lp(fc, Q[j]); set_coef(&fl[j], c); set_coef(&fr[j], c); }
        }
        double l = 0, r = 0;
        for (int v = 0; v < BRASS_VOICES; v++) {
            double s = 2 * ph[v] - 1 - polyblep(ph[v], dt[v]);
            l += s * gl[v]; r += s * gr[v];
            ph[v] += dt[v]; if (ph[v] >= 1) ph[v] -= 1;
        }
        for (int j = 0; j < 2; j++) { l = bq(&fl[j], l); r = bq(&fr[j], r); }
        double env = t < dur_s ? fmin(1.0, t / BRASS_ATTACK_S) : lvl_end * fmax(0.0, 1 - (t - dur_s) / BRASS_RELEASE_S);
        o->l[i] += (float)(l * env / BRASS_VOICES); o->r[i] += (float)(r * env / BRASS_VOICES);
    }
}

// ---- the sampled piano ----
typedef struct { float *l, *r; long n; double sr; int loaded; } Smp;
static Smp g_smp[PIANO_NROOTS][PIANO_MAXLAYERS]; static int g_nlayers;

static void root_path(char *out, size_t sz, int ri, int layer) {   // layer is 1-based
    static const char *const nm[4] = {"A", "C", "D#", "F#"};
    int root = PIANO_ROOT0 + 3 * ri;
    snprintf(out, sz, "%s/%s%dv%d.wav", g_samples, nm[ri % 4], root / 12 - 1, layer);
}
static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }
// layers present: A4v1, A4v2, ... in a row (at most 16)
static void piano_probe(int line) {
    char p[MAX_PATH_LEN + 32];
    if (g_nlayers) return;
    while (g_nlayers < PIANO_MAXLAYERS) { root_path(p, sizeof p, 16, g_nlayers + 1); if (!file_exists(p)) break; g_nlayers++; }
    if (!g_nlayers) { root_path(p, sizeof p, 16, 1); die(line, "no piano samples in %s (looked for %s)", g_samples, p); }
}
static void smp_trim(Smp *x) {   // as sampler.h: drop leading silence, keeping 1.5 ms before the first sample over 0.3 percent of the peak
    double pk = 1e-9; for (long i = 0; i < x->n; i++) pk = fmax(pk, fmax(fabsf(x->l[i]), fabsf(x->r[i])));
    long a = 0; while (a < x->n && fabsf(x->l[a]) < pk * PIANO_TRIM_THRESH && fabsf(x->r[a]) < pk * PIANO_TRIM_THRESH) a++;
    a -= (long)(PIANO_TRIM_KEEP_S * x->sr); if (a < 0) a = 0;
    if (a > 0) { memmove(x->l, x->l + a, (x->n - a) * sizeof(float)); memmove(x->r, x->r + a, (x->n - a) * sizeof(float)); x->n -= a; }
}
static Smp *piano_sample(int ri, int layer, int line) {   // layer is 0-based; loaded on first use
    Smp *x = &g_smp[ri][layer];
    if (!x->loaded) {
        char p[MAX_PATH_LEN + 32]; root_path(p, sizeof p, ri, layer + 1);
        if (!file_exists(p)) die(line, "missing piano sample %s", p);
        g_wav_quiet = 1; x->n = wav_read(p, &x->l, &x->r); x->sr = g_wav_sr; g_wav_quiet = 0;
        if (x->n < 2) die(line, "empty piano sample %s", p);
        smp_trim(x); x->loaded = 1;
    }
    return x;
}
static inline double cubic(const float *x, long n, double pos) {   // as sampler.h
    long i = (long)pos; double t = pos - i;
    double y0 = i > 0 ? x[i - 1] : x[0], y1 = i < n ? x[i] : 0, y2 = i + 1 < n ? x[i + 1] : 0, y3 = i + 2 < n ? x[i + 2] : 0;
    double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3, b = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c = -0.5 * y0 + 0.5 * y2;
    return ((a * t + b) * t + c) * t + y1;
}
// when the damper falls for a note held [t0, t1] (in beats): the end of the last pedal span that is down at some point of the note
static double piano_hold_end(double t0, double t1) {
    double e = t1;
    for (int i = 0; i < g_nped; i++) {
        double ps = g_ped[i].beat, pe = ps + g_ped[i].dur;
        if (ps <= t1 && pe > t0 && pe > e) e = pe;
    }
    return e;
}
static void piano_note(float *L, float *R, long n, long at, double midi, double vel, double hold_s, int line) {
    int ri = (int)lround((midi - PIANO_ROOT0) / 3.0); if (ri < 0) ri = 0; if (ri >= PIANO_NROOTS) ri = PIANO_NROOTS - 1;
    int layer = (int)floor((vel - 1) * g_nlayers / 127.0); if (layer < 0) layer = 0; if (layer >= g_nlayers) layer = g_nlayers - 1;
    Smp *x = piano_sample(ri, layer, line);
    double ratio = pow(2, (midi - (PIANO_ROOT0 + 3 * ri)) / 12.0) * x->sr / SR, g = db2a(-PIANO_DB_RANGE + PIANO_DB_RANGE * vel / 127.0);
    long len = lround((hold_s + PIANO_RELEASE_S) * SR) + 1; double pos = 0;
    for (long i = 0; i < len && at + i < n; i++) {
        double t = (double)i / SR, a = t < hold_s ? 1.0 : 0.5 * (1 + cos(M_PI * fmin(1.0, (t - hold_s) / PIANO_RELEASE_S)));
        if (pos >= x->n - 1) break;
        L[at + i] += (float)(cubic(x->l, x->n, pos) * a * g); R[at + i] += (float)(cubic(x->r, x->n, pos) * a * g);
        pos += ratio;
    }
}

// ---- generic manifest-backed sampled instruments (sampler.h format) ----
typedef struct { int root, layer, rr, sustained; Smp smp; char path[MAX_PATH_LEN]; } InstSmp;
typedef struct { char name[128]; InstSmp *s; int n, nl; } InstSet;
static InstSet g_insts[INST_MAXSETS]; static int g_ninsts;

static InstSet *inst_load(const char *name, int line) {
    for (int i = 0; i < g_ninsts; i++) if (!strcmp(g_insts[i].name, name)) return &g_insts[i];
    if (g_ninsts == INST_MAXSETS) die(line, "too many distinct manifests (max %d)", INST_MAXSETS);
    char mpath[MAX_PATH_LEN + 160];
    if (name[0] == '/') snprintf(mpath, sizeof mpath, "%s", name);
    else {
        size_t ln = strlen(name);
        if (ln > 4 && !strcmp(name + ln - 4, ".txt")) snprintf(mpath, sizeof mpath, "%s/%s", g_manifests, name);
        else snprintf(mpath, sizeof mpath, "%s/%s.txt", g_manifests, name);
    }
    FILE *f = fopen(mpath, "r");
    if (!f) die(line, "missing manifest %s", mpath);
    InstSet *is = &g_insts[g_ninsts++]; memset(is, 0, sizeof *is);
    snprintf(is->name, sizeof is->name, "%s", name);
    is->s = calloc(INST_MAXSMP, sizeof(InstSmp)); if (!is->s) die(line, "out of memory");
    const char *root = g_sampleroot[0] ? g_sampleroot : getenv("SAMPLES");
    if (!root || !root[0]) root = SAMPLEROOT_DEFAULT;
    char lbuf[1024];
    while (fgets(lbuf, sizeof lbuf, f)) {
        InstSmp x; memset(&x, 0, sizeof x); char rawp[MAX_PATH_LEN];
        if (lbuf[0] == '#' || sscanf(lbuf, "%d %d %d %1023[^\r\n]", &x.root, &x.layer, &x.rr, rawp) != 4) continue;
        if (is->n == INST_MAXSMP) die(line, "manifest %s exceeds %d samples", mpath, INST_MAXSMP);
        const char *cut = strstr(rawp, "/soundtrack/samples/");
        if (cut) snprintf(x.path, sizeof x.path, "%s/%s", root, cut + 20);
        else snprintf(x.path, sizeof x.path, "%s", rawp);
        if (x.layer + 1 > is->nl) is->nl = x.layer + 1;
        is->s[is->n++] = x;
    }
    fclose(f);
    if (!is->n) die(line, "empty manifest %s", mpath);
    return is;
}

static void inst_ensure(InstSmp *x, int line) {
    if (x->smp.loaded) return;
    if (!file_exists(x->path)) die(line, "missing instrument sample %s", x->path);
    g_wav_quiet = 1; x->smp.n = wav_read(x->path, &x->smp.l, &x->smp.r); x->smp.sr = g_wav_sr; g_wav_quiet = 0;
    if (x->smp.n < 2) die(line, "empty instrument sample %s", x->path);
    smp_trim(&x->smp);
    // detect sustained vs decaying sample: compare RMS in 70-85% against 15-45%
    long a0 = x->smp.n * 15 / 100, a1 = x->smp.n * 45 / 100, b0 = x->smp.n * 70 / 100, b1 = x->smp.n * 85 / 100;
    double ea = 0, eb = 0;
    for (long i = a0; i < a1; i++) ea += x->smp.l[i] * x->smp.l[i] + x->smp.r[i] * x->smp.r[i];
    for (long i = b0; i < b1; i++) eb += x->smp.l[i] * x->smp.l[i] + x->smp.r[i] * x->smp.r[i];
    double ra = a1 > a0 ? sqrt(ea / (a1 - a0)) : 0, rb = b1 > b0 ? sqrt(eb / (b1 - b0)) : 0;
    x->sustained = (ra > 1e-6 && rb >= ra * 0.15) ? 1 : 0;
    x->smp.loaded = 1;
}

static double inst_corr(const Smp *x, long a, long c, long W) {
    double sab = 0, saa = 0, sbb = 0;
    for (long k = 0; k < W; k++) {
        double u = x->l[a + k] + x->r[a + k], v = x->l[c + k] + x->r[c + k];
        sab += u * v; saa += u * u; sbb += v * v;
    }
    return saa > 1e-12 && sbb > 1e-12 ? sab / sqrt(saa * sbb) : 0;
}

static void inst_note(float *L, float *R, long n, long at, const char *manifest, double midi, double vel, double dur_s, int line, Rng *rg) {
    InstSet *is = inst_load(manifest, line);
    int layer = (int)floor((vel - 1) * is->nl / 127.0);
    if (layer < 0) layer = 0; if (layer >= is->nl) layer = is->nl - 1;
    int best = -1, cand[64], nc = 0; double bd = 1e9;
    for (int pass = 0; pass < 2 && best < 0; pass++) {
        for (int i = 0; i < is->n; i++) {
            InstSmp *x = &is->s[i];
            if (pass == 0 && x->layer != layer) continue;
            double d = fabs(x->root - midi) + (x->root < midi ? 0.01 : 0);
            if (d < bd - 1e-9) { bd = d; best = i; nc = 0; }
            if (fabs(d - bd) < 1e-9 && nc < 64) cand[nc++] = i;
        }
    }
    if (best < 0) return;
    InstSmp *x = &is->s[nc > 1 ? cand[(int)(r01(rg) * nc) % nc] : best];
    inst_ensure(x, line);
    double ratio = pow(2, (midi - x->root) / 12.0) * x->smp.sr / SR;
    double g = db2a(-PIANO_DB_RANGE + PIANO_DB_RANGE * vel / 127.0);
    long N = x->smp.n; double sr = x->smp.sr;
    // Phase-aligned constant-power long loop (matching sampler.h mode 2) to avoid loop bumps/clicks on bowed/sustained samples
    double A = 0.25 * N, B = N - 0.08 * sr, Z = B - A;
    int do_loop = x->sustained && (Z >= 0.20 * sr);
    double X = fmin(fmax(0.35 * Z, 0.08 * sr), 0.22 * sr);
    double period = sr / (440.0 * pow(2, (x->root - 69) / 12.0));
    long W = (long)fmax(2.0 * period, 0.015 * sr);
    double att_s = x->sustained ? INST_SUS_ATTACK_S : INST_ATTACK_S;
    long len = lround((dur_s + INST_RELEASE_S) * SR) + 1;
    double pos = 0, pos2 = 0, w = 0, rc = 0; int xf = 0;
    for (long i = 0; i < len && at + i < n; i++) {
        double t = (double)i / SR;
        double a = t < att_s ? t / att_s : 1.0;
        if (t > dur_s) a *= 0.5 * (1 + cos(M_PI * fmin(1.0, (t - dur_s) / INST_RELEASE_S)));
        if (do_loop && !xf && pos >= B - X) {
            double hi = fmin(A + 0.5 * Z, B - 2 * X - period);
            if (hi < A) hi = A;
            double s = A + r01(rg) * (hi - A);
            long cp = (long)pos, bestc = (long)s; double br = -2.0;
            for (long c = (long)(s - period); c <= (long)(s + period); c++) {
                if (c < 1 || c + W >= N || cp + W >= N) continue;
                double r = inst_corr(&x->smp, cp, c, W);
                if (r > br) { br = r; bestc = c; }
            }
            pos2 = bestc + (pos - cp);
            rc = fmin(fmax(br, 0.0), 1.0);
            w = 0; xf = 1;
        }
        double l, r;
        if (xf) {
            double ca = cos(w * M_PI / 2), cb = sin(w * M_PI / 2), nrm = sqrt(ca * ca + cb * cb + 2 * ca * cb * rc);
            l = (cubic(x->smp.l, N, pos) * ca + cubic(x->smp.l, N, pos2) * cb) / nrm;
            r = (cubic(x->smp.r, N, pos) * ca + cubic(x->smp.r, N, pos2) * cb) / nrm;
        } else {
            if (pos >= N - 1) break;
            l = cubic(x->smp.l, N, pos); r = cubic(x->smp.r, N, pos);
        }
        L[at + i] += (float)(l * a * g); R[at + i] += (float)(r * a * g);
        pos += ratio;
        if (xf) {
            pos2 += ratio; w += ratio / X;
            if (w >= 1.0) { pos = pos2; xf = 0; }
        }
    }
}

static RevCfg hall(double rt) { return (RevCfg){rt, rt * 0.5, 10, 1.4, 200, 6000, 0.14, 1.0, 0.2, 0.25}; }
// gain that makes a send of 0 dB come back as loud as the dry: steady pink noise through the same hall, wet RMS against dry
// RMS (the calibration splice.c uses for a WASH; the raw hall returns about 30 dB less than it is given)
static double hall_cal(double rt) {
    Rev rv; rev_init(&rv, hall(rt)); Rng rg = {0x5eed1234}; double b0 = 0, b1 = 0, b2 = 0, ed = 0, ew = 0;
    long n = (long)((rt * 2.5 + 3) * SR), from = n * 6 / 10;
    for (long i = 0; i < n; i++) {
        double w = rn(&rg); b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
        double x = (b0 + b1 + b2 + w * 0.1848) * 0.2, l, r; rev_tick(&rv, x, x, &l, &r);
        if (i >= from) { ed += x * x; ew += l * l; }
    }
    return sqrt(ed / (ew + 1e-30));
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: glue score.txt out.wav\n"); return 2; }
    parse(argv[1]);
    double spb = 60.0 / g_tempo, end_beats = 0;
    for (int i = 0; i < g_nst; i++) {
        const Stmt *s = &g_st[i];
        double e = s->kind == PIANO ? piano_hold_end(s->beat, s->beat + s->dur) + PIANO_RELEASE_S / spb :
                   s->kind == INST ? s->beat + s->dur + INST_RELEASE_S / spb : s->beat + s->dur;
        if (e > end_beats) end_beats = e;
        if (s->kind == PIANO) piano_probe(s->line);
        if (s->kind == INST) inst_load(s->inst, s->line);
    }
    double len_beats = g_len > 0 ? g_len : end_beats;
    long body = lround(len_beats * spb * SR), n = body + lround(TAIL_S * SR);
    if (n <= 0 || n > 3600L * SR) die(0, "length out of range");
    float *L = calloc(n, sizeof(float)), *R = calloc(n, sizeof(float)); if (!L || !R) die(0, "out of memory");
    Rng rg = {g_seed};
    for (int i = 0; i < g_nst; i++) {
        const Stmt *s = &g_st[i];
        if (s->kind == PIANO) {
            long pat = lround(s->beat * spb * SR); double hold = (piano_hold_end(s->beat, s->beat + s->dur) - s->beat) * spb;
            if (pat < n) for (int k = 0; k < s->n; k++) piano_note(L, R, n, pat, s->midi[k], s->vel, hold, s->line);
            continue;
        }
        if (s->kind == INST) {
            long iat = lround(s->beat * spb * SR); double dur_s = s->dur * spb;
            if (iat < n) for (int k = 0; k < s->n; k++) inst_note(L, R, n, iat, s->inst, s->midi[k], s->vel, dur_s, s->line, &rg);
            continue;
        }
        double rel = s->kind == PAD ? PAD_RELEASE_S : s->kind == KEYS ? KEYS_RELEASE_S : s->kind == BRASS ? BRASS_RELEASE_S : SUB_RELEASE_S;
        double dur_s = s->dur * spb; long at = lround(s->beat * spb * SR), len = lround((dur_s + rel) * SR) + 1;
        if (at >= n) continue;
        Buf b = buf_new(at, len);
        for (int k = 0; k < s->n; k++) {
            if (s->kind == PAD) pad_note(&b, s->midi[k], dur_s, &rg);
            else if (s->kind == KEYS) keys_note(&b, s->midi[k], dur_s);
            else if (s->kind == BRASS) brass_note(&b, s->midi[k], dur_s, &rg);
            else sub_note(&b, s->midi[k], dur_s);
        }
        double pk = 0; for (long j = 0; j < b.n; j++) { pk = fmax(pk, fabs(b.l[j])); pk = fmax(pk, fabs(b.r[j])); }
        double g = pk > 0 ? db2a(s->db) / pk : 0;
        for (long j = 0; j < b.n && at + j < n; j++) { L[at + j] += (float)(b.l[j] * g); R[at + j] += (float)(b.r[j] * g); }
        free(b.l); free(b.r);
    }
    Rev rv; rev_init(&rv, hall(REVERB_RT));
    double sg = hall_cal(REVERB_RT) * db2a(g_rev_db);
    for (long i = 0; i < n; i++) { double wl, wr; rev_tick(&rv, L[i], R[i], &wl, &wr); L[i] += (float)(wl * sg); R[i] += (float)(wr * sg); }
    double pk = 0; for (long i = 0; i < n; i++) { pk = fmax(pk, fabs(L[i])); pk = fmax(pk, fabs(R[i])); }
    if (pk > db2a(PEAK_CEIL_DB)) {
        double g = db2a(PEAK_CEIL_DB) / pk;
        for (long i = 0; i < n; i++) { L[i] *= (float)g; R[i] *= (float)g; }
        fprintf(stderr, "glue: peak %.2f dBFS, whole file scaled by %.2f dB\n", a2db(pk), a2db(g));
    }
    wav_write24(argv[2], L, R, n);
    fprintf(stderr, "glue: %.2f s, %d statements, seed %llu\n", (double)n / SR, g_nst, (unsigned long long)g_seed);
    return 0;
}
