// snd_test - the driver's rules on the Mac, against the register writes it
// makes (no APU needed). `make test` runs it; tests/mutants.sh breaks each
// rule in a copy of snd.c and requires this to fail.
#include "snd.h"
#include <stdio.h>

static uint8_t last[0x40];          // the last value written to each register
static int writes;
void snd_host_write(uint8_t reg, uint8_t val) { if (reg < 0x40) last[reg] = val; writes++; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// ticks until the start count changes, from now; -1 if not within n
static int ticks_to_start(int n) {
    const uint8_t s0 = snd_starts;
    for (int t = 1; t <= n; t++) { snd_tick(); if (snd_starts != s0) return t; }
    return -1;
}

int main(void) {
    // a strike starts on the next tick, never in the call (it trails)
    snd_init();
    { const uint8_t s0 = snd_starts; snd_strike(SND_PLACE, 0, 1, 0); CHECK(snd_starts == s0, "a strike started inside snd_strike"); }
    CHECK(ticks_to_start(4) == 1, "a delay-0 strike did not start on the first tick");

    // the stagger: two cards, two starts SND_STAGGER ticks apart
    snd_init();
    snd_strike(SND_PLACE, 5, 2, 0);
    { int a = ticks_to_start(20), b = ticks_to_start(20);
      CHECK(a == 6, "the first landing started at tick %d, not 6", a);
      CHECK(b == (int)SND_STAGGER, "the second landing came %d ticks after the first, not %u", b, SND_STAGGER); }

    // the cap: nine cards strike six times
    snd_init();
    { const uint8_t s0 = snd_starts; snd_strike(SND_COVER, 0, 9, 0);
      for (int t = 0; t < 100; t++) snd_tick();
      CHECK((uint8_t)(snd_starts - s0) == SND_MAX_HITS, "nine cards made %d strikes", (uint8_t)(snd_starts - s0)); }

    // the owner: while a bot thinks a strike is refused and counted, and the tick writes nothing
    snd_init();
    snd_strike(SND_LIFT, 0, 1, 0);
    snd_tick();
    snd_strike(SND_PLACE, 5, 2, 0);    // still in the queue when the think starts
    snd_yield();
    CHECK(!snd_busy(), "sound still busy after snd_yield");
    { const uint8_t r0 = snd_refused; snd_strike(SND_PLACE, 0, 1, 0); CHECK(snd_refused == r0 + 1, "a strike in a think was not refused"); }
    writes = 0;
    for (int t = 0; t < 50; t++) snd_tick();
    CHECK(writes == 0, "the tick wrote %d registers while a bot owned the RAM", writes);
    snd_claim();
    snd_strike(SND_PLACE, 0, 1, 0);
    CHECK(ticks_to_start(3) == 1, "no start after snd_claim");

    // backwards: the first row written is the take's last
    {
        const snd_row *r = snd_cues[SND_OUT].takes[0].v[1];
        const snd_row *end = r; while (end[1].t) end++;
        snd_init();
        snd_strike(SND_OUT, 0, 1, SND_BACKWARDS);
        snd_tick();
        CHECK(last[0x18] == end->r3, "backwards began on NR23 0x%02X, not the last row's 0x%02X", last[0x18], end->r3);
    }

    // far: the same row at half the volume
    {
        snd_init();
        snd_strike(SND_GOOD, 0, 1, 0); snd_tick();
        const uint8_t near = last[0x17] >> 4;
        snd_init();
        snd_strike(SND_GOOD, 0, 1, SND_FAR); snd_tick();     // GOOD has one take: the same row
        const uint8_t far = last[0x17] >> 4;
        CHECK(far == (uint8_t)((near + 1u) >> 1) && far < near, "far volume %u against near %u", far, near);
    }

    if (fails) { printf("snd_test: %d failed\n", fails); return 1; }
    printf("snd_test: ok\n");
    return 0;
}
