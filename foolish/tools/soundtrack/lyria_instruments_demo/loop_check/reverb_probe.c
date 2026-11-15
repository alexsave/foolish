// reverb_probe.c: feeds one 5 ms click through the demo's haegeum reverb bus settings and writes 4 s of the left wet output
// as raw float32 (48 kHz) to argv[1]. Build: clang -O2 -w -o rp reverb_probe.c   (include path via -DMIXH)
#include MIXH
int main(int argc, char **argv) {
    RevCfg c = {1.9, 1.05, 0, 0.8, 250, 7500, 0.15, 1.0, 0.25, 0.2};
    Rev r; rev_init(&r, c);
    long N = 4 * SR; float *o = malloc(N * sizeof(float));
    for (long i = 0; i < N; i++) { double in = i < (long)(0.005 * SR) ? 0.5 : 0, l, rr; rev_tick(&r, in, in, &l, &rr); o[i] = (float)l; }
    FILE *f = fopen(argv[1], "wb"); fwrite(o, 4, N, f); fclose(f); return 0;
}
