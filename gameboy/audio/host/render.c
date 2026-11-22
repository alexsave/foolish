// render - preview the Game Boy's cues on the Mac: the real driver (snd.c)
// and tables (snd_tab.c) on the APU model (apu.c). Not shipped; mGBA's
// capture is the judge in the build phase.
//
//   render a OUTDIR            approach A: every take of every cue, alone
//   render b PCMDIR OUTDIR     approach B: each .pcm through CH3 reloads
//   render context OUT.wav     approach A in context: a short game's cues at the frames the game strikes them
//
// WAVs are 16-bit mono at 32768 Hz. No randomness anywhere.
#ifndef SND_HOST
#define SND_HOST 1
#endif
#include "snd.h"
#include "apu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define STEPS_PER_TICK (2097152u * (256u - SND_TMA) / 4096u)   // 8704 at 241 Hz, 34816 at 60 Hz
#define MAXS (APU_OUT_HZ * 40u)

static float *out;
static uint32_t n_out;

void snd_host_write(uint8_t reg, uint8_t val) { apu_write(reg, val); }

static void run(uint32_t steps) {
    static float tmp[1 << 16];
    while (steps) {
        uint32_t k = steps > 60000 ? 60000 : steps;
        uint32_t n = apu_run(k, tmp);
        if (n_out + n < MAXS) { memcpy(out + n_out, tmp, n * sizeof(float)); n_out += n; }
        steps -= k;
    }
}

static void write_wav(const char *path, const float *x, uint32_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    uint32_t rate = APU_OUT_HZ, bytes = n * 2;
    uint8_t h[45] = "RIFF____WAVEfmt \x10\0\0\0\1\0\1\0________\2\0\x10\0data____";
    uint32_t v;
    v = 36 + bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 24, &rate, 4); v = rate * 2; memcpy(h + 28, &v, 4);
    memcpy(h + 40, &bytes, 4);
    fwrite(h, 1, 44, f);
    for (uint32_t i = 0; i < n; i++) {
        float s = x[i] * 32767.0f;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        int16_t q = (int16_t)(s < 0 ? s - 0.5f : s + 0.5f);
        fwrite(&q, 2, 1, f);
    }
    fclose(f);
}

static const char *cue_name(int c) {
    static const char *n[SND_N_CUES] = { "none", "deal", "flip", "lift", "place", "cover", "pickup", "sweep", "pile",
        "out", "revert", "good", "hand_off", "finish", "fool", "deal_land", "turn", "pass", "into_hand", "burst",
        "finish_miami", "finish_new_york", "finish_seoul", "finish_moscow" };
    return n[c];
}

// The APU settles for 50 ms (the DACs on, the capacitor charged) before a
// render, so the start-up pop is not in the preview.
static void begin(void) {
    apu_reset();
    snd_init();
    n_out = 0;
    run(2097152u / 20u);
    n_out = 0;
}

static void render_a(const char *dir) {
    char path[512];
    for (int c = 1; c < SND_N_CUES; c++) {
        const snd_cue *cue = &snd_cues[c];
        for (int k = 0; k < cue->n_takes; k++) {
            begin();
            // the round robin picks take (strikes % n): strike the others away first, silently
            for (int j = 0; j < k; j++) { snd_strike((uint8_t)c, 0, 1, 0); snd_yield(); snd_claim(); }
            snd_strike((uint8_t)c, 0, 1, 0);
            uint32_t tail = 0;
            for (uint32_t t = 0; t < SND_TICK_HZ * 12u; t++) {
                snd_tick();
                run(STEPS_PER_TICK);
                if (!snd_busy() && ++tail > SND_TICK_HZ / 10u) break;
            }
            snprintf(path, sizeof path, "%s/%s_%d.wav", dir, cue_name(c), k);
            write_wav(path, out, n_out);
        }
    }
}

// ---- approach B: 4-bit PCM through the wave channel ------------------------------------
// The reload interrupt runs at 256 Hz (the timer at 4096 Hz, TMA 240): each
// one turns CH3's DAC off, writes the next 32 samples, turns it on and
// retriggers at 8192 Hz (period 1792). An interrupt cannot start while
// interrupts are off, so the reload is late whenever it falls in one of the
// game's busy windows, modelled from the ROM as it is (DESIGN.md "Busy
// windows"): every frame GBDK's VBlank handler (about 250 M-cycles), and on a
// frame that draws sprites the staging copy (1120 M-cycles: the unrolled
// memcpy at 0x2E1D, 7 M-cycles a byte, 160 bytes).
#define FRAME_STEPS  35112u      // 70224 dots
#define VBL_STEPS    500u
#define OAM_AT       6000u
#define OAM_STEPS    2240u
#define RELOAD_STEPS 8192u       // 256 Hz
#define COPY_STEPS   200u        // the DAC off while 16 bytes go in (about 100 M-cycles)

static uint32_t busy_until(uint32_t t, int sprite_frames) {
    const uint32_t f = t / FRAME_STEPS, o = t % FRAME_STEPS;
    if (o < VBL_STEPS) return f * FRAME_STEPS + VBL_STEPS;
    if ((int)f < sprite_frames && o >= OAM_AT && o < OAM_AT + OAM_STEPS) return f * FRAME_STEPS + OAM_AT + OAM_STEPS;
    return t;
}

static int render_b_one(const char *in, const char *wav, int sprite_frames) {
    FILE *f = fopen(in, "rb");
    if (!f) return 0;
    uint8_t h[3];
    if (fread(h, 1, 3, f) != 3) { fclose(f); return 0; }
    const uint32_t n = (uint32_t)h[0] | ((uint32_t)h[1] << 8);
    uint8_t *nib = malloc(n / 2 + 32);
    memset(nib, 0x77, n / 2 + 32);
    if (fread(nib, 1, n / 2, f) != n / 2) { fclose(f); free(nib); return 0; }
    fclose(f);
    begin();
    apu_write(0x1C, (uint8_t)(h[2] << 5));
    uint32_t t = 0, chunk = 0, due = 0;
    const uint32_t chunks = (n + 31) / 32;
    while (chunk <= chunks) {
        const uint32_t at = busy_until(due, sprite_frames);
        run(at - t); t = at;
        apu_write(0x1A, 0x00);
        if (chunk < chunks) {
            // DMG: the first sample after a trigger is the old buffer's, then 1..31, then 0:
            // lay the chunk so nibble i+1 holds sample i, nibble 0 the 32nd
            uint8_t s[32];
            for (int i = 0; i < 32; i++) {
                const uint32_t j = chunk * 32 + i;
                const uint8_t b = nib[j / 2];
                s[(i + 1) & 31] = j < n ? ((j & 1) ? (b & 15) : (b >> 4)) : 7;
            }
            for (int i = 0; i < 16; i++) apu_write((uint8_t)(0x30 + i), (uint8_t)(s[2 * i] << 4 | s[2 * i + 1]));
        }
        run(COPY_STEPS); t += COPY_STEPS;
        if (chunk == chunks) break;
        apu_write(0x1A, 0x80);
        apu_write(0x1C, (uint8_t)(h[2] << 5));
        apu_write(0x1D, 0x00);
        apu_write(0x1E, 0x87);   // period 1792: 8192 Hz, trigger
        chunk++;
        due += RELOAD_STEPS;
    }
    run(2097152u / 10u);
    write_wav(wav, out, n_out);
    free(nib);
    return 1;
}

static void render_b(const char *pcmdir, const char *dir) {
    DIR *d = opendir(pcmdir);
    struct dirent *e;
    char in[512], wav[512];
    if (!d) { perror(pcmdir); exit(1); }
    while ((e = readdir(d))) {
        const size_t L = strlen(e->d_name);
        if (L < 5 || strcmp(e->d_name + L - 4, ".pcm")) continue;
        // a take-off cue plays under a flight (12 frames of sprites); a landing over one frame's redraw
        const int takeoff = !strncmp(e->d_name, "lift", 4) || !strncmp(e->d_name, "turn", 4) || !strncmp(e->d_name, "pickup", 6)
            || (!strncmp(e->d_name, "deal_", 5) && strncmp(e->d_name, "deal_land", 9));
        snprintf(in, sizeof in, "%s/%s", pcmdir, e->d_name);
        snprintf(wav, sizeof wav, "%s/%.*s.wav", dir, (int)(L - 4), e->d_name);
        render_b_one(in, wav, takeoff ? 12 : 1);
    }
    closedir(d);
}

// ---- in context -------------------------------------------------------------------------
// A short game, struck the way main.c will strike it (DESIGN.md "When each
// cue is struck"): times in frames, 60 a second near enough.
#define FLY 12                    // FLY_FRAMES
#define FR(f) ((uint32_t)(f) * SND_TICK_HZ / 60u)
typedef struct { uint16_t frame; uint8_t cue, n, flags, landing; } ev;
static const ev context[] = {
    // the deal: one card in flight at a time, 8 frames a card, alternating; the bot's land far
    { 0, SND_DEAL, 1, 0, 0 },   { 8, SND_DEAL_LAND, 1, SND_FAR, 1 },
    { 8, SND_DEAL, 1, 0, 0 },   { 16, SND_DEAL_LAND, 1, 0, 1 },
    { 16, SND_DEAL, 1, 0, 0 },  { 24, SND_DEAL_LAND, 1, SND_FAR, 1 },
    { 24, SND_DEAL, 1, 0, 0 },  { 32, SND_DEAL_LAND, 1, 0, 1 },
    { 32, SND_TURN, 1, 0, 0 },  { 44, SND_FLIP, 1, 0, 1 },
    // I attack with a pair: one lift, two landings a stagger apart
    { 110, SND_LIFT, 1, 0, 0 }, { 110 + FLY, SND_PLACE, 2, 0, 1 },
    // the bot covers both, 30 frames after the board last changed
    { 152, SND_LIFT, 1, 0, 0 }, { 152 + FLY, SND_COVER, 2, 0, 1 },
    // I say good: the bout ends (no flight: the table clears)
    { 220, SND_GOOD, 1, 0, 0 }, { 236, SND_SWEEP, 1, 0, 0 }, { 236 + 26, SND_PILE, 1, 0, 0 },
    // the bot attacks, I pick up
    { 320, SND_LIFT, 1, 0, 0 }, { 320 + FLY, SND_PLACE, 1, 0, 1 },
    { 380, SND_PICKUP, 1, 0, 0 }, { 380 + FLY, SND_INTO_HAND, 1, 0, 1 },
    // the bot attacks, I transfer
    { 440, SND_LIFT, 1, 0, 0 }, { 440 + FLY, SND_PLACE, 1, 0, 1 },
    { 490, SND_LIFT, 1, 0, 0 }, { 490 + FLY, SND_PLACE, 1, 0, 1 }, { 490 + FLY, SND_PASS, 1, 0, 0 },
    // my last card goes out
    { 560, SND_LIFT, 1, 0, 0 }, { 560, SND_OUT, 1, 0, 0 }, { 560 + FLY, SND_PLACE, 1, 0, 1 },
    // END_DELAY (90 frames) later, the win picture: the city's motif, then the fireworks
    { 650, SND_FINISH_MOSCOW, 1, 0, 0 },
    { 790, SND_BURST, 1, 0, 0 }, { 798, SND_BURST, 1, 0, 0 }, { 822, SND_BURST, 1, 0, 0 },
    // and the other end: the fool
    { 900, SND_FOOL, 1, 0, 0 },
};

static void render_context(const char *wav) {
    const uint32_t n_ev = sizeof context / sizeof context[0];
    uint32_t i = 0;
    begin();
    for (uint32_t t = 0; t < FR(1080); t++) {
        // a cue is struck at its frame; a landing `snd_pre` early, so its moment is the landing frame,
        // and a transfer's slide 249 ms after the landing (the take's own late start)
        while (i < n_ev) {
            const ev *e = &context[i];
            uint32_t at = FR(e->frame);
            uint8_t delay = 0;
            if (e->landing) at -= snd_pre[e->cue];
            if (e->cue == SND_PASS) delay = (uint8_t)(249u * SND_TICK_HZ / 1000u);
            if (at > t) break;
            snd_strike(e->cue, delay, e->n, e->flags);
            i++;
        }
        snd_tick();
        run(STEPS_PER_TICK);
    }
    write_wav(wav, out, n_out);
}

int main(int argc, char **argv) {
    out = malloc(MAXS * sizeof(float));
    if (argc == 3 && !strcmp(argv[1], "a")) render_a(argv[2]);
    else if (argc == 4 && !strcmp(argv[1], "b")) render_b(argv[2], argv[3]);
    else if (argc == 3 && !strcmp(argv[1], "context")) render_context(argv[2]);
    else { fprintf(stderr, "render a OUTDIR | render b PCMDIR OUTDIR | render context OUT.wav\n"); return 2; }
    return 0;
}
