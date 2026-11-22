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
//   CLIP  name a b [key value]...             a, b: cut points in source seconds (b may be `end`)
//   LAYER name a b [key value]...
// Placement: a CLIP's `a` lands at the previous CLIP's `b` + GAP (exact to a sample, whatever PRE, POST, fades or effects).
// A LAYER never moves that chain: its `a` lands at the `b` of the latest preceding CLIP + AT, or at that CLIP's `a` + OFF.
// With RATE r a source span s lasts s/r song seconds. A clip is audible from a-PRE to b+POST in the source.
// Keys (CLIP and LAYER): PRE POST FIN FOUT LUFS GAIN RIDE BASS EQ LPOUT LPIN HPOUT HPIN WASH ECHO SWELL RATE SEMI VARI
//   BRAKE ROLE JOIN; CLIP only: GAP; LAYER only: AT OFF (not both).
// Order of processing: rate/pitch, DC removal, level and GAIN and RIDE, BASS, EQ, filters, effect sends (pre-fade), fades, sum.
// The only random source is the Rng of dsp.h (the pink noise that calibrates the reverb sends, with a fixed seed).
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

#define LEAD (SR / 2)     // settling run before the audible start (DC removal, bands, sweeps)
#define APAD (SR * 3 / 2) // real padding either side of an AudioToolbox render

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
static const char *swname[4] = {"LPOUT", "LPIN", "HPOUT", "HPIN"};

static void parse(const char *path, double *m_lufs, double *m_tp) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(1); }
    char *line = 0; size_t cap = 0; int ln = 0; double level = -16; int level_on = 1;
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
        } else die(ln, "unknown statement %s", tok[0]);
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
    Bq lp1[2] = {bq_lp(150, 0.7071), bq_lp(150, 0.7071)}, lp2[2] = {bq_lp(150, 0.7071), bq_lp(150, 0.7071)};
    Bq hp1[2] = {bq_hp(150, 0.7071), bq_hp(150, 0.7071)}, hp2[2] = {bq_hp(150, 0.7071), bq_hp(150, 0.7071)};
    Bq eq[2][3]; for (int ch = 0; ch < 2; ch++) { eq[ch][0] = bq_lshelf(120, c->eq[0]); eq[ch][1] = bq_peak(1000, 0.7, c->eq[1]); eq[ch][2] = bq_hshelf(5000, c->eq[2]); }
    int rs = 0, bsg = 0;
    for (long i = 0; i < Ns; i++) {
        double t = map_u(&m, (double)(i - LEAD)) / SR;
        double g = db2a(c->lev_db + (c->ride.n ? curve_at(&c->ride, t, &rs) : 0));
        double gb = c->bass.n ? db2a(curve_at(&c->bass, t, &bsg)) : 1;
        for (int ch = 0; ch < 2; ch++) {
            double x = bq(&dc[ch], X[ch][i]) * g;
            if (c->bass.n) { double lo = bq(&lp2[ch], bq(&lp1[ch], x)), hi = bq(&hp2[ch], bq(&hp1[ch], x)); x = hi + lo * gb; }
            if (c->has_eq) for (int q = 0; q < 3; q++) x = bq(&eq[ch][q], x);
            X[ch][i] = (float)x;
        }
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
    long lo = 0, hi = 0;
    for (int i = 0; i < g_nst; i++) {
        Stmt *c = &g_st[i]; c->idx = i; long a = c->startS, b = c->endS + c->tail;
        if (c->swell && c->ia - c->sw_len < a) a = c->ia - c->sw_len;
        if (i == 0 || a < lo) lo = a;
        if (i == 0 || b > hi) hi = b;
    }
    g_lo = lo; g_olen = hi - lo + 1 + (long)(0.4 * SR);   // the room for the 0.4 s that the master keeps after the last sound
     g_oL = xcalloc(g_olen, sizeof(float)); g_oR = xcalloc(g_olen, sizeof(float));
    for (int i = 0; i < g_nst; i++) {
        Stmt *c = &g_st[i]; render_clip(c);
        fprintf(stderr, "%4d %s %9.3f - %9.3f  src %s %.3f-%.3f  %6.1f LUFS  gain %+6.2f dB  rate %.4f\n", i + 1, c->kind ? "LAYER" : "CLIP ",
                (double)(c->startS - lo) / SR, (double)(c->endS - lo) / SR, g_src[c->src].name, c->src_in, c->src_out, c->meas, c->lev_db, c->rate);
        if (g_src[c->src].last_use == i) src_free(&g_src[c->src]);
    }
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
            if (red > 2) { if (nr == cap) { cap = cap * 2 + 64; rd = xrealloc(rd, cap * sizeof(Red)); } rd[nr++] = (Red){i, red}; }
        }
        fprintf(stderr, "limiter: at most %.2f dB of reduction\n", worst);
        for (int k = 0; k < 10; k++) {
            long bi = -1; for (long i = 0; i < nr; i++) if (rd[i].db > 0 && (bi < 0 || rd[i].db > rd[bi].db)) bi = i;
            if (bi < 0) break;
            fprintf(stderr, "  over 2 dB at %.3f s: %.2f dB\n", (double)rd[bi].at / SR, rd[bi].db);
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
            fprintf(tf, "%d\t%.6f\t%.6f\t%s\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%s\t%s\n", c->idx, (double)(c->startS - lo) / SR, (double)(c->endS - lo) / SR,
                    s->from ? s->from : s->path, c->src_in, c->src_out, c->rate, c->vari ? 12 * log2(c->rate) : c->semi, c->lev_db + gdb, c->role ? c->role : "", c->join ? c->join : "");
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
