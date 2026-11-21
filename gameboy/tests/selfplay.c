// selfplay - the cross-compiler check. The same portable code plays seeded games
// with uniformly random moves and folds every menu count, every move, every
// pill answer and every result into one CRC-16. tests/selfplay_host.c runs it
// built by clang, tests/selfplay_rom.c runs it inside a test ROM built by SDCC
// (read back by tools/gbrun), and the two sums must be equal: the difftest
// proves the clang build right, this proves the ROM computes the same thing.
#include "selfplay.h"
#include "../src/gd_play.h"
#include "../src/gd_rng.h"

static uint16_t crc;
// The sum after each of the first SELFPLAY_TRACE moves, to find where two builds part.
uint16_t selfplay_trace[SELFPLAY_TRACE];

// Every folded byte of the start of the run, for the same purpose.
uint8_t selfplay_log[SELFPLAY_LOG];
uint16_t selfplay_log_n;

static void fold(uint8_t b) {
    uint8_t i;
    if (selfplay_log_n < SELFPLAY_LOG) selfplay_log[selfplay_log_n++] = b;
    crc ^= (uint16_t)b << 8;
    for (i = 0; i < 8; i++) crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
}

static void fold32(uint32_t v) {
    fold((uint8_t)v); fold((uint8_t)(v >> 8)); fold((uint8_t)(v >> 16)); fold((uint8_t)(v >> 24));
}

static gd_game g;
static gd_rep r;
static gd_move m;

static void gesture(uint8_t seat, const uint8_t *sel, uint8_t n) {
    gd_move cm;
    int8_t b;
    uint8_t i;
    fold32(gd_coverable(&g, seat, sel, n));
    b = gd_best_cover(&g, seat, sel, n);
    fold((uint8_t)b);
    if (b >= 0 && gd_resolve_cover(&g, seat, sel, n, (uint8_t)b, &cm))
        for (i = 0; i < cm.n; i++) { fold(cm.cards[i]); fold(cm.atts[i]); }
}

uint16_t selfplay(uint16_t seed, uint8_t games, uint16_t *moves_out) {
    uint8_t deck[GD_NCARDS], i, j, t, k, c;
    uint16_t moves = 0;
    crc = 0xFFFFu;
    gd_rng_seed(seed);
    for (k = 0; k < games; k++) {
        for (i = 0; i < GD_NCARDS; i++) deck[i] = i;
        for (i = GD_NCARDS - 1u; i > 0; i--) {
            j = (uint8_t)gd_rng_below((uint32_t)i + 1u);
            t = deck[i]; deck[i] = deck[j]; deck[j] = t;
        }
        gd_deal(&g, &r, deck, (uint8_t)gd_rng_below(2));
        fold(g.trump); fold(g.fa);
        while (g.status == GD_PLAYING && moves < 60000u) {
            const uint32_t n0 = gd_menu_count(&g, 0), n1 = gd_menu_count(&g, 1);
            uint32_t pick;
            uint8_t seat;
            fold32(n0); fold32(n1);
            // every single card's pills, for both seats
            for (c = 0; c < GD_NCARDS; c++)
                for (seat = 0; seat < 2; seat++)
                    if (GD_HAS(g.hand[seat], c)) fold(gd_pills(&g, seat, &c, 1));
            fold(gd_pills(&g, 0, &c, 0)); fold(gd_pills(&g, 1, &c, 0));
            // the defender's gestures: each card alone, and each pair of
            // neighbouring cards, through coverable, the Cover aim and resolve
            {
                uint8_t sel[2], prev = 0xFF, d = g.defender;
                for (c = 0; c < GD_NCARDS; c++) {
                    if (!GD_HAS(g.hand[d], c)) continue;
                    sel[0] = c;
                    gesture(d, sel, 1);
                    if (prev != 0xFF) { sel[0] = prev; sel[1] = c; gesture(d, sel, 2); }
                    prev = c;
                }
            }
            if (n0 + n1 == 0) { fold(0xEE); break; }
            pick = gd_rng_below(n0 + n1);
            seat = pick < n0 ? 0 : 1;
            gd_menu_move(&g, seat, seat ? pick - n0 : pick, &m);
            fold(m.type); fold(m.n);
            for (i = 0; i < m.n; i++) { fold(m.cards[i]); fold(m.type == GD_COVER ? m.atts[i] : 0); }
            if (!gd_apply(&g, &r, seat, &m)) { fold(0xDD); break; }
            if (moves < SELFPLAY_TRACE) selfplay_trace[moves] = crc;
            moves++;
        }
        fold(gd_result(&g));
    }
    *moves_out = moves;
    return crc;
}
