// splice: renders one song from stretches of existing recordings, by a cue sheet written as text.
//   splice cue.txt out.wav [timeline.tsv [seams.tsv]]
// Sources of any format are decoded through ffmpeg (pipe, 48 kHz stereo float), so sample positions equal what an
// analysis of the same file sees. One statement per line, '#' starts a comment; an unknown statement or key, or a bad
// value, is a fatal error naming the line.
//   SRC name /abs/path [FROM /abs/original]   FROM: the file the path was derived from (a stem); the timeline prints it,
//                                             and a clip's loudness is measured on it, so stems of one recording get the
//                                             gain of the full mix and sum back to it
//   LEVEL lufs|off                            level of the following lines (default -16)
//   MASTER lufs dbtp                          default -16 -1.5
//   TAILS rt:db:len|off                       every following CLIP without a WASH of its own, and without DRY 1, gets
//                                             that WASH: what is cut off rings on under the next attack (high-passed by
//                                             the hall), so a join is dry only where the cue says so. LAYERs are not touched.
//   CLIP  name a b [key value]...             a, b: cut points in source seconds (b may be `end`)
//   LAYER name a b [key value]...
//   FX kind AT|OFF x LEN seconds DB level [BEAT seconds]
//                                             a synthesized one-shot on top of the mix (never in the timeline or the seams).
//                                             kind: CRASH (cymbal from the anchor, -60 dB at LEN), REVCYM (the same reversed,
//                                             loudest at the end: AT -LEN lands it on a seam), RISER (band-passed noise
//                                             sweeping 300 Hz to 9 kHz, +24 dB, cut at the end), DOWN (9 kHz to 200 Hz, -30 dB),
//                                             BOOM (mono sub impact, 110 to 38 Hz, -60 dB at LEN), ROLL (snare roll, needs BEAT:
//                                             hits every half beat for the first half, every quarter for the next quarter,
//                                             every eighth for the last, rising -18 to 0 dB). DB: the loudest 400 ms window
//                                             (hop 100 ms, plus the one ending at the end) of the effect, K-weighted as in
//                                             lufs(), set to that level before the master gain (the domain of a CLIP's LUFS).
//   BUS LP|HP AT|OFF x CURVE t:hz,t:hz,...    a 24 dB/oct filter (two RBJ biquads, Q 0.707, coefficients every 32 samples, state
//                                             kept) over the whole summed mix (clips, layers, washes, echoes), after the sum and
//                                             before the FX; t in seconds from the anchor, ascending, 2 or more points, hz
//                                             interpolated logarithmically. The first and last points must be open (LP >= 16000,
//                                             HP <= 30); dry crossfades to wet over 20 ms at each edge, the filter runs 0.5 s
//                                             before the first point to settle, and outside [first t, last t] the mix is
//                                             untouched. Two regions of one type may not overlap.
// FX and BUS are anchored like a LAYER: AT x is seconds from the b of the latest preceding CLIP (song time), OFF x from its a;
// exactly one is required, x may be negative, and they never move the CLIP chain.
// Placement: a CLIP's `a` lands at the previous CLIP's `b` + GAP (exact to a sample, whatever PRE, POST, fades or effects).
// A LAYER never moves that chain: its `a` lands at the `b` of the latest preceding CLIP + AT, or at that CLIP's `a` + OFF.
// With RATE r a source span s lasts s/r song seconds. A clip is audible from a-PRE to b+POST in the source.
// Keys (CLIP and LAYER): PRE POST FIN FOUT LUFS GAIN RIDE BASS EQ LPOUT LPIN HPOUT HPIN WASH ECHO SWELL RATE SEMI VARI
//   BRAKE ROLE JOIN; CLIP only: GAP DRY; LAYER only: AT OFF (not both).
// Order of processing: rate/pitch, DC removal, level and GAIN and RIDE, BASS, EQ, filters, effect sends (pre-fade), fades, sum.
// The only random source is the Rng of dsp.h: the pink noise that calibrates the reverb sends (a fixed seed), and the noise of
// the FX, whose seed is FX_SEED and the ordinal of the statement among the FX.
// Sources are decoded when first needed and freed after their last statement, so a long song with many sources stays small.
#include <AudioToolbox/AudioToolbox.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/wait.h>

// the shared headers carry helpers this tool does not use
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "mix.h"
#pragma clang diagnostic pop

// whole numbers of samples, worked out once here and never inside a floating point expression
static const long LEAD = SR / 2;     // settling run before the audible start (DC removal, bands, sweeps)
static const long APAD = SR * 3 / 2; // real padding either side of an AudioToolbox render

static void die(int line, const char *fmt, ...) __attribute__((noreturn));
static void die(int line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (line > 0) fprintf(stderr, "splice: line %d: ", line); else fprintf(stderr, "splice: ");
    vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap); exit(1);
}
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) die(0, "out of memory (%zu bytes)", n); return q; }
static void *xcalloc(size_t n, size_t m) { void *q = calloc(n ? n : 1, m ? m : 1); if (!q) die(0, "out of memory (%zu bytes)", n * m); return q; }

// ---------------------------------------------------------------- sources
typedef struct {
    char name[64]; char *path, *from; long n, nfrom; int line, last_use;
    float *L, *R, *FL, *FR;
} Src;
static Src *g_src; static int g_nsrc, g_csrc;

static void shquote(const char *s, char *out, size_t cap) {
    size_t k = 0; out[k++] = '\'';
    for (; *s && k + 6 < cap; s++) { if (*s == '\'') { memcpy(out + k, "'\\''", 4); k += 4; } else out[k++] = *s; }
    out[k++] = '\''; out[k] = 0;
}
// decode PATH through ffmpeg; keep the samples unless Lp is null (then only count). Fatal on a failed run or zero frames.
static long decode(const char *path, float **Lp, float **Rp, int line) {
    char q[4096], cmd[8192]; shquote(path, q, sizeof q);
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -nostdin -i %s -f f32le -ar 48000 -ac 2 -", q);
    FILE *p = popen(cmd, "r"); if (!p) die(line, "cannot run ffmpeg for %s", path);
    long n = 0, cap = 0; float *L = 0, *R = 0; static float buf[2 * 16384];
    size_t have = 0;
    for (;;) {
        size_t got = fread((char *)buf + have, 1, sizeof buf - have, p);
        if (got == 0) break;
        have += got; long fr = (long)(have / 8);
        if (Lp) {
            if (n + fr > cap) { cap = (n + fr) * 2 + 65536; L = xrealloc(L, cap * sizeof(float)); R = xrealloc(R, cap * sizeof(float)); }
            for (long i = 0; i < fr; i++) { L[n + i] = buf[2 * i]; R[n + i] = buf[2 * i + 1]; }
        }
        n += fr; size_t used = fr * 8; memmove(buf, (char *)buf + used, have - used); have -= used;
    }
    int st = pclose(p);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || have != 0) die(line, "ffmpeg failed on %s", path);
    if (n == 0) die(line, "%s decodes to zero frames", path);
    if (Lp) { *Lp = L; *Rp = R; }
    return n;
}
static void src_load(Src *s) { if (!s->L) s->n = decode(s->path, &s->L, &s->R, s->line); }
static void src_load_from(Src *s) { if (!s->FL) s->nfrom = decode(s->from, &s->FL, &s->FR, s->line); }
static void src_free(Src *s) { free(s->L); free(s->R); free(s->FL); free(s->FR); s->L = s->R = s->FL = s->FR = 0; }

// ---------------------------------------------------------------- statements
typedef struct { double *t, *db; int n; } Curve;
typedef struct { double s, hz; int set; } Sweep;
typedef struct {
    int kind, line, src, b_end, idx;           // kind 0 CLIP, 1 LAYER
    double a, b, gap, at, off; int has_at, has_off;
    double pre, post; int pre_set;
    double fin, fout;
    double level; int lufs_set; double lufs_v; int lufs_off;
    double gain; Curve ride, bass; double eq[3]; int has_eq;
    Sweep sw[4];                                // 0 LPOUT 1 LPIN 2 HPOUT 3 HPIN
    int wash; double w_rt, w_db, w_len;
    int dry;
    int echo; double e_d, e_fb, e_db, e_len;
    int swell; double s_len, s_db;
    double rate, semi; int vari; double brake;
    char *role, *join;
    // layout, in song samples before the global shift
    long ia, startS, endS, Nc, pre_s, mid_s, post_s, sa, sb, sw_len, tail, bs;
    double src_in, src_out, lev_db, meas;
} Stmt;
static Stmt *g_st; static int g_nst, g_cst;

static double num(const char *s, int line, const char *what) {
    char *e; double v = strtod(s, &e);
    if (e == s || *e || !isfinite(v)) die(line, "bad number '%s' for %s", s, what);
    return v;
}
static void curve_parse(Curve *c, const char *s, int line, const char *what) {
    char *d = strdup(s), *sv = 0; int n = 0;
    for (char *p = strtok_r(d, ",", &sv); p; p = strtok_r(0, ",", &sv)) {
        char *col = strchr(p, ':'); if (!col) die(line, "%s wants t:db,t:db,...", what);
        *col = 0; c->t = xrealloc(c->t, (n + 1) * sizeof(double)); c->db = xrealloc(c->db, (n + 1) * sizeof(double));
        c->t[n] = num(p, line, what); c->db[n] = num(col + 1, line, what);
        if (n && c->t[n] <= c->t[n - 1]) die(line, "%s times must ascend", what);
        n++;
    }
    if (!n) die(line, "%s is empty", what);
    c->n = n; free(d);
}
static void colon_parse(const char *s, double *v, int nv, int line, const char *what) {
    char *d = strdup(s), *sv = 0; int n = 0;
    for (char *p = strtok_r(d, ":", &sv); p; p = strtok_r(0, ":", &sv)) { if (n >= nv) die(line, "%s has too many fields", what); v[n++] = num(p, line, what); }
    if (n != nv) die(line, "%s wants %d fields separated by ':'", what, nv);
    free(d);
}
static int src_find(const char *n, int line) {
    for (int i = 0; i < g_nsrc; i++) if (!strcmp(g_src[i].name, n)) return i;
    die(line, "no SRC named %s", n);
}
// FX and BUS statements, resolved against the layout in main
enum { FX_CRASH, FX_REVCYM, FX_RISER, FX_DOWN, FX_BOOM, FX_ROLL, FX_KINDS };
static const char *fxname[FX_KINDS] = {"CRASH", "REVCYM", "RISER", "DOWN", "BOOM", "ROLL"};
typedef struct { int line, clip, kind, has_at, has_off, has_len, has_db, has_beat; double x, len, db, beat; long a, n; } Fx;
typedef struct { int line, clip, lp, has_at, has_off, has_curve; double x; Curve c; long a, r0, r1; } Bus;   // c.db holds ln(hz)
static Fx *g_fx; static int g_nfx; static Bus *g_bus; static int g_nbus;
static const double BUS_OPEN_LP = 16000, BUS_OPEN_HP = 30, BUS_HZ_MIN = 10, BUS_HZ_MAX = 20000;

static void parse_fx(char **tok, int nt, int ln, int last_clip) {
    if (nt < 2 || nt % 2) die(ln, "FX kind AT|OFF x LEN seconds DB level [BEAT seconds]");
    if (last_clip < 0) die(ln, "FX before any CLIP");
    g_fx = xrealloc(g_fx, (g_nfx + 1) * sizeof(Fx)); Fx *f = &g_fx[g_nfx++]; memset(f, 0, sizeof *f); f->line = ln; f->clip = last_clip; f->kind = -1;
    for (int k = 0; k < FX_KINDS; k++) if (!strcmp(tok[1], fxname[k])) f->kind = k;
    if (f->kind < 0) die(ln, "unknown FX kind %s", tok[1]);
    for (int i = 2; i < nt; i += 2) {
        const char *k = tok[i], *v = tok[i + 1];
        if (!strcmp(k, "AT") || !strcmp(k, "OFF")) {
            if (f->has_at || f->has_off) die(ln, "FX wants exactly one of AT and OFF");
            f->x = num(v, ln, k); if (k[0] == 'A') f->has_at = 1; else f->has_off = 1;
        } else if (!strcmp(k, "LEN")) { if (f->has_len) die(ln, "LEN twice"); f->len = num(v, ln, k); f->has_len = 1; }
        else if (!strcmp(k, "DB")) { if (f->has_db) die(ln, "DB twice"); f->db = num(v, ln, k); f->has_db = 1; }
        else if (!strcmp(k, "BEAT")) { if (f->has_beat) die(ln, "BEAT twice"); f->beat = num(v, ln, k); f->has_beat = 1; }
        else die(ln, "unknown key %s on FX", k);
    }
    if (!f->has_at && !f->has_off) die(ln, "FX wants AT or OFF");
    if (!f->has_len) die(ln, "FX wants LEN");
    if (!f->has_db) die(ln, "FX wants DB");
    if (!(f->len >= 0.05)) die(ln, "FX LEN must be at least 0.05 s");
    if (f->kind == FX_ROLL && !(f->has_beat && f->beat > 0)) die(ln, "ROLL wants BEAT seconds > 0");
    if (f->kind != FX_ROLL && f->has_beat) die(ln, "BEAT only goes with ROLL");
}
static void parse_bus(char **tok, int nt, int ln, int last_clip) {
    if (nt != 6) die(ln, "BUS LP|HP AT|OFF x CURVE t:hz,t:hz,...");
    if (last_clip < 0) die(ln, "BUS before any CLIP");
    g_bus = xrealloc(g_bus, (g_nbus + 1) * sizeof(Bus)); Bus *b = &g_bus[g_nbus++]; memset(b, 0, sizeof *b); b->line = ln; b->clip = last_clip;
    if (!strcmp(tok[1], "LP")) b->lp = 1; else if (strcmp(tok[1], "HP")) die(ln, "BUS wants LP or HP, not %s", tok[1]);
    for (int i = 2; i < nt; i += 2) {
        const char *k = tok[i], *v = tok[i + 1];
        if (!strcmp(k, "AT") || !strcmp(k, "OFF")) {
            if (b->has_at || b->has_off) die(ln, "BUS wants exactly one of AT and OFF");
            b->x = num(v, ln, k); if (k[0] == 'A') b->has_at = 1; else b->has_off = 1;
        } else if (!strcmp(k, "CURVE")) { if (b->has_curve) die(ln, "CURVE twice"); curve_parse(&b->c, v, ln, k); b->has_curve = 1; }
        else die(ln, "unknown key %s on BUS", k);
    }
    if (!b->has_at && !b->has_off) die(ln, "BUS wants AT or OFF");
    if (!b->has_curve) die(ln, "BUS wants CURVE");
    if (b->c.n < 2) die(ln, "BUS CURVE wants at least 2 points");
    for (int i = 0; i < b->c.n; i++) if (b->c.db[i] < BUS_HZ_MIN || b->c.db[i] > BUS_HZ_MAX) die(ln, "BUS CURVE hz must be within %g..%g", BUS_HZ_MIN, BUS_HZ_MAX);
    double h0 = b->c.db[0], h1 = b->c.db[b->c.n - 1];
    if (b->lp ? (h0 < BUS_OPEN_LP || h1 < BUS_OPEN_LP) : (h0 > BUS_OPEN_HP || h1 > BUS_OPEN_HP))
        die(ln, "BUS %s CURVE must start and end open (%s %g Hz)", tok[1], b->lp ? ">=" : "<=", b->lp ? BUS_OPEN_LP : BUS_OPEN_HP);
    for (int i = 0; i < b->c.n; i++) b->c.db[i] = log(b->c.db[i]);
}
static const char *swname[4] = {"LPOUT", "LPIN", "HPOUT", "HPIN"};

static void parse(const char *path, double *m_lufs, double *m_tp) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(1); }
    char *line = 0; size_t cap = 0; int ln = 0; double level = -16, tails[3] = {0}; int level_on = 1, tails_on = 0, last_clip = -1;
    while (getline(&line, &cap, f) >= 0) {
        ln++; char *h = strchr(line, '#'); if (h) *h = 0;
        char **tok = 0; int nt = 0, ct = 0;
        for (char *sv = 0, *p = strtok_r(line, " \t\r\n", &sv); p; p = strtok_r(0, " \t\r\n", &sv)) {
            if (nt == ct) { ct = ct * 2 + 16; tok = xrealloc(tok, ct * sizeof(char *)); }
            tok[nt++] = p;
        }
        if (!nt) { free(tok); continue; }
        if (!strcmp(tok[0], "SRC")) {
            if (nt != 3 && nt != 5) die(ln, "SRC name path [FROM path]");
            if (nt == 5 && strcmp(tok[3], "FROM")) die(ln, "unknown key %s on SRC", tok[3]);
            if (strlen(tok[1]) >= 64) die(ln, "SRC name too long");
            for (int i = 0; i < g_nsrc; i++) if (!strcmp(g_src[i].name, tok[1])) die(ln, "duplicate SRC %s", tok[1]);
            if (g_nsrc == g_csrc) { g_csrc = g_csrc * 2 + 16; g_src = xrealloc(g_src, g_csrc * sizeof(Src)); }
            Src *s = &g_src[g_nsrc++]; memset(s, 0, sizeof *s); strcpy(s->name, tok[1]); s->path = strdup(tok[2]);
            s->from = nt == 5 ? strdup(tok[4]) : 0; s->line = ln; s->last_use = -1;
        } else if (!strcmp(tok[0], "LEVEL")) {
            if (nt != 2) die(ln, "LEVEL lufs|off");
            if (!strcmp(tok[1], "off")) level_on = 0; else { level_on = 1; level = num(tok[1], ln, "LEVEL"); }
        } else if (!strcmp(tok[0], "TAILS")) {
            if (nt != 2) die(ln, "TAILS rt:db:len|off");
            if (!strcmp(tok[1], "off")) tails_on = 0;
            else {
                colon_parse(tok[1], tails, 3, ln, "TAILS"); tails_on = 1;
                if (tails[0] <= 0 || tails[2] <= 0) die(ln, "TAILS wants rt > 0 and len > 0");
            }
        } else if (!strcmp(tok[0], "MASTER")) {
            if (nt != 3) die(ln, "MASTER lufs dbtp");
            *m_lufs = num(tok[1], ln, "MASTER"); *m_tp = num(tok[2], ln, "MASTER");
        } else if (!strcmp(tok[0], "CLIP") || !strcmp(tok[0], "LAYER")) {
            if (nt < 4 || (nt - 4) % 2) die(ln, "%s name a b [key value]...", tok[0]);
            if (g_nst == g_cst) { g_cst = g_cst * 2 + 64; g_st = xrealloc(g_st, g_cst * sizeof(Stmt)); }
            Stmt *c = &g_st[g_nst++]; memset(c, 0, sizeof *c); c->kind = tok[0][1] == 'A' ? 1 : 0; c->line = ln;
            c->src = src_find(tok[1], ln); c->a = num(tok[2], ln, "a");
            if (!strcmp(tok[3], "end")) c->b_end = 1; else c->b = num(tok[3], ln, "b");
            c->level = level_on ? level : NAN; c->rate = 1; c->fin = c->fout = 0.008;
            for (int i = 4; i < nt; i += 2) {
                const char *k = tok[i], *v = tok[i + 1];
                #define KEYD(NAME, FIELD, SETF) if (!strcmp(k, NAME)) { c->FIELD = num(v, ln, NAME); SETF; continue; }
                if (!strcmp(k, "GAP") && c->kind == 0) { c->gap = num(v, ln, k); continue; }
                if (!strcmp(k, "DRY") && c->kind == 0) { if (num(v, ln, k) != 1) die(ln, "DRY wants 1"); c->dry = 1; continue; }
                if (!strcmp(k, "AT") && c->kind == 1) { if (c->has_off) die(ln, "AT and OFF together"); c->at = num(v, ln, k); c->has_at = 1; continue; }
                if (!strcmp(k, "OFF") && c->kind == 1) { if (c->has_at) die(ln, "AT and OFF together"); c->off = num(v, ln, k); c->has_off = 1; continue; }
                KEYD("PRE", pre, c->pre_set = 1) KEYD("POST", post, (void)0) KEYD("FIN", fin, (void)0) KEYD("FOUT", fout, (void)0)
                KEYD("GAIN", gain, (void)0) KEYD("SEMI", semi, (void)0) KEYD("RATE", rate, (void)0) KEYD("BRAKE", brake, (void)0)
                #undef KEYD
                if (!strcmp(k, "LUFS")) { c->lufs_set = 1; if (!strcmp(v, "off")) c->lufs_off = 1; else c->lufs_v = num(v, ln, k); continue; }
                if (!strcmp(k, "RIDE")) { curve_parse(&c->ride, v, ln, k); continue; }
                if (!strcmp(k, "BASS")) { curve_parse(&c->bass, v, ln, k); continue; }
                if (!strcmp(k, "EQ")) { colon_parse(v, c->eq, 3, ln, k); c->has_eq = 1; continue; }
                int hit = 0;
                for (int j = 0; j < 4; j++) if (!strcmp(k, swname[j])) { double w[2]; colon_parse(v, w, 2, ln, k); c->sw[j] = (Sweep){w[0], w[1], 1}; hit = 1; }
                if (hit) continue;
                if (!strcmp(k, "WASH")) { double w[3]; colon_parse(v, w, 3, ln, k); c->wash = 1; c->w_rt = w[0]; c->w_db = w[1]; c->w_len = w[2]; continue; }
                if (!strcmp(k, "ECHO")) { double w[4]; colon_parse(v, w, 4, ln, k); c->echo = 1; c->e_d = w[0]; c->e_fb = w[1]; c->e_db = w[2]; c->e_len = w[3]; continue; }
                if (!strcmp(k, "SWELL")) { double w[2]; colon_parse(v, w, 2, ln, k); c->swell = 1; c->s_len = w[0]; c->s_db = w[1]; continue; }
                if (!strcmp(k, "VARI")) { if (num(v, ln, k) != 1) die(ln, "VARI wants 1"); c->vari = 1; continue; }
                if (!strcmp(k, "ROLE") || !strcmp(k, "JOIN")) {
                    char *d = strdup(v); for (char *p = d; *p; p++) if (*p == '_') *p = ' ';
                    if (k[0] == 'R') c->role = d; else c->join = d;
                    continue;
                }
                die(ln, "unknown key %s on %s", k, tok[0]);
            }
            if (c->dry && c->wash) die(ln, "DRY and WASH together");
            if (c->kind == 0 && tails_on && !c->dry && !c->wash) { c->wash = 1; c->w_rt = tails[0]; c->w_db = tails[1]; c->w_len = tails[2]; }
            if (!(c->rate > 0)) die(ln, "RATE must be positive");
            if (c->vari && c->rate == 1) die(ln, "VARI needs a RATE other than 1");
            if (c->vari && c->semi != 0) die(ln, "VARI and SEMI together");
            if (c->fin < 0 || c->fout < 0 || c->post < 0 || (c->pre_set && c->pre < 0) || c->brake < 0) die(ln, "negative length");
            if (c->wash && (c->w_rt <= 0 || c->w_len <= 0)) die(ln, "WASH wants rt > 0 and len > 0");
            if (c->echo && (c->e_d <= 0 || c->e_fb <= 0 || c->e_fb >= 1 || c->e_len <= 0)) die(ln, "ECHO wants delay > 0, 0 < fb < 1, len > 0");
            if (c->swell && c->s_len <= 0) die(ln, "SWELL wants len > 0");
            for (int j = 0; j < 4; j++) if (c->sw[j].set && (c->sw[j].s <= 0 || c->sw[j].hz < 20 || c->sw[j].hz > 20000)) die(ln, "%s wants s > 0 and 20 <= hz <= 20000", swname[j]);
            if (fabs(c->semi) > 24) die(ln, "SEMI out of range");
            g_src[c->src].last_use = g_nst - 1;
            if (c->kind == 0) last_clip = g_nst - 1;
        } else if (!strcmp(tok[0], "FX")) parse_fx(tok, nt, ln, last_clip);
        else if (!strcmp(tok[0], "BUS")) parse_bus(tok, nt, ln, last_clip);
        else die(ln, "unknown statement %s", tok[0]);
        free(tok);
    }
    free(line); fclose(f);
}

// ---------------------------------------------------------------- the position map of one clip
// stream index i = k + LEAD, k = song sample from the audible start. u(k): source sample (double) heard at k.
typedef struct { double sa, pre_s, r; long Nc, bs; int au; double p1, c1, reff, sx; } Map;
static inline double map_w(const Map *m, double k) {
    double s0 = (double)(m->Nc - m->bs);
    if (m->bs <= 0 || k <= s0) return k;
    double u = k - s0; return s0 + u - u * u / (2.0 * m->bs);
}
static inline double map_u(const Map *m, double k) { return m->sa + (map_w(m, k) - m->pre_s) * m->r; }
static double map_pos(const void *c, long i) {
    const Map *m = c; double u = map_u(m, (double)(i - LEAD));
    return m->au ? m->p1 + (u - m->sx - m->c1) / m->reff : u;
}

// windowed-sinc resampling: out[i] = in(pos(i)), zero outside; low-passed when it reads faster than 1
static void sinc_resample(const float *in, long nin, long nout, double (*pf)(const void *, long), const void *ctx, float *out) {
    for (long i = 0; i < nout; i++) {
        double p = pf(ctx, i), v = fabs(pf(ctx, i + 1) - p); if (v < 1e-9) v = 1e-9;
        double fc = v > 1 ? 1.0 / v : 1.0, hw = 16.0 / fc;
        long c = (long)floor(p); double fr = p - (double)c;
        if (fc == 1.0 && fr < 1e-9) { out[i] = (c >= 0 && c < nin) ? in[c] : 0; continue; }
        long lo = (long)ceil(p - hw), hi = (long)floor(p + hw); double acc = 0;
        for (long j = lo; j <= hi; j++) {
            if (j < 0 || j >= nin) continue;
            double x = (double)j - p, ax = fabs(x); if (ax >= hw) continue;
            double s = ax < 1e-12 ? 1.0 : sin(M_PI * fc * x) / (M_PI * fc * x);
            acc += in[j] * fc * s * (0.5 + 0.5 * cos(M_PI * x / hw));
        }
        out[i] = (float)acc;
    }
}

// ---------------------------------------------------------------- AudioToolbox NewTimePitch
typedef struct { const float *L, *R; long n, pos; } Feed;
static OSStatus au_cb(void *ref, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 nf, AudioBufferList *io) {
    (void)f; (void)ts; (void)bus; Feed *fd = ref; float *l = io->mBuffers[0].mData, *r = io->mBuffers[1].mData;
    for (UInt32 i = 0; i < nf; i++) { long p = fd->pos + i; l[i] = p < fd->n ? fd->L[p] : 0; r[i] = p < fd->n ? fd->R[p] : 0; }
    fd->pos += nf; return noErr;
}
static void au_run(const float *inL, const float *inR, long nin, double rate, double cents, long nout, float *oL, float *oR) {
    AudioComponentDescription d = {kAudioUnitType_FormatConverter, kAudioUnitSubType_NewTimePitch, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(NULL, &d); AudioUnit au;
    if (!c || AudioComponentInstanceNew(c, &au)) die(0, "NewTimePitch is not available");
    AudioStreamBasicDescription a = {SR, kAudioFormatLinearPCM, kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved, 4, 1, 4, 2, 32, 0};
    UInt32 mf = 4096, on = 1, rq = kRenderQuality_Max; Feed fd = {inL, inR, nin, 0}; AURenderCallbackStruct cs = {au_cb, &fd};
    OSStatus s = 0;
    s |= AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &a, sizeof a);
    s |= AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &a, sizeof a);
    s |= AudioUnitSetProperty(au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &mf, sizeof mf);
    s |= AudioUnitSetProperty(au, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &on, sizeof on);
    s |= AudioUnitSetProperty(au, kAudioUnitProperty_RenderQuality, kAudioUnitScope_Global, 0, &rq, sizeof rq);
    s |= AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cs, sizeof cs);
    s |= AudioUnitSetParameter(au, kNewTimePitchParam_Rate, kAudioUnitScope_Global, 0, rate, 0);
    s |= AudioUnitSetParameter(au, kNewTimePitchParam_Pitch, kAudioUnitScope_Global, 0, cents, 0);
    s |= AudioUnitSetParameter(au, kNewTimePitchParam_Smoothness, kAudioUnitScope_Global, 0, 32, 0);
    s |= AudioUnitInitialize(au);
    if (s) die(0, "NewTimePitch setup failed (%d)", (int)s);
    AudioTimeStamp ts; memset(&ts, 0, sizeof ts); ts.mFlags = kAudioTimeStampSampleTimeValid;
    struct { AudioBufferList l; AudioBuffer b; } abl;
    for (long got = 0; got < nout;) {
        UInt32 nf = (UInt32)(nout - got < 4096 ? nout - got : 4096);
        abl.l.mNumberBuffers = 2; abl.l.mBuffers[0] = (AudioBuffer){1, nf * 4, oL + got}; abl.b = (AudioBuffer){1, nf * 4, oR + got};
        AudioUnitRenderActionFlags fl = 0; ts.mSampleTime = (double)got;
        s = AudioUnitRender(au, &fl, &ts, 0, nf, &abl.l); if (s) die(0, "NewTimePitch render failed (%d)", (int)s);
        got += nf;
    }
    AudioUnitUninitialize(au); AudioComponentInstanceDispose(au);
}
// The unit quantises its rate, so the speed it really runs at is measured (two clicks 10 s apart) and the resampling that
// follows takes up the difference; the pitch is trimmed by the same amount so the semitones stay exact.
typedef struct { double rate, semi, cents, reff, p1, c1; } AuCal;
static AuCal *g_cal; static int g_ncal, g_ccal;
static AuCal au_probe(double rate, double cents) {
    const long c1 = SR * 2, c2 = c1 + SR * 10, nin = c2 + SR * 2, nout = (long)((double)nin / rate) + SR * 2;
    float *in = xcalloc(nin, sizeof(float)), *oL = xcalloc(nout, sizeof(float)), *oR = xcalloc(nout, sizeof(float));
    in[c1] = 1; in[c2] = 1; au_run(in, in, nin, rate, cents, nout, oL, oR);
    double p[2];      // where the energy of each click lands (its centroid: the peak of a smeared click can sit on either lobe)
    for (int k = 0; k < 2; k++) {
        long ctr = (long)((double)(k ? c2 : c1) / rate), lo = ctr - SR * 3 / 10 > 0 ? ctr - SR * 3 / 10 : 0, hi = ctr + SR * 3 / 10; if (hi > nout) hi = nout;
        double e = 0, w = 0; for (long i = lo; i < hi; i++) { double x = (double)oL[i] * oL[i]; e += x; w += x * (double)i; }
        p[k] = e > 0 ? w / e : (double)ctr;
    }
    free(in); free(oL); free(oR);
    return (AuCal){rate, 0, cents, (double)(c2 - c1) / (p[1] - p[0]), p[0], (double)c1};
}
static AuCal au_calibrate(double rate, double semi) {
    for (int i = 0; i < g_ncal; i++) if (g_cal[i].rate == rate && g_cal[i].semi == semi) return g_cal[i];
    AuCal c = au_probe(rate, semi * 100);
    for (int it = 0; it < 2; it++) c = au_probe(rate, semi * 100 - 1200 * log2(rate / c.reff));
    c.semi = semi;
    if (g_ncal == g_ccal) { g_ccal = g_ccal * 2 + 8; g_cal = xrealloc(g_cal, g_ccal * sizeof(AuCal)); }
    g_cal[g_ncal++] = c; return c;
}

// ---------------------------------------------------------------- helpers
static double curve_at(const Curve *c, double t, int *seg) {
    if (t <= c->t[0]) return c->db[0];
    if (t >= c->t[c->n - 1]) return c->db[c->n - 1];
    while (*seg + 1 < c->n - 1 && t >= c->t[*seg + 1]) (*seg)++;
    while (*seg > 0 && t < c->t[*seg]) (*seg)--;
    int j = *seg; return c->db[j] + (c->db[j + 1] - c->db[j]) * (t - c->t[j]) / (c->t[j + 1] - c->t[j]);
}
static void rev_free(Rev *r) {
    free(r->pre); for (int i = 0; i < FDN_N; i++) free(r->line[i]); for (int i = 0; i < 4; i++) free(r->ap[i]);
}
static RevCfg hall(double rt) { return (RevCfg){rt, rt * 0.5, 10, 1.4, 200, 6000, 0.14, 1.0, 0.2, 0.25}; }
// gain that makes a send of 0 dB come back as loud as the dry: steady pink noise through the same hall, wet RMS against dry RMS
typedef struct { double rt, g; } WCal;
static WCal *g_wcal; static int g_nwcal, g_cwcal;
static double wash_cal(double rt) {
    for (int i = 0; i < g_nwcal; i++) if (g_wcal[i].rt == rt) return g_wcal[i].g;
    Rev rv; rev_init(&rv, hall(rt)); Rng rg = {0x5eed1234}; double b0 = 0, b1 = 0, b2 = 0, ed = 0, ew = 0;
    long n = (long)((rt * 2.5 + 3) * SR), from = n * 6 / 10;
    for (long i = 0; i < n; i++) {
        double w = rn(&rg); b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
        double x = (b0 + b1 + b2 + w * 0.1848) * 0.2, l, r; rev_tick(&rv, x, x, &l, &r);
        if (i >= from) { ed += x * x; ew += l * l; }
    }
    rev_free(&rv); double g = sqrt(ed / (ew + 1e-30));
    if (g_nwcal == g_cwcal) { g_cwcal = g_cwcal * 2 + 8; g_wcal = xrealloc(g_wcal, g_cwcal * sizeof(WCal)); }
    g_wcal[g_nwcal++] = (WCal){rt, g}; return g;
}
static double echo_level(const Stmt *c, int m) { return c->e_db + (m - 1) * 20 * log10(c->e_fb); }
static int echo_count(const Stmt *c) { int m = 0; while (echo_level(c, m + 1) >= -60 && m < 4000) m++; return m; }

// ---------------------------------------------------------------- layout
static void layout(void) {
    long chain_a = 0, chain_b = 0; int seen = 0;
    for (int i = 0; i < g_nst; i++) {
        Stmt *c = &g_st[i]; Src *s = &g_src[c->src];
        if (!s->n) s->n = decode(s->path, 0, 0, s->line);      // counted once, not kept
        double dur = (double)s->n / SR; if (c->b_end) c->b = dur;
        if (!c->pre_set) c->pre = c->a < 0.008 ? c->a : 0.008;
        if (c->a < 0 || c->b <= c->a || c->b > dur + 1e-9) die(c->line, "%s %.3f-%.3f is outside its source (%.3f s)", s->name, c->a, c->b, dur);
        if (c->pre > c->a + 1e-9) die(c->line, "PRE %.3f is larger than a %.3f", c->pre, c->a);
        if (c->b + c->post > dur + 1e-9) die(c->line, "POST runs %.3f s beyond the source", c->b + c->post - dur);
        double r = c->rate; c->sa = llround(c->a * SR); c->sb = c->b_end ? s->n : llround(c->b * SR);
        long spre = llround(c->pre * SR), spost = llround(c->post * SR);
        if (c->sa - spre < 0) spre = c->sa;
        if (c->sb + spost > s->n) spost = s->n - c->sb;
        c->pre_s = llround((double)spre / r); c->mid_s = llround((double)(c->sb - c->sa) / r); c->post_s = llround((double)spost / r);
        c->Nc = c->pre_s + c->mid_s + c->post_s; c->bs = llround(c->brake * SR);
        if (c->bs > c->Nc) die(c->line, "BRAKE is longer than the clip");
        if (c->kind == 0) c->ia = seen ? chain_b + llround(c->gap * SR) : llround(c->gap * SR);
        else {
            if (!seen) die(c->line, "LAYER before any CLIP");
            c->ia = c->has_off ? chain_a + llround(c->off * SR) : chain_b + llround(c->at * SR);
        }
        c->startS = c->ia - c->pre_s; c->endS = c->startS + c->Nc;
        if (c->kind == 0) { chain_a = c->ia; chain_b = c->ia + c->mid_s; seen = 1; }
        c->sw_len = c->swell ? llround(c->s_len * SR) : 0;
        long tail = 0;
        if (c->wash) tail = (long)((1.3 * c->w_rt + 0.5) * SR);
        if (c->echo) { long end = (long)echo_count(c) * llround(c->e_d * SR) + SR / 20; if (end > tail) tail = end; }
        c->tail = tail;
        Map m = {(double)c->sa, (double)c->pre_s, r, c->Nc, c->bs, 0, 0, 0, 0, 0};
        c->src_in = map_u(&m, 0) / SR; c->src_out = map_u(&m, (double)c->Nc) / SR;
    }
}

// ---------------------------------------------------------------- one clip into the mix
static float *g_oL, *g_oR; static long g_olen, g_lo;
static void add_at(long pos, const float *L, const float *R, long n) {
    for (long i = 0; i < n; i++) { long j = pos - g_lo + i; if (j < 0 || j >= g_olen) continue; g_oL[j] += L[i]; g_oR[j] += R[i]; }
}
// pre: an OUT sweep (the filter blends in over B before ks and stays to the end); else an IN sweep (wet from the start,
// blending out over B after ke)
static void sweep(float *X[2], long Ns, int lp, long ks, long ke, double f0, double f1, int pre, long B) {
    long i0 = pre ? ks - B - SR / 10 : 0, i1 = pre ? Ns : ke + B; if (i0 < 0) i0 = 0; if (i1 > Ns) i1 = Ns;
    Svf st[2] = {{0, 0}, {0, 0}};
    for (long i = i0; i < i1; i++) {
        double fc = i <= ks ? f0 : i >= ke ? f1 : f0 * pow(f1 / f0, (double)(i - ks) / (double)(ke - ks));
        double w = pre ? smoothstep((double)(i - (ks - B)) / (double)B) : 1 - smoothstep((double)(i - ke) / (double)B);
        for (int c = 0; c < 2; c++) {
            double x = X[c][i], bp, hp, y = svf(&st[c], x, fc, 0.7071, &bp, &hp); if (!lp) y = hp;
            if (w > 0) X[c][i] = (float)(x + w * (y - x));
        }
    }
}

// BASS: the band under 150 Hz is taken out with no phase shift (4th order Butterworth, forward and then backward) and
// added back at the curve's gain, so where the curve is 0 dB the clip is untouched, sample for sample.
static void bass_band(float *X[2], long Ns, const Stmt *c, const Map *m) {
    float *lo = xcalloc(Ns, sizeof(float));
    for (int ch = 0; ch < 2; ch++) {
        for (long i = 0; i < Ns; i++) lo[i] = X[ch][i];
        for (int dir = 0; dir < 2; dir++) {
            Bq a = bq_lp(150, 0.5412), b = bq_lp(150, 1.3066);
            for (long i = 0; i < Ns; i++) { long k = dir ? Ns - 1 - i : i; lo[k] = (float)bq(&b, bq(&a, lo[k])); }
        }
        int seg = 0;
        for (long i = 0; i < Ns; i++) {
            double db = curve_at(&c->bass, map_u(m, (double)(i - LEAD)) / SR, &seg);
            if (db != 0) X[ch][i] += (float)((db2a(db) - 1) * lo[i]);
        }
    }
    free(lo);
}

static void render_clip(Stmt *c) {
    Src *s = &g_src[c->src]; src_load(s);
    double target = c->lufs_set ? (c->lufs_off ? NAN : c->lufs_v) : c->level, meas = -200, lev_db = 0;
    if (!isnan(target)) {
        const float *ML = s->L, *MR = s->R; long mn = s->n;
        if (s->from) { src_load_from(s); ML = s->FL; MR = s->FR; mn = s->nfrom; }
        long a = c->sa, b = c->sb;
        if (b - a < SR / 2) { long mid = (a + b) / 2; a = mid - SR / 4; b = a + SR / 2; }
        if (b > mn) b = mn;
        if (a < 0) a = 0;
        if (b - a < SR / 2) { a = b - SR / 2; if (a < 0) a = 0; }
        if (b > a) meas = lufs(ML + a, MR + a, b - a, 0, 0, 0);
        if (meas > -100) lev_db = target - meas; else fprintf(stderr, "splice: line %d: %s is silent there, no level applied\n", c->line, s->name);
    }
    c->meas = meas; c->lev_db = lev_db + c->gain;
    // the stream
    long Ns = LEAD + c->Nc; Map m = {(double)c->sa, (double)c->pre_s, c->rate, c->Nc, c->bs, 0, 0, 0, 0, 0};
    float *X[2] = {xcalloc(Ns, sizeof(float)), xcalloc(Ns, sizeof(float))};
    bool au = !c->vari && (c->rate != 1 || c->semi != 0);
    if (!au) {
        if (c->rate == 1 && c->bs == 0) {
            for (long i = 0; i < Ns; i++) { long j = c->sa - c->pre_s - LEAD + i; if (j >= 0 && j < s->n) { X[0][i] = s->L[j]; X[1][i] = s->R[j]; } }
        } else { sinc_resample(s->L, s->n, Ns, map_pos, &m, X[0]); sinc_resample(s->R, s->n, Ns, map_pos, &m, X[1]); }
    } else {
        AuCal cal = au_calibrate(c->rate, c->semi); m.au = 1; m.c1 = cal.c1; m.p1 = cal.p1; m.reff = cal.reff;
        double u0 = map_u(&m, -(double)LEAD), u1 = map_u(&m, (double)c->Nc);
        long sx = (long)floor(u0) - APAD - 64, ex = (long)ceil(u1) + APAD + 64, nin = ex - sx; m.sx = (double)sx;
        float *iL = xcalloc(nin, sizeof(float)), *iR = xcalloc(nin, sizeof(float));
        for (long i = 0; i < nin; i++) { long j = sx + i; if (j >= 0 && j < s->n) { iL[i] = s->L[j]; iR[i] = s->R[j]; } }
        long nout = (long)(map_pos(&m, Ns) + 256); if (nout < 1024) nout = 1024;
        float *oL = xcalloc(nout, sizeof(float)), *oR = xcalloc(nout, sizeof(float));
        au_run(iL, iR, nin, c->rate, cal.cents, nout, oL, oR); free(iL); free(iR);
        sinc_resample(oL, nout, Ns, map_pos, &m, X[0]); sinc_resample(oR, nout, Ns, map_pos, &m, X[1]); free(oL); free(oR);
    }
    // DC removal, level and rides, bands, EQ
    Bq dc[2] = {bq_hp(18, 0.7071), bq_hp(18, 0.7071)};
    Bq eq[2][3]; for (int ch = 0; ch < 2; ch++) { eq[ch][0] = bq_lshelf(120, c->eq[0]); eq[ch][1] = bq_peak(1000, 0.7, c->eq[1]); eq[ch][2] = bq_hshelf(5000, c->eq[2]); }
    int rs = 0;
    for (long i = 0; i < Ns; i++) {
        double t = map_u(&m, (double)(i - LEAD)) / SR;
        double g = db2a(c->lev_db + (c->ride.n ? curve_at(&c->ride, t, &rs) : 0));
        for (int ch = 0; ch < 2; ch++) X[ch][i] = (float)(bq(&dc[ch], X[ch][i]) * g);
    }
    if (c->bass.n) bass_band(X, Ns, c, &m);
    if (c->has_eq) for (int ch = 0; ch < 2; ch++) for (long i = 0; i < Ns; i++) {
        double x = X[ch][i];
        for (int q = 0; q < 3; q++) x = bq(&eq[ch][q], x);
        X[ch][i] = (float)x;
    }
    // filter sweeps
    long B = SR / 20;
    for (int j = 0; j < 4; j++) {
        if (!c->sw[j].set) continue;
        long len = llround(c->sw[j].s * SR); if (len > c->Nc) len = c->Nc; if (len < 2) continue;
        int out = j % 2 == 0, lp = j < 2; double hz = c->sw[j].hz, edge = lp ? 18000 : 20;
        if (out) sweep(X, Ns, lp, Ns - len, Ns, edge, hz, 1, B);
        else sweep(X, Ns, lp, LEAD, LEAD + len, hz, edge, 0, B);
    }
    // sends, taken before the fades
    float *wL = 0, *wR = 0; long wn = 0, wat = 0;           // wash wet, from song sample wat (relative to the audible start)
    if (c->wash) {
        long nl = llround(c->w_len * SR); if (nl > c->Nc) nl = c->Nc;
        long open = nl < (long)(0.3 * SR) ? nl : (long)(0.3 * SR), tl = (long)((1.3 * c->w_rt + 0.5) * SR);
        wn = nl + tl; wat = c->Nc - nl; wL = xcalloc(wn, sizeof(float)); wR = xcalloc(wn, sizeof(float));
        double sg = wash_cal(c->w_rt) * db2a(c->w_db); Rev rv; rev_init(&rv, hall(c->w_rt));
        for (long i = 0; i < wn; i++) {
            long k = wat + i; double l = 0, r = 0;
            if (i < nl) {
                double e = smoothstep((double)i / (double)(open > 0 ? open : 1)); long ce = k - (c->Nc - SR / 100);
                if (ce > 0) e *= 1 - smoothstep((double)ce / (SR / 100.0));
                l = X[0][LEAD + k] * e * sg; r = X[1][LEAD + k] * e * sg;
            }
            double ol, orr; rev_tick(&rv, l, r, &ol, &orr); wL[i] = (float)ol; wR[i] = (float)orr;
        }
        rev_free(&rv);
    }
    float *eL = 0, *eR = 0; long en = 0, eat = 0;
    if (c->echo) {
        long elen = llround(c->e_len * SR); if (elen > c->Nc) elen = c->Nc; long ed = llround(c->e_d * SR), M = echo_count(c);
        long segn = elen + SR / 20, ef = SR / 200; en = M * ed + segn; eat = c->Nc - elen; eL = xcalloc(en, sizeof(float)); eR = xcalloc(en, sizeof(float));
        float *sg[2] = {xcalloc(segn, sizeof(float)), xcalloc(segn, sizeof(float))};
        for (long i = 0; i < elen; i++) {
            double e = 1; if (i < ef) e = (double)i / (double)ef; if (i >= elen - ef) e *= (double)(elen - 1 - i) / (double)ef;
            sg[0][i] = (float)(X[0][LEAD + eat + i] * e); sg[1][i] = (float)(X[1][LEAD + eat + i] * e);
        }
        for (int mm = 1; mm <= M; mm++) {
            double a = db2a(echo_level(c, mm));
            for (int ch = 0; ch < 2; ch++) { Bq h = bq_hp(200, 0.7071), l = bq_lp(6000, 0.7071); for (long i = 0; i < segn; i++) sg[ch][i] = (float)bq(&l, bq(&h, sg[ch][i])); }
            for (long i = 0; i < segn; i++) { long j = (long)mm * ed + i; eL[j] += (float)(sg[0][i] * a); eR[j] += (float)(sg[1][i] * a); }
        }
        free(sg[0]); free(sg[1]);
    }
    float *sL = 0, *sR = 0; long sn = 0;
    if (c->swell) {
        long seg = SR / 2, T = seg + c->sw_len; double rt = c->s_len < 0.3 ? 0.3 : c->s_len;
        float *rin = xcalloc(T, sizeof(float)), *rinR = xcalloc(T, sizeof(float)); sL = xcalloc(c->sw_len, sizeof(float)); sR = xcalloc(c->sw_len, sizeof(float));
        for (long i = 0; i < seg; i++) { long k = LEAD + c->pre_s + (seg - 1 - i); if (k < Ns) { rin[i] = X[0][k]; rinR[i] = X[1][k]; } }
        Rev rv; rev_init(&rv, hall(rt)); double sg = wash_cal(rt) * db2a(c->s_db); float *yL = xcalloc(T, sizeof(float)), *yR = xcalloc(T, sizeof(float));
        for (long i = 0; i < T; i++) { double l, r; rev_tick(&rv, rin[i] * sg, rinR[i] * sg, &l, &r); yL[i] = (float)l; yR[i] = (float)r; }
        rev_free(&rv);
        for (long t = 0; t < c->sw_len; t++) { double e = sin(0.5 * M_PI * (double)t / (double)c->sw_len); sL[t] = (float)(yL[T - 1 - t] * e); sR[t] = (float)(yR[T - 1 - t] * e); }
        sn = c->sw_len; free(rin); free(rinR); free(yL); free(yR);
    }
    // fades, then the sum
    long nfi = llround(c->fin * SR), nfo = llround(c->fout * SR); if (nfi > c->Nc) nfi = c->Nc; if (nfo > c->Nc) nfo = c->Nc;
    float *dL = X[0] + LEAD, *dR = X[1] + LEAD;
    for (long k = 0; k < c->Nc; k++) {
        double e = 1; if (k < nfi) e = sin(0.5 * M_PI * (double)k / (double)nfi);
        if (k >= c->Nc - nfo) e *= cos(0.5 * M_PI * (double)(k - (c->Nc - nfo) + 1) / (double)nfo);
        if (e != 1) { dL[k] = (float)(dL[k] * e); dR[k] = (float)(dR[k] * e); }
    }
    add_at(c->startS, dL, dR, c->Nc);
    if (wL) add_at(c->startS + wat, wL, wR, wn);
    if (eL) add_at(c->startS + eat, eL, eR, en);
    if (sL) add_at(c->ia - c->sw_len, sL, sR, sn);
    free(X[0]); free(X[1]); free(wL); free(wR); free(eL); free(eR); free(sL); free(sR);
}

// ---------------------------------------------------------------- FX one-shots and the BUS filter
#define LIMIT_REPORT_DB 1.0                              // the render log lists the ten worst places where the master limiter takes more than this (the suites allow 1 dB)
#define FX_SEED 0x5f3759df2b1e4d6bULL                    // the one seed of every FX; FX number k draws from mix64(FX_SEED + k)
static const long FX_FADE = SR / 200;                     // 5 ms guards at both ends of every effect
static const double FX_ATTACK = 0.003, FX_COMMON = 0.84, FX_OWN = 0.54;   // cymbal attack; share of the noise both channels hold
enum { FX_PARTIALS = 7 };
static const double FX_DECAY_DB = 60, FX_SWEEP_Q = 3, BOOM_HI = 110, BOOM_LO = 38, BOOM_SWEEP = 0.25, BOOM_ATTACK = 0.002, BOOM_CLICK = 0.01;
static const double RISE_F0 = 300, RISE_F1 = 9000, RISE_DB = 24, DOWN_F0 = 9000, DOWN_F1 = 200, DOWN_DB = 30, ROLL_DB0 = -18, ROLL_HIT = 0.5;
static const long BUS_STEP = 32, BUS_EDGE = SR / 50;      // coefficient update interval; the dry-to-wet crossfade at each edge

// two noises that share most of their energy: slightly decorrelated left and right
static void fx_noise(Rng *g, double *l, double *r) {
    double c = rn(g), x = rn(g), y = rn(g); *l = FX_COMMON * c + FX_OWN * x; *r = FX_COMMON * c + FX_OWN * y;
}
static void fx_crash(Rng *g, long n, float *L, float *R) {
    double pf[FX_PARTIALS], ph[2][FX_PARTIALS], len = (double)n / SR;
    for (int k = 0; k < FX_PARTIALS; k++) { pf[k] = 3000 * pow(3, ((double)k + rr(g, 0.1, 0.9)) / FX_PARTIALS); ph[0][k] = rr(g, 0, TAU); ph[1][k] = rr(g, 0, TAU); }
    Bq hp[2][2] = {{bq_hp(3000, 0.7071), bq_hp(3000, 0.7071)}, {bq_hp(3000, 0.7071), bq_hp(3000, 0.7071)}};
    for (long i = 0; i < n; i++) {
        double t = (double)i / SR, nz[2], env = (t < FX_ATTACK ? t / FX_ATTACK : 1) * pow(10, -FX_DECAY_DB / 20 * t / len);
        fx_noise(g, &nz[0], &nz[1]);
        for (int c = 0; c < 2; c++) {
            double p = 0; for (int k = 0; k < FX_PARTIALS; k++) p += sin(TAU * pf[k] * t + ph[c][k]);
            double x = bq(&hp[c][1], bq(&hp[c][0], nz[c])) + 1.5 * p / FX_PARTIALS * (0.6 + 0.4 * nz[c]);
            (c ? R : L)[i] = (float)(x * env);
        }
    }
}
static void fx_reverse(long n, float *L, float *R) {
    for (long i = 0; i < n / 2; i++) { float a = L[i], b = R[i]; L[i] = L[n - 1 - i]; R[i] = R[n - 1 - i]; L[n - 1 - i] = a; R[n - 1 - i] = b; }
}
// band-passed noise: the centre sweeps exponentially f0 to f1, the gain goes linearly in dB from g0 to g1
static void fx_sweep(Rng *g, long n, float *L, float *R, double f0, double f1, double g0, double g1) {
    Svf st[2] = {{0, 0}, {0, 0}};
    for (long i = 0; i < n; i++) {
        double u = (double)i / (double)n, nz[2], bp, gain = db2a(g0 + (g1 - g0) * u), fc = f0 * pow(f1 / f0, u);
        fx_noise(g, &nz[0], &nz[1]);
        for (int c = 0; c < 2; c++) { svf(&st[c], nz[c], fc, FX_SWEEP_Q, &bp, 0); (c ? R : L)[i] = (float)(bp * gain); }
    }
}
static void fx_boom(Rng *g, long n, float *L, float *R) {
    Bq lp = bq_lp(800, 0.7071); double ph = 0, len = (double)n / SR; long nc = llround(BOOM_CLICK * SR);
    for (long i = 0; i < n; i++) {
        double t = (double)i / SR, f = t < BOOM_SWEEP ? BOOM_HI * pow(BOOM_LO / BOOM_HI, t / BOOM_SWEEP) : BOOM_LO;
        double env = (t < BOOM_ATTACK ? t / BOOM_ATTACK : 1) * pow(10, -FX_DECAY_DB / 20 * t / len);
        double click = i < nc ? 1.5 * bq(&lp, rn(g)) * (1 - (double)i / (double)nc) : 0;
        L[i] = R[i] = (float)(sin(ph) * env + click); ph += TAU * f / SR;
    }
}
// hits on a grid from the start: every half beat for the first half, every quarter beat for the next quarter, every eighth for the last
static void fx_roll(Rng *g, long n, double beat, float *L, float *R) {
    long hl = llround(ROLL_HIT * SR);
    for (long p = 0; p < n;) {
        double gain = db2a(ROLL_DB0 - ROLL_DB0 * (double)p / (double)n);
        double div = p < n / 2 ? 2 : p < n * 3 / 4 ? 4 : 8;
        Bq hp[2] = {bq_hp(1500, 0.7071), bq_hp(1500, 0.7071)};
        for (long i = 0; i < hl && p + i < n; i++) {
            double t = (double)i / SR, nz[2], body = sin(TAU * 190 * t) * exp(-t / 0.06) * 0.5, att = i < SR / 2000 ? (double)i / (SR / 2000) : 1;
            fx_noise(g, &nz[0], &nz[1]);
            for (int c = 0; c < 2; c++) (c ? R : L)[p + i] += (float)((bq(&hp[c], nz[c]) * exp(-t / 0.09) + body) * att * gain);
        }
        p += llround(beat / div * SR);
    }
}
// loudest 400 ms window (hop 100 ms, and the one that ends at the end), K-weighted, in LUFS
static double fx_loud(const float *L, const float *R, long n) {
    long blk = SR * 4 / 10, hop = SR / 10, m = n > blk ? n : blk;
    double *pl = xcalloc(m, sizeof(double)), *pr = xcalloc(m, sizeof(double)), *cs = xcalloc(m + 1, sizeof(double)), best = 0;
    kweight(L, n, 1, pl); kweight(R, n, 1, pr);
    for (long i = 0; i < m; i++) cs[i + 1] = cs[i] + pl[i] + pr[i];
    for (long s = 0; s + blk <= m; s += hop) best = fmax(best, cs[s + blk] - cs[s]);
    best = fmax(best, cs[m] - cs[m - blk]);
    free(pl); free(pr); free(cs); return -0.691 + 10 * log10(best / blk + 1e-30);
}
static void fx_render(const Fx *f, int ord, long lo) {
    long n = f->n; float *L = xcalloc(n, sizeof(float)), *R = xcalloc(n, sizeof(float)); Rng rg = {mix64(FX_SEED + (uint64_t)ord)};
    switch (f->kind) {
    case FX_CRASH: fx_crash(&rg, n, L, R); break;
    case FX_REVCYM: fx_crash(&rg, n, L, R); fx_reverse(n, L, R); break;
    case FX_RISER: fx_sweep(&rg, n, L, R, RISE_F0, RISE_F1, -RISE_DB, 0); break;
    case FX_DOWN: fx_sweep(&rg, n, L, R, DOWN_F0, DOWN_F1, 0, -DOWN_DB); break;
    case FX_BOOM: fx_boom(&rg, n, L, R); break;
    default: fx_roll(&rg, n, f->beat, L, R); break;
    }
    long nf = FX_FADE < n / 2 ? FX_FADE : n / 2;
    for (long k = 0; k < nf; k++) {
        double e = sin(0.5 * M_PI * (double)k / (double)nf);
        L[k] *= (float)e; R[k] *= (float)e; L[n - 1 - k] *= (float)e; R[n - 1 - k] *= (float)e;
    }
    double loud = fx_loud(L, R, n); if (loud < -100) die(f->line, "FX %s is silent", fxname[f->kind]);
    double g = db2a(f->db - loud);
    for (long i = 0; i < n; i++) { L[i] *= (float)g; R[i] *= (float)g; }
    add_at(f->a, L, R, n); free(L); free(R);
    fprintf(stderr, "FX %-6s %9.3f - %9.3f  %6.1f LUFS\n", fxname[f->kind], (double)(f->a - lo) / SR, (double)(f->a + n - lo) / SR, f->db);
}
static void bus_coef(Bq *d, const Bq *s) { d->b0 = s->b0; d->b1 = s->b1; d->b2 = s->b2; d->a1 = s->a1; d->a2 = s->a2; }
// the filter runs from LEAD before the first point to settle; the mix is written only where the edge blend is above zero
static void bus_apply(const Bus *b, long lo) {
    long i0 = b->r0 - LEAD, i1 = b->r1; if (i0 < g_lo) i0 = g_lo; if (i1 > g_lo + g_olen) i1 = g_lo + g_olen;
    Bq st[2][2] = {{{0}, {0}}, {{0}, {0}}}; int seg = 0;
    for (long p = i0; p < i1; p++) {
        if ((p - i0) % BUS_STEP == 0) {
            double fc = exp(curve_at(&b->c, (double)(p - b->a) / SR, &seg)); fc = clampd(fc, BUS_HZ_MIN, 0.45 * SR);
            Bq c = b->lp ? bq_lp(fc, 0.7071) : bq_hp(fc, 0.7071);
            for (int ch = 0; ch < 2; ch++) for (int s = 0; s < 2; s++) bus_coef(&st[ch][s], &c);
        }
        double w = smoothstep((double)(p - b->r0) / BUS_EDGE) * smoothstep((double)(b->r1 - p) / BUS_EDGE);
        for (int ch = 0; ch < 2; ch++) {
            float *o = (ch ? g_oR : g_oL) + (p - g_lo); double x = *o, y = bq(&st[ch][1], bq(&st[ch][0], x));
            if (w > 0) *o = (float)(x + w * (y - x));
        }
    }
    fprintf(stderr, "BUS %s  %9.3f - %9.3f  %d points\n", b->lp ? "LP" : "HP", (double)(b->r0 - lo) / SR, (double)(b->r1 - lo) / SR, b->c.n);
}

// ---------------------------------------------------------------- master and outputs
typedef struct { long at; double db; } Red;
static int cmp_start(const void *a, const void *b) {
    const Stmt *x = *(Stmt *const *)a, *y = *(Stmt *const *)b;
    if (x->startS != y->startS) return x->startS < y->startS ? -1 : 1;
    return x->idx - y->idx;
}
int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) { fprintf(stderr, "usage: splice cue.txt out.wav [timeline.tsv [seams.tsv]]\n"); return 2; }
    double m_lufs = -16, m_tp = -1.5; parse(argv[1], &m_lufs, &m_tp);
    int any_clip = 0; for (int i = 0; i < g_nst; i++) if (g_st[i].kind == 0) any_clip = 1;
    if (!any_clip) die(0, "no CLIP");
    layout();
    for (int i = 0; i < g_nfx; i++) {
        Fx *f = &g_fx[i]; const Stmt *k = &g_st[f->clip];
        f->a = (f->has_off ? k->ia : k->ia + k->mid_s) + llround(f->x * SR); f->n = llround(f->len * SR);
    }
    for (int i = 0; i < g_nbus; i++) {
        Bus *b = &g_bus[i]; const Stmt *k = &g_st[b->clip];
        b->a = (b->has_off ? k->ia : k->ia + k->mid_s) + llround(b->x * SR);
        b->r0 = b->a + llround(b->c.t[0] * SR); b->r1 = b->a + llround(b->c.t[b->c.n - 1] * SR);
        for (int j = 0; j < i; j++) if (g_bus[j].lp == b->lp && b->r0 < g_bus[j].r1 && g_bus[j].r0 < b->r1)
            die(b->line, "BUS %s overlaps the one on line %d", b->lp ? "LP" : "HP", g_bus[j].line);
    }
    long lo = 0, hi = 0;
    for (int i = 0; i < g_nst; i++) {
        Stmt *c = &g_st[i]; c->idx = i; long a = c->startS, b = c->endS + c->tail;
        if (c->swell && c->ia - c->sw_len < a) a = c->ia - c->sw_len;
        if (i == 0 || a < lo) lo = a;
        if (i == 0 || b > hi) hi = b;
    }
    for (int i = 0; i < g_nfx; i++) { if (g_fx[i].a < lo) lo = g_fx[i].a; if (g_fx[i].a + g_fx[i].n > hi) hi = g_fx[i].a + g_fx[i].n; }
    g_lo = lo; g_olen = hi - lo + 1 + (long)(0.4 * SR);   // the room for the 0.4 s that the master keeps after the last sound
     g_oL = xcalloc(g_olen, sizeof(float)); g_oR = xcalloc(g_olen, sizeof(float));
    for (int i = 0; i < g_nst; i++) {
        Stmt *c = &g_st[i]; render_clip(c);
        fprintf(stderr, "%4d %s %9.3f - %9.3f  src %s %.3f-%.3f  %6.1f LUFS  gain %+6.2f dB  rate %.4f\n", i + 1, c->kind ? "LAYER" : "CLIP ",
                (double)(c->startS - lo) / SR, (double)(c->endS - lo) / SR, g_src[c->src].name, c->src_in, c->src_out, c->meas, c->lev_db, c->rate);
        if (g_src[c->src].last_use == i) src_free(&g_src[c->src]);
    }
    for (int i = 0; i < g_nbus; i++) bus_apply(&g_bus[i], lo);
    for (int i = 0; i < g_nfx; i++) fx_render(&g_fx[i], i, lo);
    long n = g_olen; double thr = db2a(-75);
    while (n > 1 && fabsf(g_oL[n - 1]) < thr && fabsf(g_oR[n - 1]) < thr) n--;
    n += (long)(0.4 * SR); if (n > g_olen) n = g_olen;
    // master: gain, then the limiter, iterated until the loudness is right under the ceiling
    float *WL = xcalloc(n, sizeof(float)), *WR = xcalloc(n, sizeof(float));
    double I0 = lufs(g_oL, g_oR, n, 0, 0, 0), gdb = m_lufs - I0, ceil_eff = m_tp, I = 0, tp = 0; int limited = 0, ok = 0;
    if (I0 < -100) { fprintf(stderr, "splice: WARNING the mix is silent, master gain left at 0 dB\n"); gdb = 0; }
    for (int it = 0; it < 24 && !ok; it++) {
        double g = db2a(gdb);
        for (long i = 0; i < n; i++) { WL[i] = (float)(g_oL[i] * g); WR[i] = (float)(g_oR[i] * g); }
        tp = a2db(true_peak(WL, WR, n)); limited = 0;
        if (tp > ceil_eff) { limit(WL, WR, n, ceil_eff, 0.12); limited = 1; tp = a2db(true_peak(WL, WR, n)); }
        I = lufs(WL, WR, n, 0, 0, 0);
        if (tp > m_tp) { ceil_eff -= (tp - m_tp) + 0.02; continue; }
        if (I < -100 || fabs(I - m_lufs) <= 0.03) ok = 1; else gdb += m_lufs - I;
    }
    if (!ok) fprintf(stderr, "splice: WARNING master did not settle: %.3f LUFS, %.3f dBTP\n", I, tp);
    if (limited) {
        double g = db2a(gdb), worst = 0; long cap = 0, nr = 0; Red *rd = 0;
        for (long i = 0; i < n; i++) {
            double x = fmax(fabs(g_oL[i]), fabs(g_oR[i])) * g; if (x < 1e-3) continue;
            double y = fmax(fabs(WL[i]), fabs(WR[i])), red = -a2db(y / x); if (red > worst) worst = red;
            if (red > LIMIT_REPORT_DB) { if (nr == cap) { cap = cap * 2 + 64; rd = xrealloc(rd, cap * sizeof(Red)); } rd[nr++] = (Red){i, red}; }
        }
        fprintf(stderr, "limiter: at most %.2f dB of reduction\n", worst);
        for (int k = 0; k < 10; k++) {
            long bi = -1; for (long i = 0; i < nr; i++) if (rd[i].db > 0 && (bi < 0 || rd[i].db > rd[bi].db)) bi = i;
            if (bi < 0) break;
            fprintf(stderr, "  over %g dB at %.3f s: %.2f dB\n", LIMIT_REPORT_DB, (double)rd[bi].at / SR, rd[bi].db);
            long at = rd[bi].at; for (long i = 0; i < nr; i++) if (labs(rd[i].at - at) < SR) rd[i].db = 0;
        }
        free(rd);
    } else fprintf(stderr, "limiter: not needed\n");
    wav_write24(argv[2], WL, WR, n);
    fprintf(stderr, "wrote %s  %.3f s  %.2f LUFS  %.2f dBTP%s\n", argv[2], (double)n / SR, I, tp, limited ? "  (limited)" : "");
    Stmt **ord = xcalloc(g_nst, sizeof *ord); for (int i = 0; i < g_nst; i++) ord[i] = &g_st[i];
    qsort(ord, g_nst, sizeof *ord, cmp_start);
    for (int i = 0; i < g_nst; i++) ord[i]->idx = i + 1;       // from here idx is the timeline row
    if (argc > 3) {
        FILE *tf = fopen(argv[3], "w"); if (!tf) { perror(argv[3]); return 1; }
        fprintf(tf, "idx\tsong_start\tsong_end\tsrc_path\tsrc_in\tsrc_out\trate\tsemitones\tgain_db\trole\tjoin_in\n");
        for (int i = 0; i < g_nst; i++) {
            Stmt *c = ord[i]; Src *s = &g_src[c->src];
            // the trailing silence was trimmed, so a row that ran into it ends where the file ends, and so does its source span
            long end = c->endS - lo; double sout = c->src_out;
            if (end > n) { sout = c->src_in + (c->src_out - c->src_in) * (double)(n - (c->startS - lo)) / (double)(c->endS - c->startS); end = n; }
            fprintf(tf, "%d\t%.6f\t%.6f\t%s\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%s\t%s\n", c->idx, (double)(c->startS - lo) / SR, (double)end / SR,
                    s->from ? s->from : s->path, c->src_in, sout, c->rate, c->vari ? 12 * log2(c->rate) : c->semi, c->lev_db + gdb, c->role ? c->role : "", c->join ? c->join : "");
        }
        fclose(tf);
    }
    if (argc > 4) {
        FILE *sf = fopen(argv[4], "w"); if (!sf) { perror(argv[4]); return 1; }
        fprintf(sf, "idx\ttime\tprev_end\tstart\trole\tjoin_in\n"); Stmt *prev = 0;
        for (int i = 0; i < g_nst; i++) {
            Stmt *c = &g_st[i]; if (c->kind) continue;
            if (prev) fprintf(sf, "%d\t%.6f\t%.6f\t%.6f\t%s\t%s\n", c->idx, (double)(c->ia - lo) / SR, (double)(prev->endS - lo) / SR, (double)(c->startS - lo) / SR, c->role ? c->role : "", c->join ? c->join : "");
            prev = c;
        }
        fclose(sf);
    }
    return ok ? 0 : 1;
}
