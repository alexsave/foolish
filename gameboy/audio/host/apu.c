// apu - see apu.h. A plain model, written from Pan Docs' register pages.
#include "apu.h"
#include <string.h>

static const uint8_t duty_wave[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },   // 12.5%
    { 1, 0, 0, 0, 0, 0, 0, 1 },   // 25%
    { 1, 0, 0, 0, 0, 1, 1, 1 },   // 50%
    { 0, 1, 1, 1, 1, 1, 1, 0 },   // 75%
};

typedef struct {
    uint8_t on, dac;
    uint16_t len;          // length counter
    uint8_t len_en;
    uint16_t period;       // 11-bit
    int32_t timer;         // 2 MHz steps to the next step of the channel
    uint8_t pos;           // duty step or wave position
    uint8_t vol, env_init, env_up, env_pace, env_timer;
    // CH1 sweep
    uint8_t sw_pace, sw_down, sw_step, sw_timer, sw_en;
    uint16_t sw_shadow;
    // CH4
    uint16_t lfsr;
    uint8_t narrow;
    uint8_t buf;           // CH3 sample buffer
} chan;

static chan C[4];
static uint8_t reg[0x40];
static uint32_t fs_count;  // steps to the next frame-sequencer clock
static uint8_t fs_step;
static double cap;         // the output capacitor
static double acc;         // the box average of 16 steps
static uint32_t acc_n;
static double fir_hist[32];
static uint32_t fir_i, dec_phase;

static int32_t chan_period_steps(int i) {
    const chan *c = &C[i];
    if (i < 2) return 2 * (2048 - c->period);
    if (i == 2) return 2048 - c->period;
    {
        const uint8_t r = reg[0x22] & 7u, s = (uint8_t)(reg[0x22] >> 4);
        return r ? (int32_t)(8u * r) << s : (int32_t)4 << s;
    }
}

void apu_reset(void) {
    memset(C, 0, sizeof C);
    memset(reg, 0, sizeof reg);
    fs_count = 4096; fs_step = 0;
    cap = 0; acc = 0; acc_n = 0;
    memset(fir_hist, 0, sizeof fir_hist); fir_i = 0; dec_phase = 0;
    reg[0x24] = 0x77; reg[0x25] = 0xFF; reg[0x26] = 0x80;
    for (int i = 0; i < 4; i++) C[i].lfsr = 0x7FFF;
}

static void sweep_calc(chan *c, int write) {
    uint16_t d = (uint16_t)(c->sw_shadow >> c->sw_step);
    int32_t n = c->sw_down ? (int32_t)c->sw_shadow - d : (int32_t)c->sw_shadow + d;
    if (n > 2047) { c->on = 0; return; }
    if (write && c->sw_step) { c->sw_shadow = (uint16_t)n; c->period = (uint16_t)n; }
}

static void trigger(int i) {
    chan *c = &C[i];
    c->on = c->dac;
    if (c->len == 0) c->len = i == 2 ? 256 : 64;
    c->timer = chan_period_steps(i) + (i == 2 ? 3 : 0);
    if (i == 2) c->pos = 0;          // DMG: the buffer keeps its old sample until the first step
    if (i != 2) {
        c->vol = c->env_init;
        c->env_timer = c->env_pace;
    }
    if (i == 3) c->lfsr = 0x7FFF;
    if (i == 0) {
        c->sw_shadow = c->period;
        c->sw_timer = c->sw_pace ? c->sw_pace : 8;
        c->sw_en = c->sw_pace || c->sw_step;
        if (c->sw_step) sweep_calc(c, 0);
    }
}

void apu_write(uint8_t r, uint8_t v) {
    if (r >= 0x30 && r < 0x40) { reg[r] = v; return; }
    if (r < 0x10 || r > 0x26) return;
    reg[r] = v;
    const int i = r < 0x15 ? 0 : r < 0x1A ? 1 : r < 0x1F ? 2 : r < 0x24 ? 3 : 4;
    if (i == 4) {
        if (r == 0x26 && !(v & 0x80)) { apu_reset(); reg[0x26] = 0; }
        return;
    }
    chan *c = &C[i];
    const int k = r - (0x10 + 5 * i);   // 0..4: NRx0..NRx4
    switch (k) {
    case 0:
        if (i == 0) { c->sw_pace = (v >> 4) & 7; c->sw_down = (v >> 3) & 1; c->sw_step = v & 7; }
        if (i == 2) { c->dac = v >> 7; if (!c->dac) c->on = 0; }
        break;
    case 1:
        if (i == 2) c->len = (uint16_t)(256 - v);
        else c->len = (uint16_t)(64 - (v & 63));
        break;
    case 2:
        if (i != 2) {
            c->env_init = v >> 4; c->env_up = (v >> 3) & 1; c->env_pace = v & 7;
            c->dac = (v & 0xF8) != 0;
            if (!c->dac) c->on = 0;
        }
        break;
    case 3:
        if (i == 3) { c->narrow = (v >> 3) & 1; }
        else c->period = (uint16_t)((c->period & 0x700) | v);
        break;
    case 4:
        if (i != 3) c->period = (uint16_t)((c->period & 0xFF) | ((v & 7u) << 8));
        c->len_en = (v >> 6) & 1;
        if (v & 0x80) trigger(i);
        break;
    }
}

uint8_t apu_read(uint8_t r) {
    if (r == 0x26) return (uint8_t)((reg[0x26] & 0x80) | (C[0].on) | (C[1].on << 1) | (C[2].on << 2) | (C[3].on << 3));
    return reg[r];
}

static void frame_seq(void) {
    int i;
    if ((fs_step & 1) == 0) {      // length, 256 Hz
        for (i = 0; i < 4; i++)
            if (C[i].len_en && C[i].len) { if (--C[i].len == 0) C[i].on = 0; }
    }
    if (fs_step == 2 || fs_step == 6) {   // sweep, 128 Hz
        chan *c = &C[0];
        if (c->sw_timer && --c->sw_timer == 0) {
            c->sw_timer = c->sw_pace ? c->sw_pace : 8;
            if (c->sw_en && c->sw_pace) { sweep_calc(c, 1); if (c->on) sweep_calc(c, 0); }
        }
    }
    if (fs_step == 7) {            // envelope, 64 Hz
        for (i = 0; i < 4; i++) {
            chan *c = &C[i];
            if (i == 2 || !c->env_pace) continue;
            if (--c->env_timer == 0) {
                c->env_timer = c->env_pace;
                if (c->env_up && c->vol < 15) c->vol++;
                else if (!c->env_up && c->vol > 0) c->vol--;
            }
        }
    }
    fs_step = (uint8_t)((fs_step + 1) & 7);
}

static uint8_t chan_digital(int i) {
    chan *c = &C[i];
    if (!c->on) return 0;
    switch (i) {
    case 0: case 1: return duty_wave[reg[i ? 0x16 : 0x11] >> 6][c->pos] ? c->vol : 0;
    case 2: {
        const uint8_t sh = (reg[0x1C] >> 5) & 3;
        if (!sh) return 0;
        return (uint8_t)(c->buf >> (sh - 1));
    }
    default: return (c->lfsr & 1) ? 0 : c->vol;
    }
}

// 31-tap low-pass for the 131072 -> 32768 Hz decimation (windowed sinc, cut
// at 15 kHz), computed once.
static double fir[31];
static int fir_ready;
static void fir_init(void) {
    const double fc = 15000.0 / 131072.0;
    double sum = 0;
    for (int n = 0; n < 31; n++) {
        const double m = n - 15;
        const double s = m == 0 ? 2 * fc : __builtin_sin(2 * 3.14159265358979 * fc * m) / (3.14159265358979 * m);
        const double w = 0.42 - 0.5 * __builtin_cos(2 * 3.14159265358979 * n / 30) + 0.08 * __builtin_cos(4 * 3.14159265358979 * n / 30);
        fir[n] = s * w; sum += fir[n];
    }
    for (int n = 0; n < 31; n++) fir[n] /= sum;
    fir_ready = 1;
}

uint32_t apu_run(uint32_t steps, float *buf) {
    uint32_t out = 0;
    if (!fir_ready) fir_init();
    const double charge = 0.999916;   // 0.999958 a 4.19 MHz cycle, squared: one 2 MHz step (Pan Docs, SameBoy)
    double ch16 = 1; for (int q = 0; q < 16; q++) ch16 *= charge;
    while (steps--) {
        int i;
        if (--fs_count == 0) { fs_count = 4096; frame_seq(); }
        for (i = 0; i < 4; i++) {
            chan *c = &C[i];
            if (--c->timer <= 0) {
                c->timer += chan_period_steps(i);
                if (i < 2) c->pos = (uint8_t)((c->pos + 1) & 7);
                else if (i == 2) {
                    c->pos = (uint8_t)((c->pos + 1) & 31);
                    const uint8_t b = reg[0x30 + (c->pos >> 1)];
                    c->buf = (c->pos & 1) ? (b & 15) : (b >> 4);
                } else if ((reg[0x22] >> 4) < 14) {
                    const uint16_t x = (uint16_t)((c->lfsr ^ (c->lfsr >> 1)) & 1);
                    c->lfsr = (uint16_t)((c->lfsr >> 1) | (x << 14));
                    if (c->narrow) c->lfsr = (uint16_t)((c->lfsr & ~(1u << 6)) | (x << 6));
                }
            }
        }
        if (++acc_n < 16) continue;
        // every 16 steps (131072 Hz): mix, high-pass, low-pass and decimate
        double l = 0, r = 0;
        const uint8_t nr51 = reg[0x25], nr50 = reg[0x24];
        for (i = 0; i < 4; i++) {
            const int dac = i == 2 ? C[2].dac : C[i].dac;
            const double a = dac ? 1.0 - chan_digital(i) / 7.5 : 0.0;
            if (nr51 & (0x10 << i)) l += a;
            if (nr51 & (1 << i)) r += a;
        }
        l *= ((nr50 >> 4) & 7) + 1; r *= (nr50 & 7) + 1;
        double x = (l + r) / 2 / 32.0;
        acc_n = 0;
        double y = x - cap;
        cap = x - y * ch16;
        fir_hist[fir_i] = y; fir_i = (fir_i + 1) & 31;
        if (++dec_phase < 4) continue;
        dec_phase = 0;
        double s = 0;
        for (int n = 0; n < 31; n++) s += fir[n] * fir_hist[(fir_i - 1 - n) & 31];
        buf[out++] = (float)s;
    }
    return out;
}
