// choir.h: a men's choir built from solo singers. Each singer is his own sampler track playing sustained notes cut from
// VocalSet (CC BY 4.0; tools/samples/vocalset_extract.py makes the samples and the manifests choir_<singer>_<vowel>.txt), with
// his own place in the room, his own timing and tuning. A score writes one CHOIR part; choir_post() hands every note of it
// to each singer who can sing it (a sample root within CHOIR_REACH semitones), so a unison is ten men, not one man ten times.
// Notes below every singer's reach go up an octave (the score's G1 does not exist in a human chest), above it down.
// No vibrato is added and nothing is stretched further than CHOIR_REACH: both were what made sampled parts read as synthetic.
#pragma once
#include "orch.h"
#define CHOIR_REACH 3
#ifndef CHOIR_OM
#define CHOIR_OM OM   // where the manifests are, relative to the folder the score runs in
#endif
typedef struct { int tr, nroot; int root[128]; } Singer;
static Singer g_singer[24]; static int g_nsinger, g_choir_in = -1;
static void syn_choir_none(Track *tr, const Note *n, float *L, float *R, long nf) { (void)tr; (void)n; (void)L; (void)R; (void)nf; }
// names: singer ids ("male1" ...); vowel: "ah" or "oh". Returns the silent track the score's CHOIR part should be mapped to.
static int choir_setup(const char **names, int n, const char *vowel, double gain_db, double send) {
    g_choir_in = syn_track("choir in", syn_choir_none); g_tr[g_choir_in].mute = 1;
    for (int i = 0; i < n && g_nsinger < 24; i++) {
        char man[256], nm[32]; snprintf(man, sizeof man, CHOIR_OM "choir_%s_%s.txt", names[i], vowel);
        FILE *f = fopen(man, "r"); if (!f) { fprintf(stderr, "choir: no manifest %s, singer skipped\n", man); continue; }
        int c = fgetc(f); fclose(f); if (c == EOF) { fprintf(stderr, "choir: empty manifest %s, singer skipped\n", man); continue; }
        snprintf(nm, sizeof nm, "choir %s", names[i]); int t = smp_track(nm, man); Singer *s = &g_singer[g_nsinger];
        SampleSet *ss = (SampleSet *)g_tr[t].user; s->tr = t; s->nroot = 0;
        for (int k = 0; k < ss->n && s->nroot < 128; k++) { int seen = 0; for (int j = 0; j < s->nroot; j++) if (s->root[j] == ss->s[k].root) seen = 1; if (!seen) s->root[s->nroot++] = ss->s[k].root; }
        double x = n > 1 ? (double)i / (n - 1) : 0.5;   // seated left to right in the order given
        g_tr[t].sp[0] = 0.3; g_tr[t].sp[2] = 35; g_tr[t].sp[6] = 5; g_tr[t].sp[10] = 1;   // release, a 35 ms fade-in, 5 cents of drift, looped sustains
        g_tr[t].pan = -0.65 + 1.3 * x; g_tr[t].delay_ms = (i * 17) % 43; g_tr[t].human_ms = 16; g_tr[t].human_vel = 0.1;
        g_tr[t].send[0] = send * (0.9 + 0.2 * ((i * 7) % 5) / 4.0); g_tr[t].gain_db = gain_db; eq(t, 2, 70, 0.7, 0);
        g_nsinger++;
    }
    fprintf(stderr, "choir: %d singers on \"%s\"\n", g_nsinger, vowel);
    return g_choir_in;
}
static int singer_can(const Singer *s, int pitch) { for (int j = 0; j < s->nroot; j++) if (abs(s->root[j] - pitch) <= CHOIR_REACH) return 1; return 0; }
static void choir_post(int n0) {
    if (g_choir_in < 0 || !g_nsinger) return;
    int lo = 127, hi = 0, n1 = g_nnote; for (int i = 0; i < g_nsinger; i++) for (int j = 0; j < g_singer[i].nroot; j++) { if (g_singer[i].root[j] < lo) lo = g_singer[i].root[j]; if (g_singer[i].root[j] > hi) hi = g_singer[i].root[j]; }
    for (int i = n0; i < n1; i++) { Note a = g_note[i]; if (a.tr != g_choir_in) continue;
        int p = (int)lround(a.pitch); while (p < lo - CHOIR_REACH) p += 12; while (p > hi + CHOIR_REACH) p -= 12;
        int sung = 0;
        for (int k = 0; k < g_nsinger; k++) if (singer_can(&g_singer[k], p)) { note(g_singer[k].tr, a.beat, a.len, p, a.vel); sung++; }
        if (!sung) fprintf(stderr, "choir: nobody can sing midi %d at beat %.2f\n", p, a.beat);
    }
}
