// ios_api_sound.c - the sound half of the bridge: what a cue plays (cue_voice.h,
// the resident voice over soundgen's generated table, cue_voice_table.c) and how
// loud a dealt card lands at a seat. One line each: the web exports the same C
// as wasm_cue_* (c/wasm/wasm_api.c).

#include "ios_api.h"
#include "cue_voice.h"
#include "cue_mix.h"

#include <stdlib.h>

int fio_seat_gain_pm(int viewer, int seat, int n_seats) {
    return anim_seat_gain_pm(viewer, seat, n_seats);
}

int fio_cue_n_takes(void) { return cue_table_n_takes(); }
const void *fio_cue_take(int i) { return cue_table_take(i); }
int fio_cue_lead_us(int kind) { return cue_table_lead_us(kind); }
void fio_cue_seed(uint32_t seed) { cue_table_seed(seed); }
const void *fio_cue_strike(int kind, int lead_us, int n_cards, int undoes, int gain_pm) {
    return cue_table_strike(kind, lead_us, n_cards, undoes, gain_pm);
}
int fio_cue_resampled_len(int n, int rate_pm) { return cue_resampled_len(n, rate_pm); }

int fio_cue_resample(const float *in, int n, int rate_pm, float *out, int cap) {
    return cue_resample(in, n, rate_pm, out, cap);
}

// The output mix (cue_mix.h): one per running engine, made and freed by the host.
void *fio_mix_new(double rate) {
    CueMixer *m = malloc(sizeof *m);
    cue_mix_init(m, rate);
    return m;
}
void fio_mix_free(void *m) { free(m); }
int fio_mix_add(void *m, const float *x, int n, int64_t start, float gain) { return cue_mix_add(m, x, n, start, gain); }
uint32_t fio_mix_live(const void *m) { return cue_mix_live(m); }
void fio_mix_clear(void *m) { cue_mix_clear(m); }
void fio_mix_drop(void *m, int slot) { cue_mix_drop(m, slot); }
void fio_mix_render(void *m, float *out, int frames, uint64_t host) { cue_mix_render(m, out, frames, host); }
int64_t fio_mix_now(const void *m) { return cue_mix_now(m); }
int64_t fio_mix_frame_at(const void *m, uint64_t at, double ticks_per_second) {
    return cue_mix_frame_at(m, at, ticks_per_second);
}
