// zone_report.c MANIFEST: for each sample of a manifest, the length after the engine's own leading-silence trim, the attack point,
// and the mode 2 loop zone [A, B] (A = the later of attack + 50 ms and 20% of the length, B = length - 60 ms) with its length.
#define main engine_main
#include ENGINE_ORCH
#undef main
void city_setup(void) {}
double city_loop(void) { return 0; }
void city_full(void) {}
int main(int argc, char **argv) {
    SampleSet *ss = ss_load(argv[1]);
    for (int i = 0; i < ss->n; i++) {
        Smp *x = &ss->s[i]; g_wav_quiet = 1; x->n = wav_read(x->path, &x->L, &x->R); x->sr = g_wav_sr; smp_trim(x);
        double sr = x->sr, A = fmax(x->att * sr + 0.05 * sr, 0.2 * x->n), B = x->n - 0.06 * sr;
        const char *b = strrchr(x->path, '/'); printf("%s\t%.0f\t%.0f\t%.0f\n", b ? b + 1 : x->path, x->n / sr * 1000, x->att * 1000, (B - A) / sr * 1000);
    }
    return 0;
}
