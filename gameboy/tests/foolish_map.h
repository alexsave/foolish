// foolish_map - the two card encodings mapped explicitly, and a gd_move
// played on foolish's engine, for the tests that run both (difftest.c,
// simtest.c).
//   gd: id = suit * 9 + rank, rank 0..8 = 6..A.
//   foolish: Card {suit 0..3, value 5..13 = 6..A} (the 36-card deck); its
//   card id (card_to_id) is suit * 13 + value - 1. The suits are the same
//   0..3 (spades, hearts, clubs, diamonds).
#ifndef GD_FOOLISH_MAP_H
#define GD_FOOLISH_MAP_H
#include "../src/gd_rules.h"
#include "game.h"

static inline Card f_of(uint8_t id) {
    Card c;
    c.suit = (int8_t)(id / 9);
    c.value = (int8_t)(id % 9 + MIN_VALUE_SMALL);
    return c;
}
static inline uint8_t gd_of(Card c) {
    if (c.suit < 0 || c.suit > 3 || c.value < MIN_VALUE_SMALL || c.value > ACE_VALUE) return 0xEE;
    return (uint8_t)(c.suit * 9 + (c.value - MIN_VALUE_SMALL));
}
// foolish's card id of a gd card, and back
static inline int fid_of(uint8_t id) { return (id / 9) * 13 + id % 9 + MIN_VALUE_SMALL - 1; }
static inline uint8_t gd_of_fid(int fid) { return gd_of(card_of_id(fid)); }

static inline bool f_apply(Game *f, uint8_t seat, const gd_move *m) {
    Card c[GD_MAXB], a[GD_MAXB];
    for (int i = 0; i < m->n; i++) { c[i] = f_of(m->cards[i]); a[i] = f_of(m->atts[i] < 36 ? m->atts[i] : 0); }
    switch (m->type) {
    case GD_ATTACK: return handle_attack(f, seat, c, m->n);
    case GD_COVER:  return handle_cover(f, seat, c, a, m->n);
    case GD_PASS:   return handle_pass(f, seat, c, m->n);
    case GD_PICKUP: return handle_pickup(f, seat);
    case GD_GOOD:   return handle_good(f, seat);
    }
    return false;
}
#endif
