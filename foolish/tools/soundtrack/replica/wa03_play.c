// wa03_play.c: plays a note-level recreation of the Warsaw clip wa_03 (cello and piano) from exact-time EVENTS.
// Run from a replica directory holding score.txt (TEMPO, SECTION, "EVENTS PIANO:", "EVENTS CELLO:", "EVENTS PEDAL:" lines):
//   ./wa03_play full OUTDIR
// The instrument is a parameter, set by environment variables (tools/replica/wa03_build.sh sets them from its arguments):
//   CELLO = lyria | lyria_vib | vsco | vsco_vib | none   lyria: the flat Lyria cello set (lyria_cello_flat.txt, sampler loop mode 2),
//                                                         the engine vibrato is the switch (_vib = 10.1 cents at 5.35 Hz, the measured values);
//                                                         vsco: the VSCO 2 CE cello section sustains (vc_sus.txt, loop 1, bow skip), _vib adds
//                                                         the engine vibrato on top of the recorded one
//   PIANO = salamander                                    (the only piano set so far; the name is the parameter that a later set plugs into)
//   PIANO_EQ, CELLO_EQ = "type:f:q:gain,..." (1 lp 2 hp 3 peak 4 lshelf 5 hshelf)
//   PIANO_ATT_MS (linear attack fade of every piano note, default 0)
//   MASTER_EQ = the same syntax, on the master bus
//   HALL_RT s (default 1.8), HALL_SEND 0..1 (default 0.25), HALL_DB bus gain (default -5), CELLO_DB, PIANO_DB (default 0)
// EVENTS PEDAL: pitch is ignored, len is the time the damper stays up (the engine's cc 64 on both piano tracks).
// Every note is exact: no humanise, no early start, no random detune (sp[15]), because the timing is the point.
// Build: clang -O2 -w -DMANDIR='"<abs>/samples/manifests/"' -o wa03_play wa03_play.c -lm   Run: SAMPLES=<samples dir> ENGINE=<engine dir> ./wa03_play full out
#include "../engine/orch.h"
#include <strings.h>
#ifdef MANDIR
#undef OM
#define OM MANDIR   /* -DMANDIR=\"/abs/path/samples/manifests/\" (wa03_build.sh): the manifests are not at ../samples from a replica dir */
#endif

static int PIANO, CELLO, PEDAL;
static void syn_none(Track *tr, const Note *n, float *L, float *R, long nf) { (void)tr; (void)n; (void)L; (void)R; (void)nf; }
static double envd(const char *k, double d) { const char *v = getenv(k); return v && *v ? atof(v) : d; }
// PIANO_EQ / CELLO_EQ = "type:f:q:gain,type:f:q:gain" (type 1 lp 2 hp 3 peak 4 lshelf 5 hshelf), applied with the engine's eq() to that track
static void env_eq(int t, const char *k) {
    const char *v = getenv(k); if (!v || !*v) return;
    char buf[256]; snprintf(buf, sizeof buf, "%s", v);
    for (char *b = strtok(buf, ","); b; b = strtok(0, ",")) { int ty; double f, q, g; if (sscanf(b, "%d:%lf:%lf:%lf", &ty, &f, &q, &g) == 4) eq(t, ty, f, q, g); }
}
static int map(const char *part, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1; *dp = 60;
    if (!strcasecmp(part, "PIANO")) return PIANO;
    if (!strcasecmp(part, "CELLO")) return CELLO;
    if (!strcasecmp(part, "PEDAL")) { *dp = -60; return PEDAL; }
    fprintf(stderr, "wa03_play: unknown part '%s'\n", part); exit(1);
}
void city_setup(void) {
    g_on_done = sections_out;
    const char *pi = getenv("PIANO"); if (!pi || !*pi) pi = "salamander";
    const char *ce = getenv("CELLO"); if (!ce || !*ce) ce = "lyria";
    if (strcmp(pi, "salamander")) { fprintf(stderr, "wa03_play: PIANO=%s unknown\n", pi); exit(1); }
    PIANO = smp_track("piano", OM "salamander.txt");
    Track *p = &g_tr[PIANO]; p->sp[0] = 0.35; p->sp[1] = 0.35; p->sp[5] = 0; p->sp[9] = 1; p->sp[15] = 1;
    p->human_ms = 0; p->human_vel = 0; p->delay_ms = 0; p->send[0] = envd("HALL_SEND", 0.25); p->gain_db = envd("PIANO_DB", 0); p->width = 0.55; p->sp[2] = envd("PIANO_ATT_MS", 0);
    env_eq(PIANO, "PIANO_EQ");
    PEDAL = syn_track("pedal", syn_none); g_tr[PEDAL].mute = 1;
    CELLO = -1;
    int none = !strcmp(ce, "none");
    int lyria = !strncmp(ce, "lyria", 5), vsco = !strncmp(ce, "vsco", 4), vib = strstr(ce, "_vib") != 0;
    if (!none && !lyria && !vsco) { fprintf(stderr, "wa03_play: CELLO=%s unknown\n", ce); exit(1); }
    if (lyria) {
        CELLO = smp_track("cello", OM "lyria_cello_flat.txt");
        g_tr[CELLO].sp[0] = 1.46 / 2.303; g_tr[CELLO].sp[10] = 2; g_tr[CELLO].sp[2] = 25;
    } else if (vsco) {
        CELLO = o_smp("cello", OM "vc_sus.txt", 0.45, 1, 0, 0, 0, 0);
    }
    if (CELLO < 0) { CELLO = syn_track("cello", syn_none); g_tr[CELLO].mute = 1; }
    else {
        Track *c = &g_tr[CELLO];
        c->human_ms = 0; c->human_vel = 0; c->delay_ms = 0; c->sp[15] = 1; c->send[0] = envd("HALL_SEND", 0.25); c->gain_db = envd("CELLO_DB", 0);
        c->sp[13] = vib ? 10.1 : 0; c->sp[14] = vib ? 5.35 : 0;
    }
    if (CELLO >= 0 && !g_tr[CELLO].mute) env_eq(CELLO, "CELLO_EQ");
    double rt = envd("HALL_RT", 1.8);
    g_bus[0] = (Bus){.type = 1, .rev = {rt, rt * 0.5, 12, 1.4, 180, 7000, 0.14, 1.0, 0.2, 0.25}, .gain_db = envd("HALL_DB", -5), .duck_src = -1};
    { const char *v = getenv("MASTER_EQ"); if (v && *v) { char buf[256]; snprintf(buf, sizeof buf, "%s", v);
        for (char *b = strtok(buf, ","); b; b = strtok(0, ",")) { int ty; double f, q, g; if (sscanf(b, "%d:%lf:%lf:%lf", &ty, &f, &q, &g) == 4 && g_master.neq < 8) g_master.eq[g_master.neq++] = (EqBand){ty, f, q, g}; } } }
    g_tail_s = 3;
}
double city_loop(void) {
    int n0 = g_nnote; g_nsec = 0; score_txt("score.txt", map, 0);
    for (int i = n0; i < g_nnote; i++) if (g_note[i].tr == PEDAL) {
        double b = g_note[i].beat, e = b + g_note[i].len;
        cc(PIANO, b, 64, 127); cc(PIANO, e, 64, 0);
    }
    return score_total_bars() * 4.0;
}
void city_full(void) { city_loop(); }
