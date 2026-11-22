// snd - the Game Boy Durak sound driver (DESIGN.md "The driver").
//
// One tick function, called from the timer interrupt, walks a few small
// tables of register rows. Game code strikes a cue with a delay in ticks; the
// driver never decides WHICH cue or WHEN (that is the game's, as the plan's
// is on the website), only what a strike writes to the APU.
//
// Portable C: on the Game Boy SND_WR writes the register at 0xFF00 + reg; on
// the Mac (host/render.c) it writes the APU model, so the preview runs this
// very file.
#ifndef SND_H
#define SND_H
#include <stdint.h>

// The tick: the input's timer interrupt, 4096 Hz / (256 - SND_TMA) = 60.2 Hz
// (input.c IN_TMA), so the driver adds no interrupt of its own. DESIGN.md
// "60 Hz or 241 Hz" has the measurement; host/render241 builds the 241 Hz
// variant (TMA 239, 11 ticks a stagger) for the comparison.
#ifndef SND_TMA
#define SND_TMA        188u
#define SND_TICK_HZ    60u
#define SND_STAGGER    3u       // 45 ms (ANIM_SOUND_STAGGER_MS) in ticks: 49.8 ms
#endif
#define SND_MAX_HITS   6u       // ANIM_SOUND_MAX_HITS
#define SND_QUEUE      8u

// The cues: the website's ids (foolish c/src/anim_plan.h), so a table row
// reads the same in both. 0 is none. 19 and up are the Game Boy's own.
enum {
    SND_NONE = 0,
    SND_DEAL = 1, SND_FLIP = 2, SND_LIFT = 3, SND_PLACE = 4, SND_COVER = 5,
    SND_PICKUP = 6, SND_SWEEP = 7, SND_PILE = 8, SND_OUT = 9, SND_REVERT = 10,
    SND_GOOD = 11, SND_HAND_OFF = 12, SND_FINISH = 13, SND_FOOL = 14,
    SND_DEAL_LAND = 15, SND_TURN = 16, SND_PASS = 17, SND_INTO_HAND = 18,
    SND_BURST = 19,          // one firework bursting (the win screen's loop)
    SND_FINISH_MIAMI = 20, SND_FINISH_NEW_YORK = 21, SND_FINISH_SEOUL = 22, SND_FINISH_MOSCOW = 23,
    SND_N_CUES = 24
};

// One register row: hold `t` ticks after writing it (t == 0 ends a voice).
// r0..r4 are the channel's NRx0..NRx4, except: CH2 has no NR20 (r0 unused);
// CH3's r0 is a wave table index (0xFF: keep the loaded one).
// Rule: a row with r2 == 0 turns the channel's DAC off (silence); a row with
// r4 bit 7 (trigger) writes all five; any other row writes r3 and r4 only (a
// pitch change), so an envelope is never rewritten while it runs (the DMG's
// "zombie mode" differs between models).
typedef struct { uint8_t t, r0, r1, r2, r3, r4; } snd_row;

// One take: a voice per channel, or 0. A cue: its takes, played in turn.
typedef struct { const snd_row *v[4]; } snd_take;
typedef struct { uint8_t prio; uint8_t n_takes; const snd_take *takes; } snd_cue;

extern const snd_cue snd_cues[SND_N_CUES];
extern const uint8_t snd_waves[][16];
extern const uint8_t snd_pre[SND_N_CUES];   // ticks from a strike to its moment: strike a landing this much early
extern const uint8_t snd_dur[SND_N_CUES];   // ticks from a strike to the end of its longest take

// Flags for a strike.
#define SND_BACKWARDS 1u     // a revert: the take's rows in reverse (AUDIO.md "A revert is the move it undoes, backwards")
#define SND_FAR       2u     // a card landing at the far seat: half the volume

// Who owns the shared scratch RAM (DESIGN.md "Memory: one owner at a time").
#define SND_OWNER_SOUND 0u
#define SND_OWNER_BOT   1u
// The bytes that outlive a think cannot be overlaid on the bots' scratch: on
// the Game Boy they are in HRAM (snd_hram.s), reached with ldh.
#ifdef __SDCC
#define SND_KEEP extern volatile __sfr
#else
#define SND_KEEP extern volatile uint8_t
#endif
SND_KEEP snd_owner;
SND_KEEP snd_refused;    // strikes refused while a bot owned the RAM (a test reads it; must stay 0)
SND_KEEP snd_cut;        // yields that cut a cue still sounding (the bot must wait for snd_busy(); must stay 0)
SND_KEEP snd_starts;     // strikes started, wrapping: what a test counts
SND_KEEP snd_active;     // set by a strike, cleared by the tick that finds nothing left, and by snd_yield
SND_KEEP snd_strikes;    // round robin over a cue's takes: never the same twice running

void snd_init(void);
// Strike `cue` `delay` ticks from now; `n` cards make min(n, 6) strikes,
// SND_STAGGER apart, from the delay on (never before it: a sound trails its
// picture).
void snd_strike(uint8_t cue, uint8_t delay, uint8_t n, uint8_t flags);
// The same, held until the frame clock, SND_CLOCK (sys_time, the VBlanks, on
// the Game Boy) reaches `at`, then `delay` ticks more: a landing, on the clock
// its flight runs on. A tick (60.24 Hz) slides against a frame (59.73 Hz), so
// a delay in ticks alone lands anywhere within a frame of the VBlank it aims
// at, sometimes before it; held, it starts on the first tick after it.
void snd_strike_at(uint8_t cue, uint16_t at, uint8_t delay, uint8_t n, uint8_t flags);
void snd_tick(void);         // the timer interrupt
uint8_t snd_busy(void);      // anything sounding or pending
// The bot is about to think: every channel silenced, the queue dropped, the
// RAM handed over. snd_claim() takes it back after the move is chosen.
void snd_yield(void);
void snd_claim(void);

// The frame clock snd_strike_at holds a strike against.
#ifdef SND_HOST
extern volatile uint16_t snd_host_clock;
#define SND_CLOCK snd_host_clock
#endif

// The register write. The Game Boy's is a store; the host defines its own.
#ifdef SND_HOST
void snd_host_write(uint8_t reg, uint8_t val);
#define SND_WR(reg, val) snd_host_write((uint8_t)(reg), (uint8_t)(val))
#else
#define SND_WR(reg, val) (*(volatile uint8_t *)(0xFF00u + (reg)) = (uint8_t)(val))
#endif

#endif
