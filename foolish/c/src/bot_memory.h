// BotMemory: what the belief bots remember of a game, folded one record at a
// time.
//
// Every belief brain (robusta and its rollout family, blackpowder, cordite,
// octogen and fusion, espresso_prod) used to rebuild its picture of the other
// hands by scanning the whole session log on every decision. On the server
// that meant a bot cycle read the session log back out of the store, replayed
// it into the Game's logs[], and then every brain walked it. This is that walk
// made incremental: ONE block per table, folded forward by bot_mem_fold as each
// record lands, and read by the brains in place of the log.
//
// ONE BLOCK, NOT ONE PER SEAT. Everything a brain derives from the log is
// public: draws are masked (view.c log_record_put) except the face-up trump,
// and every other record is a card the whole table saw. The only thing a scan
// did with its own seat was to skip it, so the block tracks every seat and the
// reader masks its own row (bot_mem_belief). It holds nothing from any other
// game: a LOG_GAME_START resets it.
//
// RAW STATE, READ-TIME RULES. A scan finished with rules that look at the
// board as it is NOW: the face-up trump's holder is the last drawer only once
// the stock is empty; a trump lead counts against a seat only while the deck
// was alive, which for the records after the last draw depends on whether a
// draw is still to come; out seats hold nothing. The block keeps what those
// rules need (a confirmed and a pending count either side of the last draw,
// the last drawer's row both with and without the trump) and the readers apply
// the rules against the board they are handed. So the fold needs no lookahead,
// and the same fold builds the block a record at a time (the server) or over a
// whole log at once (a host that holds the Game, the arena, the tests).
//
// ORDER IS KEPT. A seat's pinned cards are a list, not a set: the order is the
// hand slots a sampled world deals them into, so a set would change moves. The
// lists add and remove exactly as the scans did (bot_common.h bot_pin_add /
// bot_pin_remove), which makes the beliefs the brains read bit-identical to
// the scans' (tests/bot_memory_test.c holds them to it).
#ifndef CNITRO_BOT_MEMORY_H
#define CNITRO_BOT_MEMORY_H

#include "game.h"
#include "card.h"
#include <stdbool.h>
#include <stdint.h>

#define BOT_MEM_VOIDS   6                 // bot_common.h BOT_MAX_VOIDS
#define BOT_MEM_ROW     MAX_HAND_SIZE     // a pinned row's room (bot_pin_add's cap)
#define BOT_MEM_TABLE   80                // the replayed table's room (the scans' tbl[80])
#define BOT_MEM_UNC     (MAX_BATTLES * 2) // the uncovered attacks' room (the scans' unc[])

// Per-seat flag bits (BotMemory.flags).
#define BOT_MEM_FLOOR_BROKEN 0x01   // a play under the seat's floor (bot_floor_check): distrust its floors
#define BOT_MEM_TELL         0x02   // octogen's mc_tell: a void or a declined value contradicted
#define BOT_MEM_ALT_TELL     0x04   // the same, as the last drawer's row with the trump pinned saw it

typedef struct {
    // Records folded since the deal (LOG_GAME_START counts): the progress term
    // every decision seed folds in (bot_mem_records), which is what the scans'
    // g->num_logs was while the log fit in MAX_LOGS.
    uint32_t n_records;
    int8_t   num_players;     // the seats the fold tracks, from the deal
    int8_t   in_now;          // seats not yet out (LOG_PLAYER_OUT)
    int8_t   cur_def;         // the defender as octogen's scan followed it, -1 before any
    int8_t   flip_holder;     // the last seat to draw, -1 for none
    // The current bout, replayed from the per-move records (a PICKUP or DISCARD
    // record's card list truncates at MAX_LOG_PAIRS, the moves never do).
    uint8_t  tbl_n, unc_n;
    Card     tbl[BOT_MEM_TABLE];  // every card on the table, in the order played
    Card     unc[BOT_MEM_UNC];    // the attacks still uncovered
    uint64_t discard;             // every card discarded this game, by card id
    // Per seat. Rows are kept for every seat; a reader skips its own.
    uint8_t  pin_n[MAX_PLAYERS];
    Card     pin[MAX_PLAYERS][BOT_MEM_ROW];   // publicly located in the seat's hand, in order
    uint8_t  void_n[MAX_PLAYERS];
    Card     voids[MAX_PLAYERS][BOT_MEM_VOIDS];
    uint8_t  floor_v[MAX_PLAYERS];            // a lowest-first attacker's floor, 0 for none
    uint8_t  flags[MAX_PLAYERS];              // BOT_MEM_*
    uint16_t decl[MAX_PLAYERS];               // values a seat said good over (octogen's tell)
    int16_t  hand_n[MAX_PLAYERS];             // the seat's hand size as the log counts it
    // Deck-alive counts, split at the most recent draw: `conf` is every play
    // before it (the deck was alive for those), `pend` every play since (alive
    // only if a draw is still to come, which the reader asks the board).
    uint16_t viol_conf[MAX_PLAYERS], viol_pend[MAX_PLAYERS];   // trump leads
    uint16_t cp_conf[MAX_PLAYERS], cp_pend[MAX_PLAYERS];       // cards played
    uint16_t tp_conf[MAX_PLAYERS], tp_pend[MAX_PLAYERS];       // of those, trumps
    // The last drawer's row as it would be had that draw taken the face-up
    // trump: what the row is once the stock reads empty.
    uint8_t  alt_n;
    Card     alt[BOT_MEM_ROW];
} BotMemory;

// An empty block: no game, nothing seen.
void bot_mem_clear(BotMemory *m);

// Fold one record, in the masked form the session log stores (view.c
// log_record_put: a draw's cards hidden but the face-up trump's). `g` is the
// game the record is from, for its constants only (num_players, power_suit),
// which a game keeps from its deal to its end. A LOG_GAME_START resets the
// block first.
void bot_mem_fold(BotMemory *m, const GameLog *rec, const Game *g);

// One record as the session log STORES it, folded: the record is masked first
// by the one rule the store's writer applies (view.c log_record_put, with the
// operation's face-up trump before it began), and read back as the log reader
// reads it. What the server's table folds, and what the tests fold.
void bot_mem_fold_masked(BotMemory *m, const GameLog *rec, const Game *g, bool pre_has_flip, Card pre_flip);
// That masking alone: rec as the session log stores it, into *out.
void bot_mem_mask(const GameLog *rec, const Game *g, bool pre_has_flip, Card pre_flip, GameLog *out);

// The whole of g's log, folded into a cleared block (a host that holds the
// Game: the arena, the offline apps, the tests).
void bot_mem_from_logs(BotMemory *m, const Game *g);

// THE BLOCK A BRAIN READS for g: the one bound to g on this thread
// (bot_mem_bind), else g's own log folded into `scratch`. The binding is by
// pointer, so a brain handed any other Game (a sampled world, an arena's copy)
// folds that Game's own log as before.
const BotMemory *bot_mem_for(const Game *g, BotMemory *scratch);
// The same into this thread's own scratch block (valid until the next call).
const BotMemory *bot_mem_of(const Game *g);
// Bind `m` as g's block on this thread (NULL unbinds). The server's table binds
// the table's block around a bot drive (table.c).
void bot_mem_bind(const Game *g, const BotMemory *m);
const BotMemory *bot_mem_bound(const Game *g);

// The progress term of every decision seed: the bound block's record count,
// else g->num_logs (which is what a fold of g's log counts).
uint32_t bot_mem_records(const Game *g);

// The deck is alive: cards are still to be drawn (the scans' deck_alive_now).
static inline bool bot_mem_deck_alive(const Game *g) { return g->deck_count > 0 || g->has_flipped; }
// The face-up trump's holder, or -1: the last drawer once the stock is empty.
static inline int bot_mem_flip_seat(const BotMemory *m, const Game *g) {
    return bot_mem_deck_alive(g) ? -1 : m->flip_holder;
}
// Seat p's pinned row as a reader sees it now: with the trump, when `flip` and
// p holds it (bot_mem_flip_seat).
static inline const Card *bot_mem_row(const BotMemory *m, const Game *g, int p, bool flip, int *n) {
    if (flip && p == bot_mem_flip_seat(m, g)) { *n = m->alt_n; return m->alt; }
    *n = m->pin_n[p];
    return m->pin[p];
}
// A deck-alive count as the scans counted it (confirmed, plus pending while the
// deck is still alive).
static inline int bot_mem_alive(const Game *g, int conf, int pend) {
    return conf + (bot_mem_deck_alive(g) ? pend : 0);
}
// Seat p's trump leads while the deck was alive (the scans' trump_viol).
static inline int bot_mem_trump_leads(const BotMemory *m, const Game *g, int p) {
    return bot_mem_alive(g, m->viol_conf[p], m->viol_pend[p]);
}
// Seat p's mc_tell (octogen): as the row the reader sees saw it.
static inline bool bot_mem_tell(const BotMemory *m, const Game *g, int p, bool flip) {
    const int bit = flip && p == bot_mem_flip_seat(m, g) ? BOT_MEM_ALT_TELL : BOT_MEM_TELL;
    return (m->flags[p] & bit) != 0;
}
static inline bool bot_mem_discarded(const BotMemory *m, Card c) {
    const int id = card_in_range(c, 1, ACE_VALUE) ? card_to_id(c) : -1;
    return id >= 0 && id < 64 && (m->discard >> id & 1u);
}

// ---- the durable form ----------------------------------------------------
//
// Sparse: a seat's row costs its cards, an empty one a bit. The state blob's
// memory section (view.h) carries it.
#define BOT_MEM_BYTES_MAX 1400
// Writes m; returns the byte length (at most BOT_MEM_BYTES_MAX).
int bot_mem_encode(const BotMemory *m, uint8_t *out, int cap);
// Reads `len` bytes; false (m cleared) for bytes this kernel did not write.
bool bot_mem_decode(BotMemory *m, const uint8_t *p, int len);
// Equal blocks (encoding-equal: what a fold makes is canonical).
bool bot_mem_equal(const BotMemory *a, const BotMemory *b);

// ---- worlds ---------------------------------------------------------------
//
// A sampled world's log is its source's LOG_DISCARD records (bot_common.h
// bot_clone_world), which is what espresso's rollout discard memory reads. With
// a block bound to the source the records are synthesized from its discard set
// (the same cards, MAX_LOG_PAIRS to a record): writes them at dst->logs and
// returns how many.
int bot_mem_discard_logs(const BotMemory *m, GameLog *dst, int cap);

#endif
