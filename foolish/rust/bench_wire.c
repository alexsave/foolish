// C-side benchmark: the REAL kernel's msg_decode + msg_encode (c/src/msg_wire.c,
// the iMessage FMSG envelope codec — the hostile-bytes parsing surface) over
// the dumped envelope corpus (valid + corrupted).
#include "../c/src/game.h"
#include "../c/src/msg_wire.h"
#include "bench_common.h"

typedef struct { const unsigned char *p; int len; } EnvRef;

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "envelopes.bin";
    int reps = argc > 2 ? atoi(argv[2]) : 300;

    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    uint32_t magic, count;
    memcpy(&magic, buf, 4);
    memcpy(&count, buf + 4, 4);
    if (magic != 0x564E4546u) { fprintf(stderr, "bad envelope file\n"); return 1; }
    EnvRef *envs = (EnvRef *)malloc(count * sizeof(EnvRef));
    const unsigned char *p = buf + 8;
    long total_bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        int len = p[0] | (p[1] << 8);
        p += 2;
        envs[i].p = p;
        envs[i].len = len;
        p += len;
        total_bytes += len;
    }

    static MsgEnvelope e;
    static unsigned char out[8192];
    uint64_t sum = FNV_INIT;
    double best = 1e30, t_total = 0;
    for (int r = 0; r < reps; r++) {
        uint64_t rep_sum = FNV_INIT;
        double t0 = now_s();
        for (uint32_t i = 0; i < count; i++) {
            int rc = msg_decode(envs[i].p, envs[i].len, &e);
            rep_sum = fnv1a_u32(rep_sum, (uint32_t)(int32_t)rc);
            if (rc == MSG_EOK) {
                int len = msg_encode(&e, out, (int)sizeof(out));
                rep_sum = fnv1a_u32(rep_sum, (uint32_t)(int32_t)len);
                if (len > 0) rep_sum = fnv1a(rep_sum, out, (size_t)len);
            }
        }
        double dt = now_s() - t0;
        t_total += dt;
        if (dt < best) best = dt;
        sum = rep_sum;
    }

    printf("bench=wire impl=c envelopes=%u reps=%d corpus_bytes=%ld checksum=%016llx\n",
           count, reps, total_bytes, (unsigned long long)sum);
    printf("bench=wire impl=c best_ms=%.3f mean_ms=%.3f ns_per_env=%.1f mb_per_s=%.1f peak_rss_kb=%ld sizeof_MsgEnvelope=%zu\n",
           best * 1e3, t_total / reps * 1e3,
           best * 1e9 / count, (double)total_bytes / best / 1e6,
           peak_rss_kb(), sizeof(MsgEnvelope));
    free(envs); free(buf);
    return 0;
}
