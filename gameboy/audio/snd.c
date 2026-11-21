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

static snd_state S;

volatile uint8_t snd_owner;
static uint8_t snd_active;          // set by a strike, cleared by the tick that finds nothing left
volatile uint8_t snd_refused;
volatile uint8_t snd_starts;        // strikes started, wrapping (gbrun and host/snd_test.c read it)
static uint8_t strikes;
uint8_t snd_busy(void);             // round robin over a cue's takes: never the same twice running

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
    t = &c->takes[strikes % c->n_takes];
    strikes++;
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
        if (i == SND_QUEUE) return;          // full: the later strikes of a beat are dropped
        snd_active = 1;
        S.q_cue[i] = cue;
        S.q_flags[i] = flags;
        S.q_wait[i] = d;
    }
}

void snd_tick(void) {
    uint8_t i;
    // nothing sounding, or a bot thinking (snd_yield dropped every voice and
    // strike, the next tick finds nothing and clears this, and snd_strike
    // will not set it again until snd_claim): one load and a branch
    if (!snd_active) return;
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
