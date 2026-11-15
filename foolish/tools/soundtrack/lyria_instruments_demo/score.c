// Lyria sampler instruments, proof render: each instrument plays alone in its own section at 90 BPM through the engine's
// sampler (smp_track), no reverb, no effects, no humanising. Per instrument: every sampled root as a 2-beat note, a slow
// one-octave scale in semitone steps from the lowest root (so the notes between roots are resampled), three 4-beat notes.
// Looping of sustained samples (sp[10]) is on for the cello, haegeum and trumpet; the legato-cut ones get a 25 ms attack
// fade (sp[2]). score.txt is written by make_score.py from the manifests. Render from this folder:
//   clang -O2 -w -o score score.c && SAMPLES=/Users/alex/Dev/soundtrack-samples ENGINE=../engine ./score full out
#include "../engine/orch.h"
#define LM "../samples/manifests/"
static int CELLO, HAEGEUM, TRUMPET;
static int t_map(const char *p, double *tp, double *vs, double *dp) {
    *tp = 0; *vs = 1;
    if (!strcmp(p, "CELLO")) return CELLO;
    if (!strcmp(p, "HAEGEUM")) return HAEGEUM;
    if (!strcmp(p, "TRUMPET")) return TRUMPET;
    return -1;
}
static int lyria_track(const char *name, const char *manifest, int loop, double rel) {
    int t = smp_track(name, manifest);
    g_tr[t].sp[0] = rel;                  // release time constant, s
    g_tr[t].sp[10] = loop;                // sustained samples: 2 = long loop (a drawn jump target per pass, correlation matched, constant power crossfade)
    g_tr[t].sp[2] = 25;                   // attack fade for the legato-cut samples
    g_tr[t].human_ms = 0; g_tr[t].human_vel = 0; g_tr[t].send[0] = 0;
    return t;
}
// Releases, from release_measure.py on the natural note ends of the approved source clips (a voiced note followed by 300 ms
// 40 dB under the clip peak). sp[0] = median T20 / 2.303 (an exponential of time constant rel falls 20 dB in 2.303 rel).
//   cello:   1 note end,  T20 1.46 s, T40 1.91 s, slopes -14 then -44 dB/s (single stage)       rel 0.634 s, no reverb
//   haegeum: 2 note ends, median T20 0.165 s, T40 0.785 s, slopes -124 then -32 dB/s (two stage: a tail)
//            rel 0.0716 s, plus a room reverb bus (decay 1.9 s = 60 dB at the measured 32 dB/s) with the send set by measurement
//   trumpet: 4 note ends, median T20 0.735 s, T40 1.295 s, slopes -28 then -36 dB/s (single stage) rel 0.319 s, no reverb
#ifndef HAE_SEND
#define HAE_SEND 0.5   // measured: sends 0.15 0.5 0.7 0.9 1.5 gave T20 0.160 0.190 0.270 0.285 0.395 s and T40 0.395 0.620 0.705 0.770 0.885 s (target 0.165 and 0.785, both within 25 percent only at 0.5)
#endif
void city_setup(void) {
    g_on_done = sections_out;
    CELLO = lyria_track("cello", LM "lyria_cello.txt", 2, 1.46 / 2.303);
    HAEGEUM = lyria_track("haegeum", LM "lyria_haegeum.txt", 2, 0.165 / 2.303);
    TRUMPET = lyria_track("trumpet", LM "lyria_trumpet.txt", 2, 0.735 / 2.303);
    g_bus[0] = (Bus){.type = 1, .rev = {1.9, 1.05, 0, 0.8, 250, 7500, 0.15, 1.0, 0.25, 0.2}, .gain_db = 0, .duck_src = -1};
    g_tr[HAEGEUM].send[0] = HAE_SEND;
}
double city_loop(void) { g_nsec = 0; score_txt("score.txt", t_map, 0); return score_total_bars() * 4.0; }
void city_full(void) { city_loop(); }
