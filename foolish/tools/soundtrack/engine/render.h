// render.h: the driver. A city score defines city_setup(), city_full() and city_loop(),
// then includes this file. Usage: <city> full|loop outdir
//   full: renders the whole form to outdir/full.wav
//   loop: renders the loop body three times, keeps the middle pass (its start already carries
//         the first pass's tails, and its end runs into an identical third pass), and writes
//         outdir/loop.wav, sample-accurate and seamless by construction; the seam is then measured.
#pragma once
#include "sampler.h"
#include "mix.h"
#include <sys/stat.h>

typedef struct { int type; RevCfg rev; double dl_s, dr_s, fb, lp, hp; double gain_db; int duck_src; double duck_db, duck_rel; int gate_src; double gate_hold, gate_rel; } Bus; // type 1 reverb, 2 ping-pong delay
static Bus g_bus[4];
// group buses: tracks with group g (1..4) sum here; the group gets its own chain, then joins the master
typedef struct { EqBand eq[8]; int neq; double drive, cthr, cratio, catt, crel, gain_db, parallel; int duck_src; double duck_db, duck_rel; } Group;
static Group g_grp[5];
typedef struct { EqBand eq[8]; int neq; double cthr, cratio, catt, crel; double lufs, ceil_db, lim_rel; double width_lo_hz; double tape, room_db, top_hz, side; } Master;
// tape: saturation amount on the master (ADAA tanh, 0 off); room_db: a pink room-tone floor (dBFS, 0 off), periodic with the loop;
// top_hz: a gentle 12 dB/oct roll-off of the very top (0 off), as any real chain has
static Master g_master = {.lufs = -16, .ceil_db = -1.0, .lim_rel = 0.08};
static double g_tail_s = 6;
static void (*g_on_done)(const char *outdir);   // a city hook run after rendering (e.g. writes section times)

// low-pass automation per track (beats, Hz; exponential between points). A track with no points is untouched.
typedef struct { int tr; double beat, hz; int k; double t; int step; } AutoPt;   // step: hold the previous value up to this point, then jump
static AutoPt g_auto[8192]; static int g_nauto;
static void autolp(int tr, double beat, double hz) { g_auto[g_nauto++] = (AutoPt){tr, beat, hz, g_k, 0, 0}; }
static void autolp_step(int tr, double beat, double hz) { g_auto[g_nauto++] = (AutoPt){tr, beat, hz, g_k, 0, 1}; }
static void apply_autolp(int tr, float *L, float *R, long n) {
    AutoPt pts[4096]; int np = 0;
    for (int i = 0; i < g_nauto; i++) if (g_auto[i].tr == tr) pts[np++] = g_auto[i];
    if (!np) return;
    for (int a = 1; a < np; a++) for (int b = a; b > 0 && (pts[b].t < pts[b - 1].t - 1e-9 || (fabs(pts[b].t - pts[b - 1].t) <= 1e-9 && pts[b].step < pts[b - 1].step)); b--) { AutoPt x = pts[b]; pts[b] = pts[b - 1]; pts[b - 1] = x; }
    Svf sl = {0}, sr = {0}, sl2 = {0}, sr2 = {0}; int j = 0;
    for (long i = 0; i < n; i++) {
        double t = (double)i / SR; while (j + 1 < np && pts[j + 1].t <= t) j++;
        double hz = pts[j].hz; if (t < pts[0].t) hz = pts[0].step ? 19000 : pts[0].hz; else if (j + 1 < np && !pts[j + 1].step) { double x = (t - pts[j].t) / (pts[j + 1].t - pts[j].t + 1e-12); hz = pts[j].hz * pow(pts[j + 1].hz / pts[j].hz, clampd(x, 0, 1)); }
        if (hz >= 19000) { svf(&sl, L[i], 19000, 0.707, 0, 0); svf(&sr, R[i], 19000, 0.707, 0, 0); svf(&sl2, L[i], 19000, 0.707, 0, 0); svf(&sr2, R[i], 19000, 0.707, 0, 0); continue; }
        L[i] = (float)svf(&sl2, svf(&sl, L[i], hz, 0.9, 0, 0), hz, 0.6, 0, 0); R[i] = (float)svf(&sr2, svf(&sr, R[i], hz, 0.9, 0, 0), hz, 0.6, 0, 0);
    }
}
// turntable: a DJ working the record that is playing. Events act on a group bus (the "record": the loop and its drums)
// before its chain. 1 baby: the platter rocks forward and back over the cue (the sound at the event start), fader open;
// 2 chirp: the same strokes with the fader cut on every back stroke; 3 transformer: the record runs on at normal speed while
// the fader taps it on and off; 4 stutter: the cue re-pushed from the start of each stroke, a hair slow; 5 brake: the
// platter slows to a stop; 6 backspin: the record spun backwards, fast, then silence until the event ends.
typedef struct { int g, type, k; double beat, len, rate, depth, t, t1; } TtEv;
static TtEv g_tt[1024]; static int g_ntt;
static void turntable(int g, double beat, double len, int type, double strokes_per_beat, double depth_s) {
    g_tt[g_ntt++] = (TtEv){g, type, g_k, beat, len, strokes_per_beat, depth_s, 0, 0}; }
static inline double tt_rd(const float *x, long n, double pos) { long i = (long)floor(pos); double f = pos - i; if (i < 0 || i + 1 >= n) return 0; return x[i] * (1 - f) + x[i + 1] * f; }
static void apply_turntable(int g, float *L, float *R, long N) {
    for (int e = 0; e < g_ntt; e++) { TtEv *v = &g_tt[e]; if (v->g != g) continue;
        long s0 = lround(v->t * SR), s1 = lround(v->t1 * SR); if (s0 < 0 || s1 > N || s1 <= s0) continue;
        long n = s1 - s0, pre = (long)(2.0 * SR); long a = s0 - pre < 0 ? 0 : s0 - pre, m = s1 + (long)(2.0 * SR) > N ? N - a : s1 + (long)(2.0 * SR) - a;
        float *cl = malloc(m * sizeof(float)), *cr = malloc(m * sizeof(float)); memcpy(cl, L + a, m * sizeof(float)); memcpy(cr, R + a, m * sizeof(float));
        double cue = s0 - a, D = v->depth * SR, per = (v->t1 - v->t) / (v->len * v->rate);   // seconds per stroke
        double pos = cue, prev = cue, fade = 1; Rng rg = {(uint64_t)(e + 1) * 7919};
        for (long i = 0; i < n; i++) {
            double u = (double)i / SR, ph = fmod(u / per, 1.0), gate = 1;
            switch (v->type) {
            case 1: case 2: pos = cue + D * (0.5 - 0.5 * cos(2 * M_PI * ph)); if (v->type == 2 && pos < prev) gate = 0; break;
            case 3: pos = cue + i; gate = ph < 0.5 ? 1 : 0; break;
            case 4: pos = cue + ph * per * SR * 0.94; gate = ph < 0.85 ? 1 : 0; break;
            case 5: { double sp = 1 - u / ((double)n / SR); pos += sp * sp; break; }
            case 6: { double w = u / ((double)n / SR); pos = cue - (w < 0.6 ? 0.5 * 3.0 * SR * (u * u) / (0.6 * n / SR) : 1e9); if (w >= 0.6) gate = 0; break; }
            }
            if (v->type != 5 && v->type != 6) (void)rg;
            prev = pos;
            double tgt = gate; fade += (tgt - fade) * 0.02;   // ~1 ms fader edges
            double l = tt_rd(cl, m, pos), r = tt_rd(cr, m, pos);
            // a little vinyl friction on fast moves: the signal darkens as the platter speeds (a one-pole on the read)
            L[s0 + i] = (float)(l * fade); R[s0 + i] = (float)(r * fade);
        }
        // a 3 ms crossfade back into the playing record at the end
        long xf = (long)(0.003 * SR); for (long i = 0; i < xf && s1 + i < N; i++) { double w = (double)i / xf; L[s1 + i] = (float)(L[s1 + i] * w + tt_rd(cl, m, pos) * (1 - w) * fade); R[s1 + i] = (float)(R[s1 + i] * w + tt_rd(cr, m, pos) * (1 - w) * fade); }
        free(cl); free(cr);
    }
}
void city_setup(void);
void city_full(void);
double city_loop(void);

static void apply_eq(EqBand *e, int ne, float *L, float *R, long n) {
    for (int b = 0; b < ne; b++) {
        Bq fl, fr;
        switch (e[b].type) {
            case 1: fl = bq_lp(e[b].f, e[b].q > 0 ? e[b].q : 0.707); break;
            case 2: fl = bq_hp(e[b].f, e[b].q > 0 ? e[b].q : 0.707); break;
            case 3: fl = bq_peak(e[b].f, e[b].q, e[b].g); break;
            case 4: fl = bq_lshelf(e[b].f, e[b].g); break;
            default: fl = bq_hshelf(e[b].f, e[b].g); break;
        }
        fr = fl;
        for (long i = 0; i < n; i++) { L[i] = (float)bq(&fl, L[i]); R[i] = (float)bq(&fr, R[i]); }
    }
}
static void apply_comp(double thr, double ratio, double att, double rel, float *L, float *R, long n) {
    Comp c = {thr, ratio, att, rel, 6, -120};
    for (long i = 0; i < n; i++) { double g = comp_gain(&c, fmax(fabs(L[i]), fabs(R[i]))); L[i] *= g; R[i] *= g; }
}
// sidechain ducking from a track's onsets (seconds), sorted
static double *g_onsets[MAXTR]; static int g_nons[MAXTR];
static void apply_duck(int src, double ddb, double att, double rel, float *L, float *R, long n) {
    if (src < 0 || !g_nons[src]) return;
    double D = 1 - db2a(-ddb); int oi = 0; double last = -1e9;
    if (att <= 0) att = 0.004;
    for (long i = 0; i < n; i++) {
        double t = (double)i / SR;
        while (oi < g_nons[src] && g_onsets[src][oi] <= t + att) { last = g_onsets[src][oi]; oi++; }
        double g = 1;
        double x = t - last; // time since the hit's onset; dips over att before it
        if (x > -att) { double dip = x < 0 ? smoothstep((x + att) / att) : 1 - smoothstep(x / rel); g = 1 - D * dip; }
        L[i] *= g; R[i] *= g;
    }
}
static int dcmp(const void *a, const void *b) { double d = *(double *)a - *(double *)b; return d < 0 ? -1 : d > 0; }

static void mkdirs(const char *p) { char b[1024]; snprintf(b, sizeof b, "mkdir -p '%s'", p); system(b); }
static char *slurp(const char *path, long *n) { FILE *f = fopen(path, "rb"); if (!f) return 0; fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); char *b = malloc(*n + 1); fread(b, 1, *n, f); b[*n] = 0; fclose(f); return b; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s full|loop outdir\n", argv[0]); return 2; }
    sin_init(); tp_init();
    g_mode = !strcmp(argv[1], "loop"); const char *out = argv[2]; mkdirs(out);
    const char *engine_dir = getenv("ENGINE") ? getenv("ENGINE") : ".";
    city_setup();
    // SOLO=a,b renders only those tracks (their sends included) to <mode>_solo.wav, unnormalised
    const char *solo = getenv("SOLO");
    if (solo) for (int t = 0; t < g_ntr; t++) { char key[40]; snprintf(key, sizeof key, ",%s,", g_tr[t].name); char hay[1024]; snprintf(hay, sizeof hay, ",%s,", solo); if (!strstr(hay, key)) g_tr[t].mute = 1; }
    long Tint = 0;
    if (g_mode) {
        for (g_k = 0; g_k < 2; g_k++) g_body_beats = city_loop();   // two passes; the first only supplies the wrap's tails
        g_k = 0;
        qsort(g_tempo, g_ntempo, sizeof(TempoPt), tcmp);
        double Traw = b2t_raw(g_body_beats); Tint = lround(Traw * SR); g_tscale = Tint / (Traw * SR); g_period_s = (double)Tint / SR;
        fprintf(stderr, "loop body %.1f beats = %.4f s = %ld frames (tempo scaled by %.9f)\n", g_body_beats, g_period_s, Tint, g_tscale);
    } else { g_k = 0; city_full(); qsort(g_tempo, g_ntempo, sizeof(TempoPt), tcmp); }
    // beats -> seconds, with feel
    double tmax = 0;
    for (int i = 0; i < g_nnote; i++) {
        Note *n = &g_note[i]; Track *tr = &g_tr[n->tr]; Rng r = {n->seed ^ 0xabcdef};
        double b = n->beat;
        double t0 = b2t(b), t1 = b2t(b + n->len);
        if (tr->swing > 0) { double s16 = b * 4; double fr = s16 - floor(s16 + 1e-6); long idx = lround(floor(s16 + 1e-6));
            if (fabs(fr) < 1e-3 && (idx & 1)) { double sh = tr->swing * (b2t(b + 0.25) - t0); t0 += sh; } }
        if (tr->swing8 > 0) { double e8 = b * 2; double fr8 = e8 - floor(e8 + 1e-6); long i8 = lround(floor(e8 + 1e-6));
            if (fabs(fr8) < 1e-3 && (i8 & 1)) t0 += tr->swing8 * (b2t(b + 0.5) - b2t(b)); }
        double hum = tr->human_ms > 0 ? clampd(rgauss(&r), -2.5, 2.5) * tr->human_ms / 1000 : 0;
        n->t = t0 + hum + tr->delay_ms / 1000 + (g_mode ? n->k * g_period_s : 0);
        n->dur = fmax(0.001, t1 - b2t(b));
        if (tr->human_vel > 0) n->vel = clampd(n->vel * (1 + clampd(rgauss(&r), -2.5, 2.5) * tr->human_vel), 0.01, 1);
        if (n->t + n->dur > tmax) tmax = n->t + n->dur;
    }
    if (g_mode) { // cull the first pass down to its last stretch (it only provides the tails and processor state at the wrap)
        double keep = g_period_s - (getenv("PREROLL") ? atof(getenv("PREROLL")) : 25.0); int w = 0;
        for (int i = 0; i < g_nnote; i++) { Note *n = &g_note[i]; if (n->k == 0 && !g_tr[n->tr].gm && n->t + n->dur < keep - 40) continue; g_note[w++] = *n; }   // 40 s margin for long tails
        fprintf(stderr, "culled %d first-pass notes\n", g_nnote - w); g_nnote = w;
        // the first half second of a third pass, so notes humanized or delayed to start before the wrap exist at pass 2's end too
        int n0 = g_nnote; for (int i = 0; i < n0; i++) if (g_note[i].k == 1 && g_note[i].t < 2 * g_period_s + 0.5 - g_period_s + g_period_s && g_note[i].t - g_period_s < 0.5) {
            if (g_nnote >= MAXNOTE) break; g_note[g_nnote] = g_note[i]; g_note[g_nnote].t += g_period_s; g_note[g_nnote].k = 2; g_nnote++; }
    }
    for (int i = 0; i < g_ncc; i++) { Cc *c = &g_cc[i]; c->t = b2t(c->beat) + (g_mode ? c->k * g_period_s : 0); }
    for (int i = 0; i < g_nauto; i++) { AutoPt *a = &g_auto[i]; a->t = b2t(a->beat) + (g_mode ? a->k * g_period_s : 0); }
    for (int i = 0; i < g_ntt; i++) { TtEv *v = &g_tt[i]; double o = g_mode ? v->k * g_period_s : 0; v->t = b2t(v->beat) + o; v->t1 = b2t(v->beat + v->len) + o; }
    if (g_mode) { int n0 = g_nauto; for (int i = 0; i < n0 && g_nauto < 8192; i++) if (g_auto[i].k == 1 && g_auto[i].t - g_period_s < 0.5) { g_auto[g_nauto] = g_auto[i]; g_auto[g_nauto].t += g_period_s; g_auto[g_nauto].k = 2; g_nauto++; } }
    // sustain pedal intervals per track from cc 64 (procedural tracks read them; GM tracks get the cc)
    {
        Cc **pc = malloc(sizeof(Cc *) * (g_ncc + 1));
        for (int t = 0; t < g_ntr; t++) {
            int np = 0; for (int i = 0; i < g_ncc; i++) if (g_cc[i].tr == t && g_cc[i].num == 64) pc[np++] = &g_cc[i];
            if (!np) continue;
            for (int a = 1; a < np; a++) for (int b = a; b > 0 && pc[b]->t < pc[b - 1]->t; b--) { Cc *x = pc[b]; pc[b] = pc[b - 1]; pc[b - 1] = x; }
            g_ped[t] = malloc(sizeof(Ped) * (np + 1)); double down = -1;
            for (int i = 0; i < np; i++) { if (pc[i]->val >= 64) { if (down < 0) down = pc[i]->t; } else if (down >= 0) { g_ped[t][g_nped[t]++] = (Ped){down, pc[i]->t}; down = -1; } }
            if (down >= 0) g_ped[t][g_nped[t]++] = (Ped){down, 1e9};
        }
        free(pc);
    }
    long N = g_mode ? 2 * Tint + SR : (long)((tmax + g_tail_s) * SR);
    g_total = N;
    fprintf(stderr, "%d tracks, %d notes, %.1f s\n", g_ntr, g_nnote, (double)N / SR);
    // onsets for sidechains
    for (int t = 0; t < g_ntr; t++) { g_onsets[t] = malloc(sizeof(double) * (g_nnote + 1)); g_nons[t] = 0; }
    for (int t = 0; t < g_ntr; t++) g_onsets[t] = realloc(g_onsets[t], sizeof(double) * (2 * g_nnote + 2));
    for (int i = 0; i < g_nnote; i++) { Note *n = &g_note[i]; g_onsets[n->tr][g_nons[n->tr]++] = n->t;
        if (g_mode && n->k == 1) g_onsets[n->tr][g_nons[n->tr]++] = n->t + g_period_s; }   // the next pass's hits, so sidechains look ahead across the wrap
    for (int t = 0; t < g_ntr; t++) qsort(g_onsets[t], g_nons[t], sizeof(double), dcmp);
    // ---- GM stems via gmrender ----
    int anygm = 0; for (int t = 0; t < g_ntr; t++) if (g_tr[t].gm && !g_tr[t].mute) anygm = 1;
    char gdir[1024]; snprintf(gdir, sizeof gdir, "%s/gm_%s", out, argv[1]);
    if (anygm) {
        mkdirs(gdir);
        size_t cap = 1 << 24; char *ev = malloc(cap); size_t el = 0;
        // loop mode: Apple's sampler does not render repeated passes identically (voice allocation drifts), so GM stems are
        // rendered for ONE pass plus its tail and folded into an exactly periodic stem below
        double gmlen = g_mode ? g_period_s + g_tail_s : (double)N / SR;
        for (int t = 0; t < g_ntr; t++) if (g_tr[t].gm && !g_tr[t].mute) el += snprintf(ev + el, cap - el, "G %s %d %d %d %.3f\n", g_tr[t].name, g_tr[t].prog, g_tr[t].msb, g_tr[t].lsb, gmlen);
        for (int i = 0; i < g_ncc; i++) { Cc *c = &g_cc[i]; if (!g_tr[c->tr].gm || g_tr[c->tr].mute || (g_mode && c->k != 0)) continue; if (el + 200 > cap) { cap *= 2; ev = realloc(ev, cap); }
            el += snprintf(ev + el, cap - el, "C %s %.6f %d %d\n", g_tr[c->tr].name, c->t, c->num, c->val); }
        for (int i = 0; i < g_nnote; i++) { Note *n = &g_note[i]; if (!g_tr[n->tr].gm || g_tr[n->tr].mute || (g_mode && n->k != 0)) continue; if (el + 200 > cap) { cap *= 2; ev = realloc(ev, cap); }
            double tt = (g_mode && n->t < 0) ? n->t + g_period_s : n->t; el += snprintf(ev + el, cap - el, "N %s %.6f %.6f %d %d\n", g_tr[n->tr].name, fmax(0, tt), n->dur, (int)lround(n->pitch), (int)lround(clampd(n->vel, 0, 1) * 127)); }
        char evp[1100]; snprintf(evp, sizeof evp, "%s/events.txt", gdir);
        long ol = 0; char *old = slurp(evp, &ol);
        if (!old || ol != (long)el || memcmp(old, ev, el)) {
            FILE *f = fopen(evp, "wb"); fwrite(ev, 1, el, f); fclose(f);
            char cmd[3000]; snprintf(cmd, sizeof cmd, "'%s/gmrender' '%s' '%s' >&2", engine_dir, evp, gdir);
            if (system(cmd)) { fprintf(stderr, "gmrender failed\n"); return 1; }
        } else fprintf(stderr, "GM stems cached\n");
        free(old); free(ev);
    }
    // ---- render tracks ----
    float *ML = calloc(N, sizeof(float)), *MR = calloc(N, sizeof(float));
    float *GL[5] = {0}, *GR[5] = {0}; for (int g = 1; g < 5; g++) { GL[g] = calloc(N, sizeof(float)); GR[g] = calloc(N, sizeof(float)); }
    float *BL[4], *BR[4]; for (int b = 0; b < 4; b++) { BL[b] = calloc(N, sizeof(float)); BR[b] = calloc(N, sizeof(float)); }
    for (int t = 0; t < g_ntr; t++) {
        Track *tr = &g_tr[t]; if (tr->mute) continue;
        tr->L = calloc(N, sizeof(float)); tr->R = calloc(N, sizeof(float));
        if (tr->gm) {
            char p[1200]; snprintf(p, sizeof p, "%s/%s.f32", gdir, tr->name); long nb = 0; char *d = slurp(p, &nb);
            if (!d) { fprintf(stderr, "missing GM stem %s\n", p); return 1; }
            float *f = (float *)d; long fr = nb / 8;
            if (g_mode) { // fold the single pass (and its tail) into one period, then tile it across all passes
                double *pl = calloc(Tint, sizeof(double)), *pr = calloc(Tint, sizeof(double));
                for (long i = 0; i < fr; i++) { pl[i % Tint] += f[2 * i]; pr[i % Tint] += f[2 * i + 1]; }
                for (long i = 0; i < N; i++) { tr->L[i] = (float)pl[i % Tint]; tr->R[i] = (float)pr[i % Tint]; }
                free(pl); free(pr);
            } else for (long i = 0; i < fr && i < N; i++) { tr->L[i] = f[2 * i]; tr->R[i] = f[2 * i + 1]; }
            free(d);
        } else for (int i = 0; i < g_nnote; i++) if (g_note[i].tr == t) tr->fn(tr, &g_note[i], tr->L, tr->R, N);
        apply_eq(tr->eq, tr->neq, tr->L, tr->R, N);
        apply_autolp(t, tr->L, tr->R, N);
        if (tr->chorus_mix > 0) { // two modulated taps, quadrature LFOs, locked to the loop
            double hz = tr->chorus_hz > 0 ? tr->chorus_hz : 0.6; if (g_mode) hz = fmax(1, lround(hz * g_period_s)) / g_period_s;
            long D = (long)((tr->chorus_ms + tr->chorus_depth_ms * 2 + 2) * SR / 1000) + 4; float *bl = calloc(N, sizeof(float)), *br = calloc(N, sizeof(float));
            memcpy(bl, tr->L, N * sizeof(float)); memcpy(br, tr->R, N * sizeof(float));
            for (long i = D; i < N; i++) { double tt = (double)i / SR;
                double dl = (tr->chorus_ms + tr->chorus_depth_ms * (1 + sin(TAU * hz * tt))) * SR / 1000, dr = (tr->chorus_ms + tr->chorus_depth_ms * (1 + cos(TAU * hz * tt))) * SR / 1000;
                long a = i - (long)dl; double fa = dl - (long)dl; double vl = bl[a] * (1 - fa) + bl[a - 1] * fa;
                long b = i - (long)dr; double fb = dr - (long)dr; double vr = br[b] * (1 - fb) + br[b - 1] * fb;
                tr->L[i] = (float)(bl[i] * (1 - tr->chorus_mix * 0.5) + vl * tr->chorus_mix); tr->R[i] = (float)(br[i] * (1 - tr->chorus_mix * 0.5) + vr * tr->chorus_mix); }
            free(bl); free(br);
        }
        if (tr->crush_sr > 0) { // SP-1200 style: anti-alias low-pass, then sample-and-hold at crush_sr, quantise to crush_bits
            Bq a1 = bq_lp(tr->crush_sr * 0.42, 0.707), a2 = a1, b1 = a1, b2 = a1; double step = SR / tr->crush_sr, acc = 0, hl = 0, hr = 0;
            double q = tr->crush_bits > 0 ? pow(2, tr->crush_bits - 1) : 0;
            long per = g_mode ? Tint : N, prevk = -1; (void)acc; (void)step;   // the hold grid repeats with the loop, so passes match
            for (long i = 0; i < N; i++) { double l = bq(&a2, bq(&a1, tr->L[i])), rr2 = bq(&b2, bq(&b1, tr->R[i]));
                long k = (long)floor((double)(i % per) * tr->crush_sr / SR);
                if (k != prevk) { prevk = k; hl = q > 0 ? floor(l * q + 0.5) / q : l; hr = q > 0 ? floor(rr2 * q + 0.5) / q : rr2; }
                tr->L[i] = (float)hl; tr->R[i] = (float)hr; }
            // the hold's images above crush_sr/2 were a flat fizz to 24 kHz (and inharmonic lines); a reconstruction low-pass
            Bq c1 = bq_lp(tr->crush_sr * 0.45, 0.707), c2 = c1, d1 = c1, d2 = c1;
            for (long i = 0; i < N; i++) { tr->L[i] = (float)bq(&c2, bq(&c1, tr->L[i])); tr->R[i] = (float)bq(&d2, bq(&d1, tr->R[i])); }
        }
        if (tr->haas_ms > 0) { long d = (long)(tr->haas_ms * SR / 1000); for (long i = N - 1; i >= d; i--) tr->R[i] = tr->R[i - d]; for (long i = 0; i < d; i++) tr->R[i] = 0; }
        if (tr->drive > 0) { double sl = 0, sr = 0; for (long i = 0; i < N; i++) { double k = tanh(1 + tr->drive);   // anti-aliased (ADAA) saturation
            tr->L[i] = (float)(tr->L[i] * (1 - tr->drive_mix) + tr->drive_mix * tanh_aa(&sl, tr->L[i] * (1 + tr->drive)) / k);
            tr->R[i] = (float)(tr->R[i] * (1 - tr->drive_mix) + tr->drive_mix * tanh_aa(&sr, tr->R[i] * (1 + tr->drive)) / k); } }
        if (tr->cthr != 0) apply_comp(tr->cthr, tr->cratio, tr->catt > 0 ? tr->catt : 0.005, tr->crel > 0 ? tr->crel : 0.1, tr->L, tr->R, N);
        if (tr->duck_src >= 0) apply_duck(tr->duck_src, tr->duck_db, tr->duck_att, tr->duck_rel, tr->L, tr->R, N);
        double g = db2a(tr->gain_db), pa = (tr->pan + 1) * M_PI / 4, gl = cos(pa) * 1.41421356, gr = sin(pa) * 1.41421356;
        if (gl > 1) gl = 1; if (gr > 1) gr = 1;  // balance law for stereo sources
        double sumsq = 0;
        for (long i = 0; i < N; i++) {
            double l = tr->L[i], r = tr->R[i], m = (l + r) / 2, s = (l - r) / 2 * tr->width; l = (m + s) * g * gl; r = (m - s) * g * gr;
            tr->L[i] = (float)l; tr->R[i] = (float)r; if (tr->group) { GL[tr->group][i] += (float)l; GR[tr->group][i] += (float)r; } else { ML[i] += (float)l; MR[i] += (float)r; } sumsq += l * l + r * r;
            for (int b = 0; b < 4; b++) if (tr->send[b] > 0) { BL[b][i] += (float)(l * tr->send[b]); BR[b][i] += (float)(r * tr->send[b]); }
        }
        double lu = lufs(tr->L, tr->R, N, 0, 0, 0);
        if (getenv("STEMWIN")) { // band levels of this track inside a window (s): where it sits in the spectrum
            double w0 = 0, w1 = 0; sscanf(getenv("STEMWIN"), "%lf,%lf", &w0, &w1); long a = (long)(w0 * SR) + (g_mode ? Tint : 0), z = (long)(w1 * SR) + (g_mode ? Tint : 0); if (z > N) z = N;
            static const double ed[8] = {20, 60, 250, 500, 2000, 5000, 10000, 20000};
            if (t == 0) fprintf(stderr, "  %-10s %8s %8s %8s %8s %8s %8s %8s   (dB, mean power per band in window)\n", "", "sub", "bass", "lowmid", "mid", "pres", "brill", "air");
            fprintf(stderr, "  %-10s", tr->name);
            for (int b = 0; b < 7; b++) { Bq h1 = bq_hp(ed[b], 0.54), h2 = bq_hp(ed[b], 1.31), l1 = bq_lp(ed[b + 1], 0.54), l2 = bq_lp(ed[b + 1], 1.31); double e = 0;
                for (long i = (a > SR ? a - SR : 0); i < z; i++) { double v = bq(&l2, bq(&l1, bq(&h2, bq(&h1, (tr->L[i] + tr->R[i]) * 0.5)))); if (i >= a) e += v * v; }
                fprintf(stderr, " %8.1f", 10 * log10(e / (z - a + 1) + 1e-20)); }
            fprintf(stderr, "\n");
        }
        if (g_mode) { // per-track seam check: the ends of the two passes
            double num = 0, den = 0; for (long i = 0; i < SR / 2; i++) { long a1 = Tint - SR / 2 + i, a2 = 2 * Tint - SR / 2 + i; double e = tr->L[a1] - tr->L[a2]; num += e * e; den += tr->L[a2] * tr->L[a2]; }
            if (num > 1e-12 * (den + 1e-9)) fprintf(stderr, "  seam residual %-10s %.1f dB\n", tr->name, 10 * log10(num / (den + 1e-30) + 1e-30));
            if (num > 1e-12 * (den + 1e-9) && getenv("SEAMDBG")) { // where in the last half second do the two pass ends start to differ?
                for (long i = 0; i < SR / 2; i++) { long a1 = Tint - SR / 2 + i, a2 = 2 * Tint - SR / 2 + i; if (fabs(tr->L[a1] - tr->L[a2]) > 1e-4) { fprintf(stderr, "    first difference %.4f s before the pass end\n", (SR / 2 - i) / (double)SR); break; } }
                for (int k = 0; k < g_nnote; k++) { Note *nn = &g_note[k]; if (nn->tr != t) continue; double rel = nn->t - (nn->k == 0 ? 0 : nn->k == 1 ? g_period_s : 2 * g_period_s);
                    if (rel > g_period_s - 3 || rel < 0.5) fprintf(stderr, "    k%d beat %.3f t_rel %.4f dur %.3f vel %.3f\n", nn->k, nn->beat, rel, nn->dur, nn->vel); }
            }
        }
        fprintf(stderr, "  %-10s %s %5d notes  %6.1f LUFS (pre-master)\n", tr->name, tr->gm ? "gm " : "syn", g_nons[t], lu);
        free(tr->L); free(tr->R); tr->L = tr->R = 0;
    }
    // ---- group buses: EQ, saturation, compression (parallel blend), then into the master ----
    for (int g = 1; g < 5; g++) {
        Group *G = &g_grp[g]; int used = 0; for (int t = 0; t < g_ntr; t++) if (g_tr[t].group == g && !g_tr[t].mute) used = 1;
        if (!used) continue;
        apply_turntable(g, GL[g], GR[g], N);
        float *dl = 0, *dr = 0; if (G->parallel > 0) { dl = malloc(N * sizeof(float)); dr = malloc(N * sizeof(float)); memcpy(dl, GL[g], N * sizeof(float)); memcpy(dr, GR[g], N * sizeof(float)); }
        apply_eq(G->eq, G->neq, GL[g], GR[g], N);
        if (G->drive > 0) { double k = tanh(1 + G->drive), sl = 0, sr = 0; for (long i = 0; i < N; i++) { GL[g][i] = (float)(tanh_aa(&sl, GL[g][i] * (1 + G->drive)) / k); GR[g][i] = (float)(tanh_aa(&sr, GR[g][i] * (1 + G->drive)) / k); } }
        if (G->cthr != 0) apply_comp(G->cthr, G->cratio, G->catt > 0 ? G->catt : 0.01, G->crel > 0 ? G->crel : 0.1, GL[g], GR[g], N);
        if (G->duck_db > 0) apply_duck(G->duck_src, G->duck_db, 0.004, G->duck_rel, GL[g], GR[g], N);
        double gg = db2a(G->gain_db);
        for (long i = 0; i < N; i++) { double l = GL[g][i] * gg, r = GR[g][i] * gg; if (dl) { l = dl[i] * (1 - G->parallel) + l * G->parallel; r = dr[i] * (1 - G->parallel) + r * G->parallel; } ML[i] += (float)l; MR[i] += (float)r; }
        free(dl); free(dr);
    }
    // ---- buses ----
    for (int b = 0; b < 4; b++) {
        Bus *bu = &g_bus[b]; if (!bu->type) continue;
        float *wl = calloc(N, sizeof(float)), *wr = calloc(N, sizeof(float));
        if (bu->type == 1) {
            RevCfg c = bu->rev; if (g_mode && c.mod_hz > 0) c.mod_hz = fmax(1, lround(c.mod_hz * g_period_s)) / g_period_s; // lock modulation to the loop
            Rev r; rev_init(&r, c);
            for (long i = 0; i < N; i++) { double l, rr2; rev_tick(&r, BL[b][i], BR[b][i], &l, &rr2); wl[i] = (float)l; wr[i] = (float)rr2; }
        } else if (bu->type == 2) {
            long dl = lround(bu->dl_s * SR), dr = lround(bu->dr_s * SR); Bq lf = bq_lp(bu->lp > 0 ? bu->lp : 6000, 0.7), hf = bq_hp(bu->hp > 0 ? bu->hp : 200, 0.7), lf2 = lf, hf2 = hf;
            // ping-pong: L echoes at d, 3d, 5d...; R at 2d, 4d... (dl = dr = d); each echo filtered
            for (long i = 0; i < N; i++) {
                double yl = i >= dl ? (BL[b][i - dl] + BR[b][i - dl]) * 0.5 + bu->fb * wr[i - dl] : 0;
                double yr = i >= dr ? wl[i - dr] : 0;
                wl[i] = (float)bq(&lf, bq(&hf, yl)); wr[i] = (float)bq(&lf2, bq(&hf2, yr));
            }
        }
        if (bu->duck_src >= 0 && bu->duck_db > 0) apply_duck(bu->duck_src, bu->duck_db, 0.004, bu->duck_rel, wl, wr, N);
        if (bu->gate_src > 0 && g_nons[bu->gate_src]) { // gated reverb: open for gate_hold after each onset of gate_src, then a fast close
            int oi = 0; double last = -1e9; int src = bu->gate_src;
            for (long i = 0; i < N; i++) { double t = (double)i / SR; while (oi < g_nons[src] && g_onsets[src][oi] <= t) last = g_onsets[src][oi++];
                double x = t - last, g = x < bu->gate_hold ? 1 : exp(-(x - bu->gate_hold) / bu->gate_rel); wl[i] *= (float)g; wr[i] *= (float)g; }
        }
        double g = db2a(bu->gain_db);
        for (long i = 0; i < N; i++) { ML[i] += (float)(wl[i] * g); MR[i] += (float)(wr[i] * g); }
        free(wl); free(wr);
    }
    // ---- master ----
    apply_eq(g_master.eq, g_master.neq, ML, MR, N);
    if (g_master.side > 0) for (long i = 0; i < N; i++) { double m = (ML[i] + MR[i]) / 2, sd = (ML[i] - MR[i]) / 2 * g_master.side; ML[i] = (float)(m + sd); MR[i] = (float)(m - sd); }   // overall image width (boom bap is near mono)
    if (g_master.width_lo_hz > 0) { // mono the lows
        // the side signal is high-passed (24 dB/oct) instead of subtracting a low-passed copy: the subtraction left the lows'
        // side half there (the low-pass's phase shift), and the sub measured 0.5-0.6 correlated instead of mono
        Bq a = bq_hp(g_master.width_lo_hz, 0.707), b2 = a;
        for (long i = 0; i < N; i++) { double m = (ML[i] + MR[i]) / 2, sd = bq(&a, bq(&b2, (ML[i] - MR[i]) / 2)); ML[i] = (float)(m + sd); MR[i] = (float)(m - sd); }
    }
    if (g_master.cthr != 0) apply_comp(g_master.cthr, g_master.cratio, g_master.catt, g_master.crel, ML, MR, N);
    if (g_master.tape > 0) { // the tape sees the mix at a fixed level (-18 LUFS), so the same setting saturates every city alike
        long q0 = g_mode ? Tint : 0, qn = g_mode ? Tint : N; double I0 = lufs(ML + q0, MR + q0, qn, 0, 0, 0), pre = I0 > -150 ? db2a(-18 - I0) : 1;
        double k = tanh(1 + g_master.tape), sl = 0, sr = 0, g0 = (1 + g_master.tape) * pre;
        for (long i = 0; i < N; i++) { ML[i] = (float)(tanh_aa(&sl, ML[i] * g0) / k / pre); MR[i] = (float)(tanh_aa(&sr, MR[i] * g0) / k / pre); } }
    if (g_master.room_db < 0) { long per = g_mode ? Tint : N; double a = db2a(g_master.room_db);
        // pink-ish noise (Paul Kellett's economy filter) from a per-index hash, so the floor repeats exactly with the loop
        double b0 = 0, b1 = 0, b2 = 0, c0 = 0, c1 = 0, c2 = 0; Bq hl = bq_hp(150, 0.707), hr = hl;   // no room rumble: uncorrelated lows would undo the mono sub
        for (long i = 0; i < N; i++) { uint64_t h = mix64((uint64_t)(i % per) * 0x9E3779B97F4A7C15ULL + 77); double wl = ((h >> 11) * (1.0 / 9007199254740992.0)) * 2 - 1;
            uint64_t h2 = mix64(h ^ 0xD1B54A32D192ED03ULL); double wr = ((h2 >> 11) * (1.0 / 9007199254740992.0)) * 2 - 1;
            b0 = 0.99765 * b0 + wl * 0.0990460; b1 = 0.96300 * b1 + wl * 0.2965164; b2 = 0.57000 * b2 + wl * 1.0526913;
            c0 = 0.99765 * c0 + wr * 0.0990460; c1 = 0.96300 * c1 + wr * 0.2965164; c2 = 0.57000 * c2 + wr * 1.0526913;
            ML[i] += (float)bq(&hl, a * 0.25 * (b0 + b1 + b2 + wl * 0.1848)); MR[i] += (float)bq(&hr, a * 0.25 * (c0 + c1 + c2 + wr * 0.1848)); } }
    if (g_master.top_hz > 0) { Bq a1 = bq_lp(g_master.top_hz, 0.707), b1 = a1; for (long i = 0; i < N; i++) { ML[i] = (float)bq(&a1, ML[i]); MR[i] = (float)bq(&b1, MR[i]); } }
    // measure region
    long r0 = g_mode ? Tint : 0, rn2 = g_mode ? Tint : N;
    // trim trailing silence in full mode
    if (!g_mode) { long e = N; while (e > SR && fabs(ML[e - 1]) < 1e-5 && fabs(MR[e - 1]) < 1e-5) e--; rn2 = e + SR / 2 < N ? e + SR / 2 : N; }
    float *WL = malloc(N * sizeof(float)), *WR = malloc(N * sizeof(float));
    double gain = 0, I = 0;
    if (solo) { char sp2[1200]; snprintf(sp2, sizeof sp2, "%s/%s_solo.wav", out, argv[1]); long a = g_mode ? Tint : 0, nn = g_mode ? Tint : rn2;
        double g = db2a(getenv("MGAIN") ? atof(getenv("MGAIN")) : 0); for (long i = 0; i < N; i++) { WL[i] = (float)(ML[i] * g); WR[i] = (float)(MR[i] * g); }
        wav_write24(sp2, WL + a, WR + a, nn); fprintf(stderr, "solo: %.2f LUFS (at the mix's master gain guess)\n", lufs(WL + a, WR + a, nn, 0, 0, 0)); return 0; }
    for (int it = 0; it < 4; it++) {
        double g = db2a(gain);
        for (long i = 0; i < N; i++) { WL[i] = (float)(ML[i] * g); WR[i] = (float)(MR[i] * g); }
        limit(WL, WR, N, g_master.ceil_db - 0.3, g_master.lim_rel);
        I = lufs(WL + r0, WR + r0, rn2, 0, 0, 0);
        if (it == 0 && I < -150) break;
        gain += g_master.lufs - I;
        if (fabs(g_master.lufs - I) < 0.05) break;
    }
    double tp = a2db(true_peak(WL + r0, WR + r0, rn2));
    fprintf(stderr, "master: %.2f LUFS, true peak %.2f dBTP, master gain %.2f dB\n", I, tp, gain);
    char path[1200];
    if (g_mode) {
        snprintf(path, sizeof path, "%s/loop.wav", out); wav_write24(path, WL + Tint, WR + Tint, Tint);
        // seam: the last 0.5 s of pass 1 (which had 25 s of context) against the last 0.5 s of pass 2; identical ends mean
        // the state at the wrap equals the state that opened pass 2, so the wrap is the true continuation
        double md = 0, mx = 0, num = 0, den = 0; long W = SR / 2;
        for (long i = 1; i < Tint; i++) md += fabsf(WL[Tint + i] - WL[Tint + i - 1]); md /= Tint;
        for (long i = 0; i < W; i++) { long a1 = Tint - W + i, a2 = 2 * Tint - W + i; double e = WL[a1] - WL[a2], e2 = WR[a1] - WR[a2]; num += e * e + e2 * e2; den += WL[a2] * WL[a2] + WR[a2] * WR[a2]; if (fabs(e) > mx) mx = fabs(e); }
        double jump = fabs(WL[Tint] - WL[2 * Tint - 1]), truejump = fabs(WL[Tint] - WL[Tint - 1]);
        fprintf(stderr, "seam: wrap step %.5f vs true step %.5f (mean step %.5f); pass ends residual %.1f dB, max abs diff %.6f\n", jump, truejump, md, 10 * log10(num / (den + 1e-30) + 1e-30), mx);
        FILE *f; snprintf(path, sizeof path, "%s/seam.txt", out); f = fopen(path, "w");
        fprintf(f, "frames %ld\nseconds %.6f\nwrap_step %.6f\ntrue_step %.6f\nmean_step %.6f\nresidual_db %.2f\nmax_abs_diff %.7f\nlufs %.2f\ntrue_peak_dbtp %.2f\n",
                Tint, g_period_s, jump, truejump, md, 10 * log10(num / (den + 1e-30) + 1e-30), mx, I, tp);
        fclose(f);
    } else { snprintf(path, sizeof path, "%s/full.wav", out); wav_write24(path, WL, WR, rn2); }
    fprintf(stderr, "wrote %s\n", path);
    if (g_on_done) g_on_done(out);
    return 0;
}
