// snd - see snd.h and DESIGN.md "The driver".
#include "snd.h"

// The working state: 69 bytes, every one counted (DESIGN.md "Memory"; the
// build added q_at, 16 bytes, for snd_strike_at).
// On the Game Boy it is a member of the union over the search bots' scratch
// (gd_botlib.c's candidate list), because no cue sounds while a bot thinks:
// snd_yield() drops it before a think and snd_claim() starts it clean.
// Arrays, not an array of voice structs: SDCC reaches a byte of a fixed array
// by its index in a few instructions, a field of the i-th 7-byte struct only
// through a multiply and a pointer kept on the stack (measured: the struct
// version took up to 5,900 M-cycles a tick, README "Sound").
typedef struct {
    const snd_row *row[4];          // 8: the row playing, or 0: idle
    const snd_row *first[4];        // 8: the row a backwards voice ends on
    uint8_t left[4];                // 4: ticks left on the row
    uint8_t prio[4];                // 4
    uint8_t vflags[4];              // 4: SND_BACKWARDS, SND_FAR, as struck
    uint8_t q_cue[SND_QUEUE];       // 8
    uint8_t q_wait[SND_QUEUE];      // 8: ticks to go; 0 is a free slot
    uint8_t q_flags[SND_QUEUE];     // 8
    uint16_t q_at[SND_QUEUE];       // 16: SND_HELD: not before the frame clock reads this
    uint8_t wave;                   // 1: the wave table in CH3's RAM, 0xFF none
} snd_state;                        // 69

// SND_OVERLAY names a header that puts the state over other memory: it
// defines SND_RAM (the bytes) and SND_RAM_BYTES (how many). The Game Boy's is
// src/gd_botlib.h, the bots' candidate list; host/snd_test.c's is its own, so
// it can scribble on the state as a think would.
#ifdef SND_OVERLAY
#include SND_OVERLAY
#define S (*(snd_state *)(void *)(SND_RAM))
typedef char snd_state_fits[sizeof(snd_state) <= SND_RAM_BYTES ? 1 : -1];
#else
static snd_state S;
#endif

// The tables in a ROM bank of their own (SND_BANK, the Game Boy's Makefile):
// the tick maps it in and puts back the bank it found. Code that switches
// banks with interrupts on writes _current_bank before the bank register
// (GBDK's SWITCH_ROM and its banked-call trampoline), so what the tick puts
// back is always the bank the interrupted code wants.
#ifdef __SDCC
#include <gb/gb.h>          // sys_time, the frame clock
#endif
// a queued strike held for the frame clock (snd_strike_at), in q_flags
#define SND_HELD 0x80u

#ifdef SND_BANK
#define BANK_IN  const uint8_t save_bank = _current_bank; SWITCH_ROM(SND_BANK)
#define BANK_OUT SWITCH_ROM(save_bank)
#else
#define BANK_IN
#define BANK_OUT
#endif

#if defined(SND_HOST) && defined(SND_OVERLAY)
#include <stddef.h>
// for host/snd_test.c, which fills the queue as a bot's bytes might
const unsigned snd_host_queue_at = offsetof(snd_state, q_cue), snd_host_queue_n = SND_QUEUE;
#endif

#ifndef __SDCC
volatile uint8_t snd_owner, snd_refused, snd_cut, snd_starts, snd_active, snd_strikes;
#endif
#ifdef SND_HOST
volatile uint16_t snd_host_clock;
#endif

// Quiet without turning a DAC off: a DAC switched off or on mid-play is a
// pop on the hardware, so the DACs go on once (snd_init) and stay on. A
// pulse or noise channel is retriggered at volume 0 (NRx2 0x08: volume 0,
// envelope up, no pace, DAC still on); the wave channel's level goes to mute.
// Each channel's registers by constant address (an ldh each on the SM83).
static void silence(uint8_t ch) {
    switch (ch) {
    case 0: SND_WR(0x12, 0x08); SND_WR(0x14, 0x80); break;
    case 1: SND_WR(0x17, 0x08); SND_WR(0x19, 0x80); break;
    case 2: SND_WR(0x1C, 0x00); break;
    default: SND_WR(0x21, 0x08); SND_WR(0x23, 0x80); break;
    }
}

static void write_row(uint8_t ch, const snd_row *r, uint8_t flags) {
    uint8_t r2 = r->r2;
    const uint8_t r3 = r->r3, r4 = r->r4;
    if (r2 == 0) { silence(ch); return; }   // a rest
    if (ch != 2) {
        // backwards, an envelope runs the other way
        if ((flags & SND_BACKWARDS) && (r2 & 7u)) r2 ^= 0x08u;
        // a far seat's card lands 6 dB under one's own (AUDIO.md: 6.8 dB across the table)
        if (flags & SND_FAR) r2 = (uint8_t)((uint8_t)((uint8_t)((r2 >> 4) + 1u) >> 1) << 4 | (r2 & 15u));
    }
    // a row with the trigger bit writes all five; any other, the pitch only
    switch (ch) {
    case 0:
        if (r4 & 0x80u) { SND_WR(0x10, r->r0); SND_WR(0x11, r->r1); SND_WR(0x12, r2); }
        SND_WR(0x13, r3); SND_WR(0x14, r4);
        break;
    case 1:
        if (r4 & 0x80u) { SND_WR(0x16, r->r1); SND_WR(0x17, r2); }
        SND_WR(0x18, r3); SND_WR(0x19, r4);
        break;
    case 2:
        if (r4 & 0x80u) {
            const uint8_t w = r->r0;
            if (w != 0xFFu && w != S.wave) {
                // the wave RAM is written with the DAC off (Pan Docs: wave RAM is only
                // reliably accessible while CH3 is off on the DMG)
                const uint8_t *src = snd_waves[w];
                uint8_t i;
                SND_WR(0x1A, 0x00);
                for (i = 0; i < 16; i++) SND_WR(0x30u + i, src[i]);
                S.wave = w;
            }
            SND_WR(0x1A, 0x80);
            SND_WR(0x1B, r->r1); SND_WR(0x1C, r2);
        }
        SND_WR(0x1D, r3); SND_WR(0x1E, r4);
        break;
    default:
        if (r4 & 0x80u) { SND_WR(0x20, r->r1); SND_WR(0x21, r2); }
        SND_WR(0x22, r3); SND_WR(0x23, r4);
        break;
    }
}

// The round robin counts 0 to 5 and wraps: a cue's take is the count modulo
// its take count (1 or 3), as a count over all strikes was, with no division.
#define SND_RR 6u

// 1 if a voice started (a cue of higher priority may hold every channel it needs).
static uint8_t start(uint8_t cue, uint8_t flags) {
    const snd_cue *c = &snd_cues[cue];
    const snd_take *t;
    const uint8_t n = c->n_takes, prio = c->prio;
    uint8_t ch, k, any = 0;
    if (!n) return 0;
    k = snd_strikes;
    while (k >= n) k -= n;
    t = &c->takes[k];
    if (++snd_strikes >= SND_RR) snd_strikes = 0;
    snd_starts++;
    for (ch = 0; ch < 4; ch++) {
        const snd_row *r = t->v[ch];
        if (!r) continue;
        // a channel is taken from a cue of no higher priority
        if (S.row[ch] && S.prio[ch] > prio) continue;
        S.prio[ch] = prio;
        if (flags & SND_BACKWARDS) {
            S.first[ch] = r;
            while (r[1].t) r++;
        }
        S.vflags[ch] = flags;
        S.row[ch] = r;
        S.left[ch] = r->t;
        write_row(ch, r, flags);
        any = 1;
    }
    return any;
}

void snd_init(void) {
    uint8_t i;
    SND_WR(0x26, 0x80);      // APU on
    SND_WR(0x24, 0x77);      // master volume full both sides
    SND_WR(0x25, 0xFF);      // every channel to both sides (DESIGN.md "Panning")
    SND_WR(0x1A, 0x80);      // the wave DAC on, for the session
    for (i = 0; i < 4; i++) { S.row[i] = 0; silence(i); }
    for (i = 0; i < SND_QUEUE; i++) S.q_wait[i] = 0;
    S.wave = 0xFF;
    snd_owner = SND_OWNER_SOUND;
    snd_refused = 0; snd_cut = 0; snd_starts = 0; snd_strikes = 0;
    snd_active = 0;
}

static void queue(uint8_t cue, uint16_t at, uint8_t delay, uint8_t n, uint8_t flags) {
    uint8_t k, i;
    if (snd_owner != SND_OWNER_SOUND) { snd_refused++; return; }
    if (n > SND_MAX_HITS) n = SND_MAX_HITS;
    if (n == 0) n = 1;
    for (k = 0; k < n; k++) {
        // every strike goes through the queue, so it starts on a tick (at most
        // one tick, 4 ms, after the call: it trails, never leads)
        const uint8_t d = (uint8_t)(delay + 1u + k * SND_STAGGER);
        for (i = 0; i < SND_QUEUE; i++) if (!S.q_wait[i]) break;
        if (i == SND_QUEUE) break;           // full: the later strikes of a beat are dropped
        S.q_cue[i] = cue;
        S.q_flags[i] = flags;
        S.q_at[i] = at;
        S.q_wait[i] = d;
    }
    // after the queue holds the strike: set before it, a tick between the two
    // could find nothing due, clear the flag, and never look at the strike
    // (it would stay queued, and snd_busy() true, for ever)
    snd_active = 1;
}

void snd_strike(uint8_t cue, uint8_t delay, uint8_t n, uint8_t flags) {
    queue(cue, 0, delay, n, (uint8_t)(flags & (uint8_t)~SND_HELD));
}

void snd_strike_at(uint8_t cue, uint16_t at, uint8_t delay, uint8_t n, uint8_t flags) {
    queue(cue, at, delay, n, (uint8_t)(flags | SND_HELD));
}

// One voice's tick, for a constant channel: with the index a constant, SDCC
// reaches each array cell by its address, where a loop over the channels
// kept every pointer and index on the stack (2,000 M-cycles a tick with
// nothing to write, measured; README "Sound").
#define STEP_VOICE(ch) do { \
    const snd_row *r = S.row[ch]; \
    if (r) { \
        if (--S.left[ch]) live = 1; \
        else { \
            if (!(S.vflags[ch] & SND_BACKWARDS)) { r++; if (!r->t) r = 0; } \
            else r = r == S.first[ch] ? 0 : r - 1; \
            S.row[ch] = r; \
            if (!r) silence(ch); \
            else { S.left[ch] = r->t; live = 1; write_row(ch, r, S.vflags[ch]); } \
        } \
    } \
} while (0)

void snd_tick(void) {
    uint8_t live = 0;
    // nothing sounding, or a bot thinking (snd_yield cleared this, and
    // snd_strike will not set it again until snd_claim): one load and a branch
    if (!snd_active) return;
    {
    BANK_IN;
    STEP_VOICE(0);
    STEP_VOICE(1);
    STEP_VOICE(2);
    STEP_VOICE(3);
    // the strikes coming due start after the voices stepped, so a new row
    // holds its whole t ticks
    {
        uint8_t i;
        uint8_t *w = S.q_wait;
        for (i = 0; i < SND_QUEUE; i++, w++) {
            if (!*w) continue;
            if (S.q_flags[i] & SND_HELD) {
                // held until the frame clock reaches q_at (16 bits, across its wrap)
                if ((int16_t)(SND_CLOCK - S.q_at[i]) < 0) { live = 1; continue; }
                S.q_flags[i] &= (uint8_t)~SND_HELD;
            }
            if (--*w) { live = 1; continue; }
            if (start(S.q_cue[i], S.q_flags[i])) live = 1;
        }
    }
    // what snd_busy() would say, counted on the way
    if (!live) snd_active = 0;
    BANK_OUT;
    }
}

uint8_t snd_busy(void) {
    uint8_t i;
    for (i = 0; i < 4; i++) if (S.row[i]) return 1;
    for (i = 0; i < SND_QUEUE; i++) if (S.q_wait[i]) return 1;
    return 0;
}

void snd_yield(void) {
    uint8_t i;
    snd_owner = SND_OWNER_BOT;
    // the tick stops reading the state now, before the bot writes over it
    // (left to the next tick, a tick in between would walk the bot's bytes)
    snd_active = 0;
    if (snd_busy()) snd_cut++;
    for (i = 0; i < 4; i++) { S.row[i] = 0; silence(i); }
    for (i = 0; i < SND_QUEUE; i++) S.q_wait[i] = 0;
}

void snd_claim(void) {
    uint8_t i;
    // the scratch was the bot's: nothing in it is ours any more
    for (i = 0; i < 4; i++) S.row[i] = 0;
    for (i = 0; i < SND_QUEUE; i++) S.q_wait[i] = 0;
    S.wave = 0xFF;
    snd_owner = SND_OWNER_SOUND;
}
