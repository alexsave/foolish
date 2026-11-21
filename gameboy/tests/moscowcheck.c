// moscowcheck - the cross-compiler check for Moscow: games of moves (uniformly
// random or New York's, half each, so the stock runs out into endgames like
// real play), and at every turn seat 1 has, Moscow's decision (not played, so
// the positions do not depend on it) folded into a CRC-16 with its
// candidates, playouts, solver verdict and nodes. The clang build prints the
// sum; the ROM build (banked like the game, with the cart's RAM) leaves it in
// RAM, and they must agree.
#include "../src/gd_rules.h"
#include "../src/gd_rng.h"
#include "../src/gd_bot_moscow.h"
#include "../src/gd_bot_hw.h"
#ifdef MOSCOWCHECK_HOST
#include <stdio.h>
#else
#include <gb/gb.h>
#endif

#define SC_SEED  0x4343u
#define SC_GAMES 2

static uint16_t crc;
static void fold(uint8_t b) {
    uint8_t i;
    crc ^= (uint16_t)b << 8;
    for (i = 0; i < 8; i++) crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
}

static gd_game g;
static gd_move m, mine;
uint16_t sc_sum, sc_decisions, sc_solves;
uint8_t sc_done;

static void run(void) {
    uint8_t deck[GD_NCARDS], i, j, t, k;
    crc = 0xFFFFu;
    gd_rng_seed(SC_SEED);
    for (k = 0; k < SC_GAMES; k++) {
        for (i = 0; i < GD_NCARDS; i++) deck[i] = i;
        for (i = GD_NCARDS - 1u; i > 0; i--) { j = (uint8_t)gd_rng_below((uint32_t)i + 1u); t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
        gd_deal(&g, 0, deck, (uint8_t)gd_rng_below(2));
        while (g.status == GD_PLAYING) {
            const uint32_t n0 = gd_menu_count(&g, 0), n1 = gd_menu_count(&g, 1);
            uint32_t pick;
            uint8_t seat;
            if (!n0 && !n1) break;
            if (n1) {
                gd_moscow_choose(&g, 1, &mine);
                fold(mine.type); fold(mine.n);
                for (i = 0; i < mine.n && mine.type != 0xFF; i++) { fold(mine.cards[i]); fold(mine.type == GD_COVER ? mine.atts[i] : 0); }
                fold(gd_moscow_cands); fold((uint8_t)gd_moscow_playouts); fold(gd_moscow_solve);
                fold((uint8_t)gd_moscow_nodes); fold((uint8_t)(gd_moscow_nodes >> 8));
                if (gd_moscow_solve) sc_solves++;
                sc_decisions++;
            }
            pick = gd_rng_below(n0 + n1);
            seat = pick < n0 ? 0 : 1;
            if (gd_rng_below(2)) {
                gd_hw_choose(&g, seat, &m);
                if (m.type == 0xFF || !gd_legal(&g, seat, &m)) gd_menu_move(&g, seat, seat ? pick - n0 : pick, &m);
            } else gd_menu_move(&g, seat, seat ? pick - n0 : pick, &m);
            gd_apply(&g, 0, seat, &m);
        }
    }
    sc_sum = crc;
}

#ifdef MOSCOWCHECK_HOST
int main(void) {
    run();
    fprintf(stderr, "decisions %u, endgame solves %u\n", sc_decisions, sc_solves);
    printf("%02x %02x %02x %02x\n", sc_sum & 0xFF, sc_sum >> 8, sc_decisions & 0xFF, sc_decisions >> 8);
    return 0;
}
#else
void main(void) {
    *(volatile uint8_t *)0x0000u = 0x0Au;     // the cart's RAM, MBC1 4/32 mode
    *(volatile uint8_t *)0x6000u = 0x01u;
    run();
    sc_done = 1;
    for (;;) vsync();
}
#endif
