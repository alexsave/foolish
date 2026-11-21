// gd_play - see gd_play.h.
#include "gd_play.h"

#define BIT32(i) ((uint32_t)1u << (i))

static uint8_t can_cover(uint8_t a, uint8_t d, uint8_t trump) {
    if (gd_suit_of[a] != gd_suit_of[d]) return gd_suit_of[d] == trump && gd_suit_of[a] != trump;
    return gd_rank_of[d] > gd_rank_of[a];
}

// Is this seat the defender of a live bout, with these cards its own and distinct.
static uint8_t defends_with(const gd_game *g, uint8_t seat, const uint8_t *sel, uint8_t n) {
    gd_mask set;
    uint8_t i;
    if (g->status != GD_PLAYING || seat > 1 || (g->out & (1u << seat)) || seat != g->defender || g->nb == 0) return 0;
    if (n == 0) return 0;
    for (i = 0; i < 5; i++) set.b[i] = 0;
    for (i = 0; i < n; i++) {
        if (sel[i] >= GD_NCARDS || GD_HAS(set, sel[i]) || !GD_HAS(g->hand[seat], sel[i])) return 0;
        GD_SET(set, sel[i]);
    }
    return 1;
}

uint8_t gd_can_say_good(const gd_game *g, uint8_t seat) {
    gd_move m;
    if (g->nb == 0 || gd_uncovered(g) != 0) return 0;
    m.type = GD_GOOD;
    m.n = 0;
    return gd_in_menu(g, seat, &m);
}

// ---- matching the selection onto the uncovered battles --------------------

static uint32_t adj[GD_MAXB];        // per selected card: the battles it can cover
static int8_t owner[GD_MAXB];        // per battle: the card holding it, -1 free

static uint8_t augment(uint8_t c, uint32_t *seen) {
    uint8_t b;
    for (b = 0; b < GD_MAXB; b++) {
        if (!(adj[c] & BIT32(b)) || (*seen & BIT32(b))) continue;
        *seen |= BIT32(b);
        if (owner[b] < 0 || augment((uint8_t)owner[b], seen)) { owner[b] = (int8_t)c; return 1; }
    }
    return 0;
}

// Can every selected card except `skip` get its own battle, avoiding battle `ban`?
static uint8_t matches_all(uint8_t n, uint8_t skip, uint8_t ban) {
    uint8_t c, b;
    for (b = 0; b < GD_MAXB; b++) owner[b] = -1;
    if (ban < GD_MAXB) owner[ban] = 127;      // held by nobody we can move
    for (c = 0; c < n; c++) {
        uint32_t seen = ban < GD_MAXB ? BIT32(ban) : 0;
        if (c == skip) continue;
        if (!augment(c, &seen)) return 0;
    }
    return 1;
}

uint32_t gd_coverable(const gd_game *g, uint8_t seat, const uint8_t *sel, uint8_t n) {
    uint32_t mask = 0;
    uint8_t c, b;
    if (!defends_with(g, seat, sel, n) || n > gd_uncovered(g)) return 0;
    for (c = 0; c < n; c++) {
        adj[c] = 0;
        for (b = 0; b < g->nb; b++)
            if (g->def[b] == GD_NONE && can_cover(g->att[b], sel[c], g->trump)) adj[c] |= BIT32(b);
    }
    // Battle b is coverable when some card covering it leaves the rest a full
    // matching on the other battles: then a cover move with exactly these cards
    // covers b.
    for (b = 0; b < g->nb; b++) {
        if (g->def[b] != GD_NONE) continue;
        for (c = 0; c < n; c++) {
            if (!(adj[c] & BIT32(b))) continue;
            if (matches_all(n, c, b)) { mask |= BIT32(b); break; }
        }
    }
    return mask;
}

int8_t gd_best_cover(const gd_game *g, uint8_t seat, const uint8_t *sel, uint8_t n) {
    const uint32_t mask = gd_coverable(g, seat, sel, n);
    int8_t best = -1;
    uint8_t b;
    for (b = 0; b < g->nb; b++) {
        uint8_t sb, sw;
        if (!(mask & BIT32(b))) continue;
        if (best < 0) { best = (int8_t)b; continue; }
        sb = (uint8_t)(gd_rank_of[g->att[(uint8_t)best]] + (gd_suit_of[g->att[(uint8_t)best]] == g->trump ? 100 : 0));
        sw = (uint8_t)(gd_rank_of[g->att[b]] + (gd_suit_of[g->att[b]] == g->trump ? 100 : 0));
        if (sw > sb) best = (int8_t)b;
    }
    return best;
}

// ---- play_resolve onto a named battle ---------------------------------------
//
// foolish's menu lists covers as k-subsets of the uncovered battles in table
// order, and within one subset assigns each attack in turn a covering hand card
// in hand order. The first entry whose cards are exactly the selection and
// which covers `target` is the move. Only selected cards can be in it, so the
// walk below tries those alone, in hand (card id) order.
static uint8_t rs_unc[GD_MAXB], rs_nu, rs_k, rs_target;
static uint8_t rs_pick[GD_MAXB];
static uint8_t rs_sel[GD_MAXB], rs_n;
static uint32_t rs_used;             // bits over rs_sel
static uint8_t rs_card[GD_MAXB];
static const gd_game *rs_g;

static uint8_t rs_assign(uint8_t depth) {
    uint8_t i;
    if (depth == rs_k) return 1;
    for (i = 0; i < rs_n; i++) {
        if (rs_used & BIT32(i)) continue;
        if (!can_cover(rs_g->att[rs_pick[depth]], rs_sel[i], rs_g->trump)) continue;
        rs_used |= BIT32(i);
        rs_card[depth] = rs_sel[i];
        if (rs_assign((uint8_t)(depth + 1u))) return 1;
        rs_used &= ~BIT32(i);
    }
    return 0;
}

static uint8_t rs_subset(uint8_t start, uint8_t depth, uint8_t has_target) {
    uint8_t i;
    if (depth == rs_k) {
        if (!has_target) return 0;
        rs_used = 0;
        return rs_assign(0);
    }
    for (i = start; (uint8_t)(i + (rs_k - depth)) <= rs_nu; i++) {
        rs_pick[depth] = rs_unc[i];
        if (rs_subset((uint8_t)(i + 1u), (uint8_t)(depth + 1u), has_target || rs_unc[i] == rs_target)) return 1;
    }
    return 0;
}

uint8_t gd_resolve_cover(const gd_game *g, uint8_t seat, const uint8_t *sel, uint8_t n,
                         uint8_t target, gd_move *out) {
    uint8_t i, j, c;
    if (!defends_with(g, seat, sel, n) || target >= g->nb || g->def[target] != GD_NONE) return 0;
    rs_g = g;
    rs_nu = 0;
    for (i = 0; i < g->nb; i++) if (g->def[i] == GD_NONE) rs_unc[rs_nu++] = i;
    if (n > rs_nu) return 0;
    // The selection in hand order.
    rs_n = 0;
    for (c = 0; c < GD_NCARDS; c++)
        for (j = 0; j < n; j++) if (sel[j] == c) rs_sel[rs_n++] = c;
    rs_k = n;
    rs_target = target;
    if (!rs_subset(0, 0, 0)) return 0;
    out->type = GD_COVER;
    out->n = n;
    for (i = 0; i < n; i++) { out->cards[i] = rs_card[i]; out->atts[i] = g->att[rs_pick[i]]; }
    return 1;
}

// ---- the pills ---------------------------------------------------------------

uint8_t gd_pills(const gd_game *g, uint8_t seat, const uint8_t *sel, uint8_t n) {
    const uint8_t defender = seat == g->defender;
    const uint8_t out_of_play = (g->out & (1u << seat)) || g->status != GD_PLAYING;
    uint8_t pills = 0, i;
    gd_move m;
    if (n > 0 && n <= GD_MAXB) {
        m.n = n;
        for (i = 0; i < n; i++) m.cards[i] = sel[i];
        m.type = GD_ATTACK;
        if (!defender && gd_in_menu(g, seat, &m)) pills |= GD_PILL_ATTACK;
        m.type = GD_PASS;
        if (defender && gd_in_menu(g, seat, &m)) pills |= GD_PILL_PASS;
        if (defender && gd_coverable(g, seat, sel, n)) pills |= GD_PILL_COVER;
    }
    if (n == 0 && gd_can_say_good(g, seat)) pills |= GD_PILL_GOOD;
    // Take reads no menu: a defender with a table may take, covered or not.
    if (defender && g->nb > 0 && n == 0 && !out_of_play) pills |= GD_PILL_PICKUP;
    return pills;
}
