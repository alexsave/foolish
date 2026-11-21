// solvebench - the endgame solver's parts timed on the Game Boy (see
// gd_solve_bench in src/gd_solve.c): boards from games of random and New York
// moves once the stock is gone, each part run 50 times on each.
#include "../src/gd_rules.h"
#include "../src/gd_rng.h"
#include "../src/gd_sim.h"
#include "../src/gd_bot_hw.h"
#include <gb/gb.h>

#include "../src/gd_solve.h"
void gd_solve_bench(const gd_sim *s, uint16_t iters, uint32_t (*now)(void), uint32_t *out) BANKED;

static uint32_t clock_lines(void) {
    uint16_t t;
    uint8_t ly;
    __asm__("di");
    t = sys_time;
    ly = LY_REG;
    if (ly >= 144 && (IF_REG & VBL_IFLAG)) t++;
    __asm__("ei");
    return (uint32_t)t * 154u + (ly >= 144 ? ly - 144u : ly + 10u);
}

static gd_game g;
static gd_move m;
static gd_sim s;
uint32_t sb_parts[8], sb_out[9], sb_moves, sb_kind_gen[4], sb_kind_n[4];
uint16_t sb_boards;
uint8_t sb_done;

void main(void) {
    uint8_t deck[GD_NCARDS], i, j, t, k;
    *(volatile uint8_t *)0x0000u = 0x0Au;
    *(volatile uint8_t *)0x6000u = 0x01u;
    gd_rng_seed(0x5151u);
    for (k = 0; k < 2; k++) {
        for (i = 0; i < GD_NCARDS; i++) deck[i] = i;
        for (i = GD_NCARDS - 1u; i > 0; i--) { j = (uint8_t)gd_rng_below((uint32_t)i + 1u); t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
        gd_deal(&g, 0, deck, (uint8_t)gd_rng_below(2));
        while (g.status == GD_PLAYING) {
            const uint32_t n0 = gd_menu_count(&g, 0), n1 = gd_menu_count(&g, 1);
            uint32_t pick;
            uint8_t seat;
            if (!n0 && !n1) break;
            if (g.stock_n == 0 && !g.has_flipped) {
                gd_sim_from_game(&s, &g);
                gd_solve_bench(&s, 50, clock_lines, sb_out);
                for (i = 0; i < 4; i++) sb_parts[i] += sb_out[i];
                for (i = 5; i < 8; i++) sb_parts[i - 1] += sb_out[i];
                sb_kind_gen[sb_out[8]] += sb_out[2];
                sb_kind_n[sb_out[8]]++;
                sb_moves += sb_out[4];
                sb_boards++;
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
    sb_done = 1;
    for (;;) vsync();
}
