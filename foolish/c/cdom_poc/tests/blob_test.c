/* blob_test.c - is the kernel's state blob the whole of a game the port plays? (native)
 *
 * The port keeps its game across loads as state_blob_put's bytes and adopts them with
 * state_blob_load (README, Reload). This plays seeded games bot against bot (the handwritten
 * brain on both seats, as the port's bot) and at every step round-trips the board through the
 * blob into a second, zeroed Game, then checks:
 *   - the board the blob gives back is the same board (state_put unmasked, byte for byte);
 *   - the viewer's TableView and ViewRules read from it are the same (what the port draws);
 *   - the two games, driven on from the same RNG seed, make the same moves to the end
 *     (nothing the blob leaves out, such as the log or g->flipped after the trump is drawn,
 *     changes what the bot does or what the engine allows).
 * Seat 1 is the bot and seat 0 the human, as in the port; the human's moves, which the table
 * takes from clicks, are the same brain's here, driven alone.
 * The rules are the table's own (src/game_keep.c): the game made canonical after every operation
 * (without it the round-tripped game played another board: a passer's stale awaiting flag), the
 * blob written and adopted, the seats' kinds (not in the blob) given back on adopting.
 *
 *   ./build.sh builds and runs it (out/blob_test). Exits non-zero on the first difference. */
#include "game.h"
#include "view.h"
#include "client_table.h"
#include "bot_drive.h"
#include "strategy.h"
#include "../src/game_keep.h"
#include <stdio.h>
#include <string.h>

/* The handwritten brain for both seats: the bot's, and here also the human's (whose moves the
 * table takes from clicks). */
int bot_roster_find_by_strat(int strat) { return strat == STRAT_HANDWRITTEN_PROD || strat == STRATEGY_KEY_HUMAN ? 0 : -1; }
int handwritten_prod_strategy_choose(const Game *g, int seat, const LegalMoves *moves, void *ctx);
int bot_roster_choose(int idx, const Game *g, int seat, const LegalMoves *moves) {
  if (idx != 0 || !moves || moves->n <= 0) return -1;
  int chosen = handwritten_prod_strategy_choose(g, seat, moves, 0);
  return chosen >= 0 && chosen < moves->n ? chosen : -1;
}

static Game A, B;
static unsigned char blob[8192], pa[8192], pb[8192];
static ClientSlot SA, SB;
static ClientTable CA, CB;

static int same_board(const char *when, int seed, int step) {
  int la = state_put(&A, VIEW_UNMASKED, pa), lb = state_put(&B, VIEW_UNMASKED, pb);
  if (la != lb || memcmp(pa, pb, (size_t)la)) {
    printf("FAIL seed %d step %d %s: the board differs\n", seed, step, when);
    for (int i = 0; i < la; i++) if (pa[i] != pb[i]) printf("  byte %d: %d against %d\n", i, pa[i], pb[i]);
    printf("  battles %d %d, awaiting %d%d %d%d, goodts %d %d, goodmask %u %u\n", A.num_battles, B.num_battles, A.players[0].awaiting_attack, B.players[0].awaiting_attack, A.players[1].awaiting_attack, B.players[1].awaiting_attack, A.has_good_timestamp, B.has_good_timestamp, A.good_players_mask, B.good_players_mask);
    printf("  deck %d %d, hands %d/%d %d/%d\n", A.deck_count, B.deck_count, A.players[0].hand_count, B.players[0].hand_count, A.players[1].hand_count, B.players[1].hand_count);
    return 0; }
  for (int viewer = 0; viewer < 2; viewer++) {
    if (client_adopt_board(&CA, &A, viewer) != CLIENT_OK || client_adopt_board(&CB, &B, viewer) != CLIENT_OK) {
      printf("FAIL seed %d step %d %s: a view does not adopt\n", seed, step, when); return 0;
    }
    ViewRules ra, rb;
    memset(&ra, 0, sizeof ra); memset(&rb, 0, sizeof rb);
    client_view_rules(&CA.view, 0, 0, 0, &ra);
    client_view_rules(&CB.view, 0, 0, 0, &rb);
    if (memcmp(&CA.view, &CB.view, sizeof CA.view)) { printf("FAIL seed %d step %d %s: viewer %d's TableView differs\n", seed, step, when, viewer); return 0; }
    if (memcmp(&ra, &rb, sizeof ra)) { printf("FAIL seed %d step %d %s: viewer %d's ViewRules differ\n", seed, step, when, viewer); return 0; }
  }
  return 1;
}

int main(void) {
  client_init(&CA, &SA);
  client_init(&CB, &SB);
  const int8_t keys[2] = {STRATEGY_KEY_HUMAN, STRAT_HANDWRITTEN_PROD}; /* the port's seats */
  int games = 0, steps = 0;
  for (int seed = 1; seed <= 300; seed++) {
    memset(&A, 0, sizeof A);
    game_reset_to_lobby(&A, 0);
    game_set_seed((uint32_t)seed * 2654435761u | 1);
    game_seat_and_deal(&A, keys, 2);
    game_keep_canonical(&A);
    for (int step = 0; step < 2000; step++) {
      int len = game_keep_put(&A, blob);
      if (len <= 0) { printf("FAIL seed %d step %d: state_blob_put %d\n", seed, step, len); return 1; }
      if (!game_keep_load(&B, blob, len)) { printf("FAIL seed %d step %d: the blob does not load\n", seed, step); return 1; }
      if (!same_board("after a round trip", seed, step)) return 1;
      if (A.status != GAME_STATUS_PLAYING) break;
      /* As the port plays: seat 1 is the one bot (bot_drive with seat 0 masked as the human);
       * seat 0's moves, which the port takes from clicks, are the same brain's here, driven
       * alone. Both RNG streams (the draws and the bot's) set alike for both games. */
      uint32_t s = (uint32_t)(seed * 7919 + step) | 1;
      BotDriveOut oa, ob;
      uint32_t mask = bot_drive_eligible_mask(&A, 1u) ? 1u : 2u;
      game_set_seed(s);
      random_strategy_set_seed(s);
      bot_drive(&A, mask, BOT_DRIVE_MAX_ACTIONS, 0, 0, &oa);
      game_set_seed(s);
      random_strategy_set_seed(s);
      bot_drive(&B, mask, BOT_DRIVE_MAX_ACTIONS, 0, 0, &ob);
      game_keep_canonical(&A); /* as the table does after every operation */
      if (oa.n != ob.n || oa.stop != ob.stop || (oa.n && memcmp(&oa.actions[oa.n - 1], &ob.actions[ob.n - 1], sizeof oa.actions[0]))) {
        printf("FAIL seed %d step %d: the drives differ (%d/%d actions)\n", seed, step, oa.n, ob.n);
        return 1;
      }
      if (!same_board("after the next drive", seed, step)) {
        for (int i = 0; i < oa.n; i++) printf("  A seat %d type %d n %d\n", oa.actions[i].seat, oa.actions[i].move.type, oa.actions[i].move.n_cards);
        return 1;
      }
      steps++;
    }
    if (A.status != GAME_STATUS_GAME_OVER) { printf("FAIL seed %d: no end\n", seed); return 1; }
    games++;
  }
  printf("blob_test: %d games, %d steps, every board the same after a round trip and every next drive the same\n", games, steps);
  return 0;
}
