// snd - see snd.h and DESIGN.md "The driver".
#include "snd.h"

// The working state: 53 bytes, every one counted (DESIGN.md "Memory").
// On the Game Boy it is a member of the union over the search bots' scratch
// (gd_botlib.c's candidate list), because no cue sounds while a bot thinks:
// snd_yield() drops it before a think and snd_claim() starts it clean.
typedef struct {
    const snd_row *row;   // the row playing, or 0: idle
    uint8_t left;         // ticks left on it
    uint8_t prio;
    uint8_t flags;        // SND_BACKWARDS, SND_FAR, as struck
    const snd_row *first; // the row a backwards voice ends on
} snd_voice;              // 2 + 1 + 1 + 1 + 2 = 7 bytes on the SM83 (16-bit pointers)

typedef struct {
    snd_voice v[4];                 // 28
    uint8_t q_cue[SND_QUEUE];       // 8
    uint8_t q_wait[SND_QUEUE];      // 8: ticks to go; 0 is a free slot
    uint8_t q_flags[SND_QUEUE];     // 8
    uint8_t wave;                   // 1: the wave table in CH3's RAM, 0xFF none
} snd_state;                        // 53

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
#ifdef SND_BANK
#include <gb/gb.h>
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

static const uint8_t base[4] = { 0x10, 0x15, 0x1A, 0x1F };

// Quiet without turning a DAC off: a DAC switched off or on mid-play is a
// pop on the hardware, so the DACs go on once (snd_init) and stay on. A
// pulse or noise channel is retriggered at volume 0 (NRx2 0x08: volume 0,
// envelope up, no pace, DAC still on); the wave channel's level goes to mute.
static void silence(uint8_t ch) {
    if (ch == 2) { SND_WR(0x1C, 0x00); return; }
    SND_WR(base[ch] + 2u, 0x08);
    SND_WR(base[ch] + 4u, 0x80);
}

static void write_row(uint8_t ch, const snd_row *r, uint8_t flags) {
    const uint8_t b = base[ch];
    uint8_t r2 = r->r2;
    if (r2 == 0) { silence(ch); return; }   // a rest
    // backwards, an envelope runs the other way
    if ((flags & SND_BACKWARDS) && ch != 2 && (r2 & 7u)) r2 ^= 0x08u;
    // a far seat's card lands 6 dB under one's own (AUDIO.md: 6.8 dB across the table)
    if ((flags & SND_FAR) && ch != 2) r2 = (uint8_t)((((r2 >> 4) + 1u) >> 1) << 4 | (r2 & 15u));
    if (r->r4 & 0x80u) {
        if (ch == 2) {
            if (r->r0 != 0xFFu && r->r0 != S.wave) {
                // the wave RAM is written with the DAC off (Pan Docs: wave RAM is only
                // reliably accessible while CH3 is off on the DMG)
                uint8_t i;
                SND_WR(0x1A, 0x00);
                for (i = 0; i < 16; i++) SND_WR(0x30u + i, snd_waves[r->r0][i]);
                S.wave = r->r0;
            }
            SND_WR(0x1A, 0x80);
        } else if (ch == 0) SND_WR(0x10, r->r0);
        SND_WR(b + 1u, r->r1);
        SND_WR(b + 2u, r2);
    }
    SND_WR(b + 3u, r->r3);
    SND_WR(b + 4u, r->r4);
}

static void start(uint8_t cue, uint8_t flags) {
    const snd_cue *c = &snd_cues[cue];
    const snd_take *t;
    uint8_t ch;
    if (!c->n_takes) return;
    t = &c->takes[snd_strikes % c->n_takes];
    snd_strikes++;
    snd_starts++;
    for (ch = 0; ch < 4; ch++) {
        const snd_row *r = t->v[ch];
        snd_voice *v = &S.v[ch];
        if (!r) continue;
        // a channel is taken from a cue of no higher priority
        if (v->row && v->prio > c->prio) continue;
        v->prio = c->prio;
        if (flags & SND_BACKWARDS) {
            v->first = r;
            while (r[1].t) r++;
        }
        v->flags = flags;
        v->row = r;
        v->left = r->t;
        write_row(ch, r, flags);
    }
}

void snd_init(void) {
    uint8_t i;
    SND_WR(0x26, 0x80);      // APU on
    SND_WR(0x24, 0x77);      // master volume full both sides
    SND_WR(0x25, 0xFF);      // every channel to both sides (DESIGN.md "Panning")
    SND_WR(0x1A, 0x80);      // the wave DAC on, for the session
    for (i = 0; i < 4; i++) { S.v[i].row = 0; silence(i); }
    for (i = 0; i < SND_QUEUE; i++) S.q_wait[i] = 0;
    S.wave = 0xFF;
    snd_owner = SND_OWNER_SOUND;
    snd_refused = 0; snd_cut = 0; snd_starts = 0; snd_strikes = 0;
    snd_active = 0;
}

void snd_strike(uint8_t cue, uint8_t delay, uint8_t n, uint8_t flags) {
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
        S.q_wait[i] = d;
    }
    // after the queue holds the strike: set before it, a tick between the two
    // could find nothing due, clear the flag, and never look at the strike
    // (it would stay queued, and snd_busy() true, for ever)
    snd_active = 1;
}

void snd_tick(void) {
    uint8_t i;
    // nothing sounding, or a bot thinking (snd_yield dropped every voice and
    // strike, the next tick finds nothing and clears this, and snd_strike
    // will not set it again until snd_claim): one load and a branch
    if (!snd_active) return;
    {
    BANK_IN;
    for (i = 0; i < 4; i++) {
        snd_voice *v = &S.v[i];
        if (!v->row || --v->left) continue;
        if (!(v->flags & SND_BACKWARDS)) {
            v->row++;
            if (!v->row->t) { v->row = 0; silence(i); continue; }
        } else {
            if (v->row == v->first) { v->row = 0; silence(i); continue; }
            v->row--;
        }
        v->left = v->row->t;
        write_row(i, v->row, v->flags);
    }
    // the strikes coming due start after the voices stepped, so a new row
    // holds its whole t ticks
    for (i = 0; i < SND_QUEUE; i++) {
        if (S.q_wait[i] && --S.q_wait[i] == 0) start(S.q_cue[i], S.q_flags[i]);
    }
    if (!snd_busy()) snd_active = 0;
    BANK_OUT;
    }
}

uint8_t snd_busy(void) {
    uint8_t i;
    for (i = 0; i < 4; i++) if (S.v[i].row) return 1;
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
    for (i = 0; i < 4; i++) { S.v[i].row = 0; silence(i); }
    for (i = 0; i < SND_QUEUE; i++) S.q_wait[i] = 0;
}

void snd_claim(void) {
    uint8_t i;
    // the scratch was the bot's: nothing in it is ours any more
    for (i = 0; i < 4; i++) S.v[i].row = 0;
    for (i = 0; i < SND_QUEUE; i++) S.q_wait[i] = 0;
    S.wave = 0xFF;
    snd_owner = SND_OWNER_SOUND;
}
