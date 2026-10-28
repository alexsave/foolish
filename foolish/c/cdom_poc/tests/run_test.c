/* run_test.c - does every operation the table makes give the kernel's stream for my seat? (native)
 *
 * The table animates each operation (a deal, a move of mine, a bot step) from src/anim_run.c: the
 * engine's hook snapshots, written for my seat by evwire_serialize and read back through the client
 * slot, as the web reads a server's push. This plays seeded games as the port does (seat 0 the
 * human, here the same handwritten brain driven alone; seat 1 the bot) and checks, for every
 * operation:
 *   - it gives steps (a good that ends nothing gives none, and at most a quarter may), and the
 *     stream's final board is the board the table adopts afterwards (client_adopt_board of the game,
 *     my seat), and the last step's draws the same but for the roles (the lead and the shield move
 *     after their last event): a run ends on the kernel's board;
 *   - the plan the table builds from them (client_plan_step, client_plan) is the kernel's, and
 *     lasts what anim_stream_ms says a stream of those types and seats lasts;
 *   - the deal is one DEAL step per card at ANIM_DEAL_CARD_MS each, my cards named and the bot's
 *     masked ({-1, -1}), and one FLIPPED step for the trump, 6,575 ms in all at two seats (the
 *     figure anim_plan.h gives for the opening plan);
 *   - every card the bot draws is masked, every card I draw named.
 *
 *   ./build.sh builds it (out/run_test). Exits non-zero on the first failure. */
#include "game.h"
#include "view.h"
#include "client_table.h"
#include "anim_plan.h"
#include "bot_drive.h"
#include "strategy.h"
#include "../src/game_keep.h"
#include "../src/anim_run.h"
#include <stdio.h>
#include <string.h>

int bot_roster_find_by_strat(int strat) { return strat == STRAT_HANDWRITTEN_PROD || strat == STRATEGY_KEY_HUMAN ? 0 : -1; }
int handwritten_prod_strategy_choose(const Game *g, int seat, const LegalMoves *moves, void *ctx);
int bot_roster_choose(int idx, const Game *g, int seat, const LegalMoves *moves) {
  if (idx != 0 || !moves || moves->n <= 0) return -1;
  int chosen = handwritten_prod_strategy_choose(g, seat, moves, 0);
  return chosen >= 0 && chosen < moves->n ? chosen : -1;
}

static Game G;
static ClientSlot SLOT;
static ClientTable CT;
static RunStep steps[ANIM_MAX_STEPS];
static AnimPlanAsk ask;
static AnimPlan plan;

/* What the table draws of a board, but for the roles: the cards (my hand, the battles, the trump),
 * the counts (the stock, the discard pile, each seat's hand), the seats' statuses, the goods and how
 * the game stands. */
static const char *drawn_diff(const TableView *a, const TableView *b) {
  if (a->status != b->status) return "status";
  if (a->num_players != b->num_players || a->power_suit != b->power_suit) return "the seats or the trump suit";
  if (a->num_battles != b->num_battles || memcmp(a->battles, b->battles, sizeof a->battles[0] * (size_t)a->num_battles)) return "the battles";
  if (a->my_hand_count != b->my_hand_count || memcmp(a->my_hand, b->my_hand, sizeof a->my_hand[0] * (size_t)a->my_hand_count)) return "my hand";
  if (a->deck_count != b->deck_count) return "the stock";
  if (a->discard_pile_length != b->discard_pile_length) return "the discard pile";
  if (a->has_flipped != b->has_flipped || (a->has_flipped && memcmp(&a->flipped, &b->flipped, sizeof a->flipped))) return "the trump";
  if (a->good_mask != b->good_mask) return "the goods";
  if (a->fool != b->fool || a->num_eliminated != b->num_eliminated
      || memcmp(a->elimination, b->elimination, sizeof a->elimination[0] * (size_t)a->num_eliminated)) return "the finish";
  for (int i = 0; i < a->num_players; i++)
    if (a->seats[i].hand_count != b->seats[i].hand_count || a->seats[i].status != b->seats[i].status) return "a seat";
  return 0;
}

/* The checks every operation gets. */
static int check_op(const char *what, int seed, int op, int n) {
  if (n < 0) { printf("FAIL seed %d op %d %s: the stream was not written or read\n", seed, op, what); return 0; }
  if (n == 0) return 1; /* an operation nobody is shown (a good that ends nothing): no steps, the board as it was */
  if (client_adopt_board(&CT, &G, 0) != CLIENT_OK) { printf("FAIL seed %d op %d: the board does not adopt\n", seed, op); return 0; }
  const TableView *fin = run_capture_final();
  const char *d = drawn_diff(fin, &CT.view);
  if (d || fin->defender != CT.view.defender || fin->first_attacker != CT.view.first_attacker) {
    printf("FAIL seed %d op %d %s: the stream's final board is not the board the table adopts (%s)\n", seed, op, what, d ? d : "the roles");
    return 0;
  }
  /* the last step's board draws as the final one but for the roles: a transfer's shield and a
   * bout's rotation and the goods it clears are written after their last event (the closing beat,
   * anim_plan.h "the role beat"), and so are flags no region draws (a seat's awaiting flag) */
  TableView last = steps[n - 1].board;
  last.good_mask = CT.view.good_mask; /* the goods are a role too */
  if ((d = drawn_diff(&last, &CT.view))) {
    printf("FAIL seed %d op %d %s: the last step's board draws otherwise than the final one (%s; last step type %d)\n", seed, op, what, d, steps[n - 1].ev.type);
    return 0;
  }
  int types[ANIM_MAX_STEPS], seats[ANIM_MAX_STEPS];
  for (int i = 0; i < n; i++) {
    const PushEvent *e = &steps[i].ev;
    ClientPlanStep s;
    memset(&s, 0, sizeof s);
    s.type = e->type; s.seat = e->seat; s.from = (uint8_t)e->from; s.to = (uint8_t)e->to; s.n_cards = e->n_cards;
    for (int k = 0; k < e->n_cards; k++) s.cards[k] = e->cards[k];
    if (client_plan_step(&ask, i, &s, &steps[i].board) != ANIM_EOK) { printf("FAIL seed %d op %d %s: step %d refused\n", seed, op, what, i); return 0; }
    types[i] = e->type;
    seats[i] = e->seat;
    /* a draw from the stock: mine named, the bot's masked */
    if (e->type == EVW_T_DEAL || e->type == EVW_T_REFILL)
      for (int k = 0; k < e->n_cards; k++)
        if ((e->cards[k].suit < 0) != (e->seat != 0)) {
          printf("FAIL seed %d op %d %s: seat %d draws %d-%d\n", seed, op, what, e->seat, e->cards[k].suit, e->cards[k].value);
          return 0;
        }
  }
  if (client_plan(&ask, n, &CT.view, &plan) != ANIM_EOK) { printf("FAIL seed %d op %d %s: no plan\n", seed, op, what); return 0; }
  int ms = anim_stream_ms(types, seats, n);
  if (plan.total_ms != ms || plan.n_steps != n) {
    printf("FAIL seed %d op %d %s: the plan lasts %d ms over %d steps, the stream %d ms over %d\n", seed, op, what, plan.total_ms, plan.n_steps, ms, n);
    return 0;
  }
  return 1;
}

int main(void) {
  client_init(&CT, &SLOT);
  const int8_t keys[2] = {STRATEGY_KEY_HUMAN, STRAT_HANDWRITTEN_PROD};
  int games = 0, ops = 0, deals = 0, flown = 0, silent = 0;
  for (int seed = 1; seed <= 200; seed++) {
    memset(&G, 0, sizeof G);
    game_reset_to_lobby(&G, 0);
    game_set_seed((uint32_t)seed * 2654435761u | 1);
    run_capture_begin(&G);
    game_seat_and_deal(&G, keys, 2);
    int n = run_capture_end(&G, 0, -1, 0, steps, ANIM_MAX_STEPS);
    if (!check_op("the deal", seed, 0, n)) return 1;
    int dealt = 0, first_deal = -1;
    for (int i = 0; i < n; i++)
      if (steps[i].ev.type == EVW_T_DEAL) { dealt++; if (first_deal < 0) first_deal = i; }
    if (dealt != 12 || plan.steps[first_deal].duration_ms != ANIM_DEAL_CARD_MS || plan.total_ms != 6575) {
      printf("FAIL seed %d: the deal is %d DEAL steps, the first %d ms, the whole %d ms\n", seed, dealt,
             first_deal < 0 ? -1 : plan.steps[first_deal].duration_ms, plan.total_ms);
      return 1;
    }
    int flipped = 0;
    for (int i = 0; i < n; i++) flipped += steps[i].ev.type == EVW_T_FLIPPED;
    if (flipped != 1) { printf("FAIL seed %d: %d FLIPPED steps in the deal\n", seed, flipped); return 1; }
    deals++;
    game_keep_canonical(&G);
    for (int op = 1; op < 2000 && G.status == GAME_STATUS_PLAYING; op++) {
      uint32_t s = (uint32_t)(seed * 7919 + op) | 1;
      uint32_t mask = bot_drive_eligible_mask(&G, 1u) ? 1u : 2u; /* the bot's step, or "mine" */
      game_set_seed(s);
      random_strategy_set_seed(s);
      BotDriveOut out;
      run_capture_begin(&G);
      bot_drive(&G, mask, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);
      int actor = out.n ? out.actions[out.n - 1].seat : -1;
      n = run_capture_end(&G, 0, actor, G.status == GAME_STATUS_GAME_OVER, steps, ANIM_MAX_STEPS);
      if (out.n == 0) continue; /* nothing applied: no stream to check */
      if (!check_op("a step", seed, op, n)) return 1;
      game_keep_canonical(&G);
      ops++;
      silent += n == 0;
      flown += n;
    }
    if (G.status != GAME_STATUS_GAME_OVER) { printf("FAIL seed %d: no end\n", seed); return 1; }
    games++;
  }
  printf("run_test: %d games, %d deals and %d operations (%d of them shown nothing), %d steps; every run ends on the table's board and plays as long as the kernel's stream\n",
         games, deals, ops, silent, flown);
  if (silent * 4 > ops) { printf("FAIL: %d of %d operations showed nothing\n", silent, ops); return 1; }
  return 0;
}
