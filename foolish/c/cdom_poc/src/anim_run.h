/* anim_run.h - what an operation on the table's game shows, as the kernel says it (README, Animations).
 *
 * The React table animates a server's push: the kernel's event stream for its seat (evwire), read
 * back step by step through the client slot (client_push_open, client_push_next), each step what
 * moved and the board it left. The server writes that stream from the engine's hook snapshots
 * (table.c table_push: evwire_serialize over the operation's snapshots and logs). The port is its
 * own server, so it does the same two halves with the same kernel calls: the snapshots captured
 * while an operation runs (a move, a bot step, a deal), written for my seat, and read back as the
 * web reads them. Nothing here decides what moved, where, or in what order.
 *
 * Linked by the table and by the native tests/run_test.c. */
#ifndef ANIM_RUN_H
#define ANIM_RUN_H
#include "game.h"
#include "client_table.h"

/* One step of an operation: what moved (the push's event: type, seat, from, to, cards, the attack
 * a cover covers and its battle), and the board it left, as my seat sees it. */
typedef struct {
  PushEvent ev;
  TableView board;
} RunStep;

/* Opens an operation on g: from now until run_capture_end the engine's hook snapshots are kept. */
void run_capture_begin(const Game *g);

/* Closes it: the operation's stream for `viewer` (the acting seat `actor`, -1 for none; `ended`
 * when it finished the game, which adds the closing transition as table_push does), read back into
 * steps[0, cap). Returns the step count, or -1 when the stream could not be written or read. */
int run_capture_end(const Game *g, int viewer, int actor, int ended, RunStep *steps, int cap);

/* The board the last stream read settles on (its trailer: the committed board, which a run commits
 * once its last step has landed). It is not always the last step's: a transfer is snapshotted
 * before the shield is handed over and writes no event for it (anim_plan.h anim_pass_hand_off). */
const TableView *run_capture_final(void);

#endif
