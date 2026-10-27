// board.h - WHAT A TABLE LOOKS LIKE FROM ONE CLIENT'S VIEW, as the site's row (wire.h be_w_table):
// the one place a kernel TableView becomes the row's board, seats and phase, and a push step
// becomes the feed's event kind. Both the mixed sim's reporting seat (sim.c) and the league's
// spectator (league.c) write the world through it.
//
// And THE SERVER'S BOTS as GET /v1/bots names them: every one, read once at start, so a seat that
// is a bot shows its strategy key and the league can seat any of them.
#ifndef BE_BOARD_H
#define BE_BOARD_H

#include "client_table.h"
#include "legal.h"
#include "netwire.h"
#include "table.h"
#include "world.h"

#include <stdbool.h>
#include <stdint.h>

// The row's board from view `v` (the world locked by the caller): seats, battles, trump, stock, the
// fool, the turn, the phase (a GONE row keeps its phase), then be_world_changed. `n_seats` is the
// seats planned, `moves` the moves the row counts.
void be_board_fill(be_world *w, be_table *bt, const TableView *v, int n_seats, uint32_t moves);

// The feed's kind (BE_EV_*) of a push step, 0 for a step the feed does not show.
int be_board_event_kind(const PushEvent *e);

// A move the server did not apply, answered 200 with `r`, made on version `intent`: true when that
// is a race (the board moved while the move was on its way: a moot move, a stale round, or a
// refusal at a later version than the move was made on, a pass that made the mover the defender
// among them), false when the server refused the move on the very board it was made on (a finding).
bool be_move_raced(const TableResponse *r, uint32_t intent);

// A legal move for the seat that holds `ct` (a client table's masked board) into wire, as a fake
// human (sim.c) or a test's human plays one: the handwritten brain's choice, or with chance
// `random_pct` percent (drawn from *rng) a random legal move; each candidate is validated by
// client_validate before it is taken. Its length, or 0 when the seat has nothing to do now.
// `lm` is the caller's scratch.
int be_legal_wire(ClientTable *ct, LegalMoves *lm, uint64_t *rng, int random_pct, uint8_t *wire, int cap);

#define BE_BOTS_MAX 64
typedef struct {
    char id[NW_ID_MAX + 1];
    char key[NW_KEY_MAX + 1];
    char name[NW_NAME_MAX + 1];
} be_bot;

// GET /v1/bots on the game server at `port` with `token` (NULL: none): every bot, in the server's
// order. The count, or -1 when the list could not be read.
int be_bots_fetch(int port, const char *token);
int be_bots_n(void);
const be_bot *be_bots_at(int i);
// One more row after the server's bots (league.h: the human's account, key "human"): its index, or
// -1 when the list is full. A later be_bots_fetch drops it.
int be_bots_add(const char *id, const char *key, const char *name);
// The bot whose nickname is `name` (a seat's name), or -1.
int be_bots_by_name(const char *name);
// The bot whose id is `id`, or -1.
int be_bots_by_id(const char *id, int id_len);

#endif
