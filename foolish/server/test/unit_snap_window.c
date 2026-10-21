// unit_snap_window.c - a table operation's snapshot store (foolish/c/src/table.c, snap_room.h)
// in the server's build, where an assert is live: no NDEBUG anywhere in it.
//
// The store drops a hook it has no room for; that is its design. It used to assert first in
// every hosted build without NDEBUG, and `cbase serve` is one, so a store an operation outgrew
// would have aborted the process and taken every table down. The assert is now opt-in
// (-DENGINE_SNAP_ASSERT, the kernel's own suites), and table.c asserts at compile time that
// MAX_SNAPS holds the widest window an operation can open (TABLE_ACTION_HOOKS_MAX).
//
//   1. In this build a full store drops: the check compiles to nothing.
//   2. The widest bot cycle: every one of eight seats acts in one table_bot_drive, seven silent
//      goods and then a cover that empties the defender's hand, so the bout ends and every seat
//      draws. Its hooks are counted one by one, and the window holds them with room to spare.
//      The deal's 51 hooks are in the store when the drive starts: the drive's own window
//      starts empty.
#include "t.h"

#include "bot_drive.h"
#include "game.h"
#include "legal.h"
#include "snap_room.h"
#include "table.h"

#include <string.h>

#define RS(s) (s), (int)strlen(s)

static void a_full_store_drops_and_never_aborts(void) {
    // The same flags as the kernel objects this binary links (c_base/Makefile KERNEL_CFLAGS):
    // neither defines ENGINE_SNAP_ASSERT. Before, this line aborted the process.
    ENGINE_SNAP_ROOM(0);
    T_CHECK(1);
}

static Game g_game, g_board;
static TableSnaps g_snaps;
static Table g_t;
static BotDriveOut g_out;

static void hand(Player *p, const Card *c, int n) {
    for (int i = 0; i < n; i++) p->hand[i] = c[i];
    p->hand_count = (int16_t)n;
}

// Eight seats, the 52-card deck, diamonds trumps. Seat 0 attacks seat 1 with the spades 2..5;
// seat 1 holds exactly their covers, the spades 10..K. The seven attackers hold five cards each,
// and the stock eight and the flipped trump, so a refill gives every attacker one card and the
// defender the last two: eight DRAW hooks.
static void widest_board(Game *g) {
    static const int8_t S = SUIT_SPADES, H = SUIT_HEARTS, C = SUIT_CLUBS, D = SUIT_DIAMONDS;
    g->power_suit = D;
    g->first_attacker = 0;
    g->defender = 1;
    g->good_players_mask = 0;
    g->num_battles = 4;
    for (int i = 0; i < 4; i++) g->table_battles[i] = (Battle){ (Card){ S, (int8_t)(2 + i) }, CARD_NONE };
    const Card covers[4] = { { S, 10 }, { S, 11 }, { S, 12 }, { S, 13 } };
    hand(&g->players[1], covers, 4);
    // The other 44 cards: hearts and clubs 1..13, diamonds 1..12, and the spades 1, 6..9.
    Card rest[44];
    int n = 0;
    for (int v = 1; v <= 13; v++) rest[n++] = (Card){ H, (int8_t)v };
    for (int v = 1; v <= 13; v++) rest[n++] = (Card){ C, (int8_t)v };
    for (int v = 1; v <= 12; v++) rest[n++] = (Card){ D, (int8_t)v };
    rest[n++] = (Card){ S, 1 };
    for (int v = 6; v <= 9; v++) rest[n++] = (Card){ S, (int8_t)v };
    T_EQ_INT(n, 43);
    int at = 0;
    for (int s = 0; s < 8; s++) {
        if (s == 1) continue;
        hand(&g->players[s], rest + at, 5);
        at += 5;
    }
    for (int i = 0; i < 8; i++) g->deck[i] = rest[at++];
    g->deck_count = 8;
    T_EQ_INT(at, 43);
    g->has_flipped = true;
    g->flipped = (Card){ D, 13 };
    for (int s = 0; s < 8; s++) g->players[s].status = PLAYER_STATUS_IN;
    // Seat 0 is the lobby's human: made a bot here, so all eight seats are the drive's.
    g->players[0].strategy_key = g->players[2].strategy_key;
}

static void the_widest_bot_cycle_fits(void) {
    uint8_t seed[FOOLISH_SEED_LEN];
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) seed[i] = (uint8_t)(i * 37 + 11);
    table_init(&g_t, &g_game, &g_snaps);
    T_EQ_INT(table_create(&g_t, RS("h"), RS("Human")), TABLE_OK);
    for (int b = 1; b <= 7; b++) {
        char id[16], name[24];   // room for any int, which gcc -Wformat-truncation counts on
        snprintf(id, sizeof id, "b%d", b);
        snprintf(name, sizeof name, "Bot %d", b);
        T_CHECK(table_add_bot(&g_t, RS("h"), RS(id), RS(name), RS("random"), seed) >= 0);
    }
    T_CHECK(table_ready(&g_t, RS("h"), seed) >= 0);
    T_EQ_INT(g_game.num_players, 8);
    T_EQ_INT(g_game.status, GAME_STATUS_PLAYING);
    T_EQ_INT(g_snaps.n, ENGINE_DEAL_HOOKS);   // the deal's window, still in the store
    widest_board(&g_game);
    memcpy(&g_board, &g_game, sizeof g_board);

    // What each seat plays: the seven attackers say good (silent: an attack is uncovered), the
    // defender covers all four.
    BotDriveOut want;
    memset(&want, 0, sizeof want);
    for (int s = 0; s < 8; s++) {
        BotDriveAction *a = &want.actions[want.n++];
        a->seat = (int8_t)s;
        if (s != 1) { a->move.type = MOVE_GOOD; continue; }
        a->move.type = MOVE_COVER;
        a->move.n_cards = 4;
        for (int i = 0; i < 4; i++) {
            a->move.cards[i] = g_board.players[1].hand[i];
            a->move.attack_cards[i] = g_board.table_battles[i].attack;
        }
    }
    uint8_t prefs[256];
    const int pl = table_drive_prefs(&g_t, &want, prefs, (int)sizeof prefs);
    T_CHECK(pl > 0);

    // The cycle's order is a shuffle seeded from the public board; the defender acts last on
    // some discard count, and then all eight act.
    int n = 0, tries = 0;
    for (; tries < 200 && n != 8; tries++) {
        memcpy(&g_game, &g_board, sizeof g_game);
        g_game.discard_pile_length = (int16_t)tries;
        n = table_bot_drive(&g_t, prefs, pl, 0, &g_out);
    }
    T_EQ_INT(n, 8);
    for (int i = 0; i < 7; i++) {
        T_EQ_INT(g_out.actions[i].move.type, MOVE_GOOD);
        T_EQ_INT(g_out.actions[i].pacing_class, BOT_PACE_BUNDLED_PASSIVE);
    }
    T_EQ_INT(g_out.actions[7].seat, 1);
    T_EQ_INT(g_out.actions[7].move.type, MOVE_COVER);
    T_EQ_INT(g_game.num_battles, 0);   // the bout ended

    // Its window, hook by hook: four COVERs, the DISCARD, a DRAW for each of the eight seats,
    // the DEFENDER_MOVE. The silent goods fired none.
    int count[16] = { 0 };
    for (int i = 0; i < g_snaps.n; i++)
        if (g_snaps.tag[i] >= 0 && g_snaps.tag[i] < 16) count[g_snaps.tag[i]]++;
    T_EQ_INT(count[ENGINE_HOOK_COVER], 4);
    T_EQ_INT(count[ENGINE_HOOK_DISCARD], 1);
    T_EQ_INT(count[ENGINE_HOOK_DRAW], 8);
    T_EQ_INT(count[ENGINE_HOOK_DEFENDER_MOVE], 1);
    T_EQ_INT(g_snaps.n, 14);
    T_CHECK(g_snaps.n < MAX_SNAPS);
    T_CHECK(table_cast_delta_ok(&g_t));   // and it fits the client's delta slot, so viewers get the delta
    printf("     the widest cycle: %d actions, %d hooks of %d (after %d tries)\n", n, g_snaps.n, MAX_SNAPS, tries);
}

int main(void) {
    T_RUN(a_full_store_drops_and_never_aborts);
    T_RUN(the_widest_bot_cycle_fits);
    return T_DONE();
}
