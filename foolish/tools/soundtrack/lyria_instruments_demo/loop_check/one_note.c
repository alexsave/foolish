// one_note.c: renders ONE held note of a Lyria sampler instrument alone, for the loop checks.
// Settings from the environment: INST (a manifest name, e.g. cello or cello_flat) (cello|haegeum|trumpet), LOOPMODE (sp[10]: 0, 1, 2), SEND (haegeum reverb send, 0 = off),
// REL (release, s), NOTE (e.g. D3), VEL (0..1, picks the layer of a two-layer set), LEN (seconds the note sounds, default 5).
// The score is written here to score.txt: a 4-beat rest, then the note at beat 4, then rests, at 90 BPM, no DYN line.
// Build and run from a work folder: clang -O2 -w -o one one_note.c && SAMPLES=... ENGINE=<engine dir> ./one full out
#include ENGINE_ORCH
static int T;
static int t_map(const char *p, double *tp, double *vs, double *dp) { *tp = 0; *vs = 1; return !strcmp(p, "X") ? T : -1; }
void city_setup(void) {
    g_on_done = sections_out;
    const char *inst = getenv("INST"); char man[200]; snprintf(man, sizeof man, "/Users/alex/Dev/foolish-wt-city-songs-v5/foolish/tools/soundtrack/samples/manifests/lyria_%s.txt", inst);
    T = smp_track(inst, man);
    g_tr[T].sp[0] = atof(getenv("REL")); g_tr[T].sp[10] = atof(getenv("LOOPMODE")); g_tr[T].sp[2] = 25;
    g_tr[T].human_ms = 0; g_tr[T].human_vel = 0;
    g_bus[0] = (Bus){.type = 1, .rev = {1.9, 1.05, 0, 0.8, 250, 7500, 0.15, 1.0, 0.25, 0.2}, .gain_db = 0, .duck_src = -1};
    g_tr[T].send[0] = atof(getenv("SEND"));
    if (getenv("VIBC")) { g_tr[T].sp[13] = atof(getenv("VIBC")); g_tr[T].sp[14] = atof(getenv("VIBHZ")); }   // the engine LFO: depth (cents, peak) and rate
}
double city_loop(void) {
    char sc[400]; double len = getenv("LEN") ? atof(getenv("LEN")) : 5.0;
    FILE *f = fopen("score.txt", "w");
    fprintf(f, "TEMPO 90\n\nSECTION one BARS 6 KEY C\nX: r:4 | %s:4~%.4f@%s | r:4 | r:4 | r:4 | r:4\n", getenv("NOTE"), len / 0.92 * 1.5, getenv("VEL"));
    fclose(f);
    g_nsec = 0; score_txt("score.txt", t_map, 0); return score_total_bars() * 4.0;
}
void city_full(void) { city_loop(); }
