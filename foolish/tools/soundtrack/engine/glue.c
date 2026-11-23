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
// beat and dur are in beats at TEMPO, midi may be fractional, db is the peak level of that statement in dBFS before the
// reverb. Output: 48 kHz stereo 24 bit, scaled as a whole when its peak would pass -1 dBFS (the scale is printed on stderr).
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

typedef enum { PAD, KEYS, SUBV } Kind;
typedef struct { Kind kind; int line; double beat, dur, db; int n; double midi[MAX_NOTES]; } Stmt;

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
static double g_tempo, g_len = -1, g_rev_db = -10, g_cutoff = 1800; static uint64_t g_seed = GLUE_SEED;

static void parse(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(1); }
    char buf[4096]; int line = 0, seen_tempo = 0, seen_len = 0, seen_rev = 0, seen_cut = 0, seen_seed = 0;
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
        else if (!strcmp(k, "PAD") || !strcmp(k, "KEYS") || !strcmp(k, "SUB")) {
            if (g_nst == MAX_STMT) die(line, "too many notes");
            Stmt *s = &g_st[g_nst++]; memset(s, 0, sizeof *s); s->line = line;
            s->kind = !strcmp(k, "PAD") ? PAD : !strcmp(k, "KEYS") ? KEYS : SUBV;
            ARGS(4);
            s->beat = num(line, tok[1], "beat", 0, 1e6);
            s->dur = num(line, tok[2], "duration", 0.001, 1e5);
            { size_t ln = strlen(tok[3]); if (tok[3][0] == ',' || tok[3][ln - 1] == ',' || strstr(tok[3], ",,")) die(line, "bad note list '%s'", tok[3]); }
            char *sp2 = NULL; char *ml = tok[3];
            for (char *m = strtok_r(ml, ",", &sp2); m; m = strtok_r(NULL, ",", &sp2)) {
                if (s->n == MAX_NOTES) die(line, "more than %d notes", MAX_NOTES);
                s->midi[s->n++] = num(line, m, "midi note", 0, 127);
            }
            if (s->kind == SUBV && s->n != 1) die(line, "SUB takes one note");
            s->db = num(line, tok[4], "level", -150, 0);
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
        if (s->beat + s->dur > end_beats) end_beats = s->beat + s->dur;
    }
    double len_beats = g_len > 0 ? g_len : end_beats;
    long body = lround(len_beats * spb * SR), n = body + lround(TAIL_S * SR);
    if (n <= 0 || n > 3600L * SR) die(0, "length out of range");
    float *L = calloc(n, sizeof(float)), *R = calloc(n, sizeof(float)); if (!L || !R) die(0, "out of memory");
    Rng rg = {g_seed};
    for (int i = 0; i < g_nst; i++) {
        const Stmt *s = &g_st[i];
        double rel = s->kind == PAD ? PAD_RELEASE_S : s->kind == KEYS ? KEYS_RELEASE_S : SUB_RELEASE_S;
        double dur_s = s->dur * spb; long at = lround(s->beat * spb * SR), len = lround((dur_s + rel) * SR) + 1;
        if (at >= n) continue;
        Buf b = buf_new(at, len);
        for (int k = 0; k < s->n; k++) {
            if (s->kind == PAD) pad_note(&b, s->midi[k], dur_s, &rg);
            else if (s->kind == KEYS) keys_note(&b, s->midi[k], dur_s);
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
