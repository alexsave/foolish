// ios_api_play.c - THE MOVE MENU AND THE BOARD'S GESTURE RULES (see
// ios/include/ios_api.h).
//
// Split out of ios_api.c by domain, unchanged. Two families that belong
// together: the packed legal menu, whichever board it is asked of, and the
// rules between a finger and a move that are read OFF such a menu.

#include "ios_api.h"
#include "ios_internal.h"

#include "game.h"
#include "legal.h"
#include "view.h"
#include "client_table.h"

#include <string.h>

// ---------- legal moves ----------------------------------------------------
//
// The packed wire carries the MOVE_* integer; naming it is the host's job.
//
// Client and server are kernel-to-kernel, so this hands out the SAME bytes the
// wasm build does, and Swift decodes them directly (MoveWire).

// The seat's legal moves as the packed move wire — identical layout to
// wasm_export_moves: u32 count, then per move {type, n_cards, cards[n_cards],
// attacks[n_cards]}. Cards use card_to_id (== wire_from_card for real cards);
// the reader ignores the attack bytes except on COVER.
static int emit_legal_packed(const Game *g, int seat, char *out, int cap) {
    static LegalMoves lm;
    calculate_legal_moves(g, seat, &lm);
    // NOTE: GOOD is intentionally NOT filtered here. The kernel menu stays the
    // full legal set (so fio_actor_mask and this packed menu agree, and bots see
    // GOOD as always - it is how a seat leaves the eligible set, see legal.c).
    // The owner's "a human only gets Good once the table is fully covered, and
    // it disappears when someone throws in" rule is a narrowing applied on the
    // way to a board (play_can_say_good / play_human_menu), and the "your move
    // with no live button" status case is handled in the board's status logic,
    // not by rewriting the kernel's legal set.
    const int n = legal_menu_write(&lm, 0, -1, (unsigned char *)out, cap);
    return n == LEGAL_WIRE_ECAP ? FIO_ECAP : n;
}

// The resident game's legal moves for `seat`, packed.
int fio_legal_packed(int seat, char *out, int cap) {
    const Game *g = fio_resident_game();
    if (!g) return FIO_ENOGAME;
    if (seat < 0 || seat >= g->num_players) return FIO_EBADARG;
    return emit_legal_packed(g, seat, out, cap);
}

// Legal moves for `seat` on THE BOARD THE SLOT HOLDS - the one the last adopt
// filled. A host that has adopted an envelope has the board in the kernel
// already; without this it would have to keep the envelope's bytes as well,
// slice the inner blob back out of them by offset, and hand them down again.
// Ask it in the same call as the adopt: the slot is one slot.
int fio_legal_from_view(int seat, char *out, int cap) {
    const ClientTable *c = fio_client();
    if (c->g->num_players < 2) return FIO_ENOGAME;
    if (seat < 0 || seat >= c->g->num_players) return FIO_EBADARG;
    return emit_legal_packed(c->g, seat, out, cap);
}

// Legal moves for `seat` computed from a SERVER packed masked view, packed out.
// The blob itself decodes to a GameView in pure Swift (MaskedView); its legal
// moves come through here (view.ts / MoveWire).
int fio_legal_from_packed(const uint8_t *buf, int len, int seat, char *out, int cap) {
    if (!buf || len <= 0) return FIO_EBADARG;
    Game *tmp = fio_scratch_game();       // the shared slot; see ios_api.c
    memset(tmp, 0, sizeof *tmp);
    if (state_import(tmp, buf, len, /*masked=*/1) != GAME_VALID) return FIO_EPARSE;
    if (tmp->num_players < 2) return FIO_EPARSE;
    if (seat < 0 || seat >= tmp->num_players) return FIO_EBADARG;
    return emit_legal_packed(tmp, seat, out, cap);
}

// ---------- what a gesture on a board means (the board rules) ---------------
//
// The rules between a finger and a move - which menu entry a drop resolves to,
// which battles a selection could cover, which one the Cover button aims at,
// which moves a human may make at all. They live in legal.c (play_*); this is
// the crossing.
//
// THESE READ NOTHING BUT THEIR ARGUMENTS. Not the resident game, not a static -
// which is what makes them safe to call from a SwiftUI render pass, where the
// resident game is behind an actor and unreachable. What a board asks about is
// its own PUBLISHED pair anyway: the menu it was handed and the table it was
// handed, which for the iMessage board is sometimes deliberately not the live
// position (an empty menu while a bout settlement is held back). See the note
// on PlayBoard in legal.h.

// Fill a PlayBoard from the crossing arguments. `table` is 2 bytes per battle,
// the attack then its cover or LEGAL_WIRE_NONE.
static PlayBoard fio_play_board(const uint8_t *menu, int menu_len,
                                const uint8_t *table, int n_battles,
                                int power_suit, int is_defender) {
    PlayBoard b;
    b.menu = menu; b.menu_len = menu_len;
    b.table = table; b.n_battles = n_battles;
    b.power_suit = power_suit; b.is_defender = is_defender;
    return b;
}

// ONE ANSWER, so a board cannot paint a highlight that the release then
// refuses: the resolved move, the coverable set and the button states all come
// out of one walk of one menu. Layout (LE):
//
//   0   u8    flags: 1 = attack legal with this selection, 2 = pass legal,
//                    4 = this seat may say good
//   1   i8    the battle the Cover button aims at, -1 for none
//   2   u64   bitmask of battles this selection could cover
//   10  ...   the resolved move as a ONE-ENTRY menu wire (count 0 or 1), so
//             MoveWire decodes it with no second format
int fio_play_probe(const uint8_t *menu, int menu_len,
                   const uint8_t *table, int n_battles,
                   int power_suit, int is_defender,
                   const uint8_t *sel, int n_sel, int target,
                   char *out, int cap) {
    if (!menu || menu_len < 0 || n_battles < 0 || n_sel < 0) return FIO_EBADARG;
    if (cap < FIO_PLAY_PROBE_HEAD + 4) return FIO_ECAP;
    const PlayBoard b = fio_play_board(menu, menu_len, table, n_battles,
                                       power_suit, is_defender);

    const uint64_t mask = play_coverable_battles(&b, sel, n_sel);
    const int best = play_best_cover_target(&b, sel, n_sel);
    const int idx  = play_resolve(&b, sel, n_sel, target);

    unsigned char *q = (unsigned char *)out;
    q[0] = (unsigned char)((play_has_verb(&b, MOVE_ATTACK, sel, n_sel) ? 1 : 0)
                         | (play_has_verb(&b, MOVE_PASS,   sel, n_sel) ? 2 : 0)
                         | (play_can_say_good(&b)                      ? 4 : 0));
    q[1] = (unsigned char)(signed char)best;
    for (int i = 0; i < 8; i++) q[2 + i] = (unsigned char)((mask >> (8 * i)) & 0xff);

    // The move itself, copied straight off the menu it was found on.
    unsigned char *m = q + FIO_PLAY_PROBE_HEAD;
    m[0] = m[1] = m[2] = m[3] = 0;
    if (idx < 0) return FIO_PLAY_PROBE_HEAD + 4;

    MenuWalk w;
    MenuMove mm;
    if (legal_menu_begin(&w, menu, menu_len) < 0) return FIO_PLAY_PROBE_HEAD + 4;
    while (legal_menu_next(&w, &mm) == 1 && w.index != idx) { }
    if (w.index != idx) return FIO_PLAY_PROBE_HEAD + 4;
    if (FIO_PLAY_PROBE_HEAD + 4 + 2 + 2 * mm.n_cards > cap) return FIO_ECAP;
    m[0] = 1;
    m[4] = (unsigned char)mm.type;
    m[5] = (unsigned char)mm.n_cards;
    for (int i = 0; i < mm.n_cards; i++) m[6 + i] = mm.cards[i];
    for (int i = 0; i < mm.n_cards; i++) m[6 + mm.n_cards + i] = mm.attacks[i];
    return FIO_PLAY_PROBE_HEAD + 4 + 2 + 2 * mm.n_cards;
}

// The moves a HUMAN may make on this board, as the same menu wire in.
int fio_play_human_menu(const uint8_t *menu, int menu_len,
                        const uint8_t *table, int n_battles,
                        char *out, int cap) {
    if (!menu || menu_len < 0 || n_battles < 0) return FIO_EBADARG;
    const PlayBoard b = fio_play_board(menu, menu_len, table, n_battles, -1, 0);
    const int n = play_human_menu(&b, (unsigned char *)out, cap);
    if (n == LEGAL_WIRE_ECAP) return FIO_ECAP;
    if (n < 0) return FIO_EPARSE;
    return n;
}
