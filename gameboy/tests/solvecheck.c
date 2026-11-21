// solvecheck - the endgame solver on the Game Boy: its cross-compiler check
// and its speed. Games of moves (uniformly random or New York's, half each, as
// in solvetest), and at every ply once the stock and the face-up trump are
// gone, a solve of the board from each seat's side with Moscow's node cap
// (full window, table emptied first). Each value, whether it aborted, and the
// nodes it used are folded into a CRC-16. The clang build prints the sum; the
// ROM build (banked like the game, with the cart's 32 KB of RAM) leaves it in
// RAM with the solve times on the scanline clock, and the sums must agree.
#include "../src/gd_rules.h"
#include "../src/gd_rng.h"
#include "../src/gd_sim.h"
#include "../src/gd_solve.h"
#include "../src/gd_bot_hw.h"
#ifdef SOLVECHECK_HOST
#include <stdio.h>
#else
#include <gb/gb.h>
#endif

#define SC_SEED  0x5151u
#ifndef SC_GAMES
#define SC_GAMES 3
#endif
#ifndef SC_BUDGET
#define SC_BUDGET 1500u
#endif

static uint16_t crc;
static void fold(uint8_t b) {
    uint8_t i;
    crc ^= (uint16_t)b << 8;
    for (i = 0; i < 8; i++) crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
}

#ifdef SOLVECHECK_HOST
static uint32_t clock_lines(void) { return 0; }
#else
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
#endif

static gd_game g;
static gd_move m;
static gd_sim s;
uint16_t sc_sum, sc_solves, sc_resolved;
uint32_t sc_nodes, sc_lines, sc_lines_max, sc_nodes_at_max;
uint8_t sc_done;
int16_t sc_last_v;
uint8_t sc_last_ab;
uint32_t sc_last_nodes;

static void run(void) {
    uint8_t deck[GD_NCARDS], i, j, t, k, me, ab;
    uint32_t bud, t0, dt;
    int16_t v;
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
            if (g.stock_n == 0 && !g.has_flipped)
                for (me = 0; me < 2; me++) {
                    gd_sim_from_game(&s, &g);
                    bud = SC_BUDGET;
                    gd_solve_reset();
                    t0 = clock_lines();
                    v = gd_solve(&s, me, -2000, 2000, &bud, 0, &ab);
                    dt = clock_lines();
                    dt = dt >= t0 ? dt - t0 : dt + 65536ul * 154u - t0;     // across sys_time's wrap
                    sc_lines += dt;
                    if (dt > sc_lines_max) { sc_lines_max = dt; sc_nodes_at_max = SC_BUDGET - bud; }
                    sc_nodes += SC_BUDGET - bud;
                    fold((uint8_t)v); fold((uint8_t)((uint16_t)v >> 8)); fold(ab);
                    fold((uint8_t)(SC_BUDGET - bud)); fold((uint8_t)((SC_BUDGET - bud) >> 8));
                    sc_solves++;
                    if (!ab) sc_resolved++;
                    sc_last_v = v; sc_last_ab = ab; sc_last_nodes = SC_BUDGET - bud;
#ifdef SC_STOP
                    if (sc_solves == SC_STOP) { sc_sum = crc; return; }
#endif
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

#ifdef SOLVECHECK_HOST
int main(void) {
    run();
    fprintf(stderr, "solves %u, resolved %u, nodes %lu (%.0f a solve)\n", sc_solves, sc_resolved,
            (unsigned long)sc_nodes, (double)sc_nodes / sc_solves);
    fprintf(stderr, "last: v %d ab %u nodes %lu\n", sc_last_v, sc_last_ab, (unsigned long)sc_last_nodes);
    printf("%02x %02x %02x %02x\n", sc_sum & 0xFF, sc_sum >> 8, sc_solves & 0xFF, sc_solves >> 8);
    return 0;
}
#else
#ifdef LIVE_STRESS
// make livestress: the live-think handler's stand-in runs on its own stack in
// cart RAM bank 3 on every tick of a fast timer while the solver switches
// banks 0 to 3 under it; the solves must still be clang's
#include "../src/input.h"
#include "../src/live.h"
uint32_t sc_ticks;
uint8_t sc_stack_sum;
static void sc_live(void) {
    uint8_t buf[64], i, sum = 0;
    for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i + sc_ticks);
    for (i = 0; i < sizeof buf; i++) sum += buf[i];
    sc_stack_sum = sum;
    sc_ticks++;
}
#endif

void main(void) {
    // the cart's RAM: enabled, and MBC1's 4/32 mode so 0x4000 picks its bank
    *(volatile uint8_t *)0x0000u = 0x0Au;
    *(volatile uint8_t *)0x6000u = 0x01u;
#ifdef LIVE_STRESS
    in_init();
    live_start(sc_live);
#endif
    run();
#ifdef LIVE_STRESS
    live_stop();
#endif
    sc_done = 1;
    for (;;) vsync();
}
#endif
