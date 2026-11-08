// cue_mix.c - see cue_mix.h.

#include "cue_mix.h"

#include <string.h>

void cue_mix_init(CueMixer *m, double rate) {
    if (!m) return;
    memset(m->slot, 0, sizeof m->slot);
    atomic_store(&m->live, 0);
    atomic_store(&m->clear, 0);
    atomic_store(&m->frames, 0);
    atomic_store(&m->host, 0);
    atomic_store(&m->stamped, 0);
    m->rate = rate;
}

int cue_mix_add(CueMixer *m, const float *x, int n, int64_t start, float gain) {
    if (!m || !x || n <= 0) return -1;
    const uint32_t live = atomic_load_explicit(&m->live, memory_order_acquire);
    for (int k = 0; k < CUE_MIX_SLOTS; k++) {
        if (live & (1u << k)) continue;
        m->slot[k] = (CueMixSlot){ .x = x, .n = n, .start = start, .gain = gain };
        atomic_fetch_or_explicit(&m->live, 1u << k, memory_order_release);
        return k;
    }
    return -1;
}

uint32_t cue_mix_live(const CueMixer *m) {
    return m ? atomic_load_explicit(&((CueMixer *)m)->live, memory_order_acquire) : 0;
}

void cue_mix_clear(CueMixer *m) {
    if (m) atomic_fetch_or_explicit(&m->clear, cue_mix_live(m), memory_order_release);
}

void cue_mix_drop(CueMixer *m, int slot) {
    if (m && slot >= 0 && slot < CUE_MIX_SLOTS)
        atomic_fetch_or_explicit(&m->clear, cue_mix_live(m) & (1u << slot), memory_order_release);
}

void cue_mix_render(CueMixer *m, float *out, int frames, uint64_t host) {
    if (!m || !out || frames <= 0) return;
    memset(out, 0, sizeof(float) * (size_t)frames);
    const int64_t t0 = atomic_load_explicit(&m->frames, memory_order_relaxed);
    const uint32_t drop = atomic_exchange_explicit(&m->clear, 0, memory_order_acq_rel);
    const uint32_t live = atomic_load_explicit(&m->live, memory_order_acquire);
    uint32_t done = drop & live;
    for (int k = 0; k < CUE_MIX_SLOTS; k++) {
        if (!(live & (1u << k)) || (done & (1u << k))) continue;
        CueMixSlot *s = &m->slot[k];
        // A take whose start was already rendered past starts in this block, whole.
        if (s->start < t0) s->start = t0;
        const int64_t from = s->start - t0;              // its first frame in this block
        if (from >= frames) continue;
        int64_t i = 0;
        for (int64_t f = from; f < frames && i < s->n; f++, i++) out[f] += s->x[i] * s->gain;
        // Played out: the rest of it (if any) shifts to the next block's start.
        if (i >= s->n) done |= 1u << k;
        else { s->x += i; s->n -= (int32_t)i; s->start = t0 + frames; }
    }
    if (done) atomic_fetch_and_explicit(&m->live, ~done, memory_order_release);
    atomic_store_explicit(&m->host, host, memory_order_relaxed);
    atomic_store_explicit(&m->stamped, t0, memory_order_relaxed);
    atomic_store_explicit(&m->frames, t0 + frames, memory_order_release);
}

int64_t cue_mix_now(const CueMixer *m) {
    return m ? atomic_load_explicit(&((CueMixer *)m)->frames, memory_order_acquire) : 0;
}

int64_t cue_mix_frame_at(const CueMixer *m, uint64_t at, double ticks_per_second) {
    if (!m) return 0;
    CueMixer *mm = (CueMixer *)m;
    // Read the stamp and the count of one block: a block landing between the
    // two reads is seen by the count moving, and read again.
    for (int tries = 0; tries < 4; tries++) {
        const int64_t f0 = atomic_load_explicit(&mm->frames, memory_order_acquire);
        const uint64_t h = atomic_load_explicit(&mm->host, memory_order_relaxed);
        const int64_t at_frame = atomic_load_explicit(&mm->stamped, memory_order_relaxed);
        const int64_t f1 = atomic_load_explicit(&mm->frames, memory_order_acquire);
        if (f0 != f1) continue;
        if (h == 0 || ticks_per_second <= 0) return f1;
        // The stamp is the host time the last block's first frame is heard at;
        // blocks are rendered back to back at the output's rate, so the frame
        // heard at `at` is that frame plus the time since (before, if `at` is
        // earlier than the stamp, which a stamp ahead of now makes usual).
        const double dt = ((double)at - (double)h) / ticks_per_second;
        return at_frame + (int64_t)(dt * m->rate + (dt < 0 ? -0.5 : 0.5));
    }
    return cue_mix_now(m);
}
