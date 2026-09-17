// client_table.h - the web client's slot: what a player's screen is built from.
//
// A client never holds a whole game. It receives bytes a server wrote for it -
// a response envelope (table.h table_envelope: a player_views or spectator_views
// row, a create or meta response) and realtime pushes (evwire.h as2 / as3) - and
// reads them here into a TableView: one viewer's masked board joined to the
// table's roster, with seats by index and the viewer's own seat named by the
// server (docs/C_GAME_SHAPE_MIGRATION.md 2.6, 2.9). A host copies a TableView
// out through generated snapshot readers (sdk/ts/gen/view_layout.bots.ts) and
// knows no byte of any of these layouts.
//
// REFUSE, NEVER CLAMP. Everything here arrives off the network. A reader that
// clamped a count or a card into range would render a board nobody sent, so any
// payload that does not read whole - a missing or truncated roster trailer, a
// count over its capacity, a card byte that is not a card, bytes nobody
// announced - is refused as a whole, and the TableView of a refused call is not
// to be read.
//
// ONE SECTION. The slot, the view and an open push are module storage: a host
// adopts, reads the snapshot, and is done before anything else may touch the
// kernel. A push is iterated in one synchronous loop over bytes that stay where
// the host wrote them.
//
// IDENTITY. A move's push carries no roster (a move cannot change one), so a
// client keeps each table's identity - its game id, title and seats, as the
// roster trailer's bytes (roster.h) - from the last envelope or roster-carrying
// push it adopted, and hands it back to decode the next push. Those bytes are
// opaque to the host. A push decoded without identity names no one: its seats
// have empty ids and names.
#ifndef CNITRO_CLIENT_TABLE_H
#define CNITRO_CLIENT_TABLE_H

#include "game.h"
#include "roster.h"
#include <stddef.h>

#define CLIENT_OK           0
#define CLIENT_E_FORMAT   (-201)  // not an envelope this build reads: format, flags, seat byte or lengths
#define CLIENT_E_TRAILER  (-202)  // the roster trailer is missing, short, or refused (detail: ROSTER_E_*)
#define CLIENT_E_STATE    (-203)  // a masked board does not read whole or is refused (detail: GAME_INVALID_*)
#define CLIENT_E_MISMATCH (-204)  // the roster's seats are not the board's, or the viewer is not a seat
#define CLIENT_E_PUSH     (-205)  // a push that does not read whole, or an event that is not one
#define CLIENT_E_IDENTITY (-206)  // identity bytes refused (detail: ROSTER_E_*)
#define CLIENT_E_ORDER    (-207)  // push_next / push_final with no push open
#define CLIENT_E_CAP      (-208)  // an output buffer is too small
#define CLIENT_E_MOVE     (-209)  // an action wire that is not a move, or a hand order that is not one

// One seat as the viewer sees it. `hand_count` is every seat's; the cards are
// the viewer's own only (TableView.my_hand).
typedef struct {
    int8_t  status;           // PLAYER_STATUS_*
    int8_t  hand_count;
    bool    awaiting_attack;  // meaningful for the viewer's own seat only
    bool    is_ai;
    uint8_t id_len, name_len;
    char    id[ROSTER_ID_MAX + 1];
    char    name[ROSTER_NAME_MAX + 1];
} ViewSeat;

typedef struct {
    int8_t   status;          // GAME_STATUS_*: an envelope's roster says it, a push step's board does
    int8_t   num_players;
    int8_t   power_suit, first_attacker, defender;
    int8_t   num_battles;
    int8_t   num_eliminated;
    int8_t   my_seat;         // -1: a spectator
    int8_t   my_hand_count;
    int8_t   fool;            // the fool's seat once the game is over, else -1
    int16_t  deck_count, discard_pile_length;
    bool     has_flipped;
    bool     has_good_timestamp;
    Card     flipped;
    uint8_t  gid_len, title_len;
    uint32_t good_mask;       // bit per seat
    uint32_t version;         // the committed version this view is of
    Battle   battles[MAX_BATTLES];        // defense CARD_NONE when uncovered
    ViewSeat seats[MAX_PLAYERS];
    Card     my_hand[MAX_HAND_SIZE];
    int8_t   elimination[MAX_PLAYERS];    // seats, in the order they went out
    char     game_id[ROSTER_GAME_ID_MAX + 1];
    char     title[ROSTER_TITLE_MAX + 1];
} TableView;

// One animation step of a push (evwire.h), before its board.
typedef struct {
    int8_t  type;         // EVW_T_*
    int8_t  seat;         // -1 none
    int8_t  msg;          // EVW_MSG_*
    int8_t  from, to;     // EVW_LOC_*, -1 none
    int8_t  battle;       // -1 none
    uint8_t n_cards;
    bool    has_target;
    Card    target;       // the attack card a cover covers
    Card    cards[MAX_HAND_SIZE];         // a card dealt or drawn to another seat is {-1, -1}
} PushEvent;

// What a board shows that is a rule of the game rather than a field of it: the
// seats the sword and the shield mark, whether the viewer is offered Good, and
// what the stock shows while cards fly out of it. A screen draws these instead
// of deciding them (docs/C_GAME_SHAPE_MIGRATION.md Phase 6a).
typedef struct {
    int8_t  first_attacker_badge;  // the seat that leads the next bout, marked on an empty table once dealt; -1 none
    int8_t  defender_badge;        // the defending seat, marked once dealt; -1 none
    bool    can_say_good;          // the viewer may say Good and the bout could close on it
    bool    show_deck_pile;        // the stock has cards left to draw on screen
    bool    show_flipped_slot;     // the trump's slot, kept while a card is on its way into it
    bool    show_trump_icon;       // stock and trump are gone: the power suit stands in their place
    bool    bot_to_move;           // a bot seat may act (should_bot_act's rule): a stalled bot loop is worth a nudge
    int16_t deck_pile;             // cards drawn in the stock pile
    int16_t deck_badge;            // the count on the pile: the stock, the trump, and cards in flight to the trump
} ViewRules;

// An edit a client makes to a board it holds (client_board_edit): each is a
// board the screen shows that no server wrote. See client_board_edit.
#define CLIENT_EDIT_KEEP   1  // my pending cards stand on the table: over `target`, or as attacks
#define CLIENT_EDIT_TURN   2  // the lead and the shield my pending pass moved: first_attacker, defender
#define CLIENT_EDIT_TABLE  3  // the table is `cards`, as uncovered attacks
#define CLIENT_EDIT_LIFT   4  // every battle holding one of `cards` leaves the table
#define CLIENT_EDIT_RETURN 5  // `cards` back into my hand, each that is not there already
#define CLIENT_EDIT_LOBBY  6  // the rematch's lobby (game_reset_to_lobby), before its reset arrives
#define CLIENT_EDIT_WITHDRAW 7  // a refused move's cards off the table and back in my hand

typedef struct {
    int8_t  op;              // CLIENT_EDIT_*
    int8_t  first_attacker;  // TURN
    int8_t  defender;        // TURN
    uint8_t n_cards;
    Card    target;          // KEEP: the attack every card covers, CARD_NONE for attacks
    Card    cards[MAX_HAND_SIZE];
} BoardEdit;

// The conflict verdict's question about my pending motions, asked of a push's
// boards (client_conflict_verdicts). The events and motions are the host's -
// the push's events it has not already played, the cards it has in flight -
// and the boards say the rest.
#define CLIENT_CONFLICT_MAX_EVENTS 128   // == ANIM_MAX_STEPS
#define CLIENT_CONFLICT_MAX_MOTIONS 64

typedef struct {
    int8_t  type;       // ANIM_EVT_*
    bool    masked;     // viewer-masked backs: they name nothing
    uint8_t n_cards;
    Card    cards[MAX_HAND_SIZE];   // a card that is not one (a back) names nothing
} ConflictEvent;

typedef struct {
    Card    card;       // CARD_NONE: a masked back
    int8_t  dest;       // ANIM_DEST_*
    bool    is_cover;
} ConflictMotion;

typedef struct {
    int8_t  defender_seat;      // whose hand on the final board bounds my attacks; -1: none (a hand of 0)
    bool    uncovered_on_final; // count the uncovered attacks on the final board, else on the open one
    int8_t  pending_attacks;    // my unconfirmed attacks; -1: the motions that are not covers
    uint8_t n_events;
    uint8_t n_motions;
    ConflictEvent  events[CLIENT_CONFLICT_MAX_EVENTS];
    ConflictMotion motions[CLIENT_CONFLICT_MAX_MOTIONS];
} ClientConflict;

typedef struct {
    uint8_t n;
    uint8_t verdicts[CLIENT_CONFLICT_MAX_MOTIONS];   // ANIM_CONFLICT_* per motion
} ConflictVerdicts;

// A board as the rules read it: a masked Game with room for the one log record a
// dry run writes (log_cap 1), so a move can be judged on it.
typedef struct { _Alignas(8) unsigned char bytes[offsetof(Game, logs) + sizeof(GameLog)]; } ClientRulesSlot;

typedef struct {
    Game     *g;          // the slot: a masked board, prefix storage (offsetof(Game, logs))
    Roster    r;
    uint32_t  ai_mask;
    bool      has_roster;
    uint8_t   gid_len;
    char      gid[ROSTER_GAME_ID_MAX + 1];
    int32_t   detail;
    TableView view;
    PushEvent event;
    // The open push: its bytes stay where the host put them until push_final.
    const uint8_t *push;
    const uint8_t *final;
    int32_t   final_len, at, index, n_events, viewer, n_seats;
    uint32_t  version;
    int32_t   identity_at;  // where in the last read input its roster trailer began, -1 for none
    bool      open;
    ClientRulesSlot rules;  // scratch: the board a gate, a rotation or a reset reads as a game
} ClientTable;

// The slot's storage: a Game prefix, which is all a masked import writes.
typedef struct { _Alignas(8) unsigned char bytes[offsetof(Game, logs)]; } ClientSlot;

void client_init(ClientTable *c, ClientSlot *slot);

// A response envelope (table.h table_envelope) into the view and the slot. The
// view's status is the roster trailer's (the row's column); its seats, game id
// and title are the trailer's, which becomes the identity.
int client_adopt_envelope(ClientTable *c, const uint8_t *p, int len);

// A board the module already holds - the game an FMSG decode adopted (the
// iMessage /m/ page) - as `viewer` (a seat, or -1) sees it, masked by state_put
// and read back like any board off the wire. The view names no one.
int client_adopt_board(ClientTable *c, const Game *g, int viewer);

// Opens a push for iteration. `as3`: the payload carries the flags byte and
// maybe a roster trailer (evwire.h); otherwise it is an as2 sequence alone, or
// the as3 push whole (a server since Phase 4b labels its as3 pushes as2 until
// Phase 5b), and then the bytes after the sequence must be exactly an as3 block.
// The roster comes from the push when it carries one, else from `identity`
// (bytes client_identity wrote, len 0 for none). `version` is the realtime
// message's committed version. The whole push is checked here, every event and
// every board, so a push that opens reads to its end.
int client_push_open(ClientTable *c, const uint8_t *p, int len, int as3,
                     const uint8_t *identity, int identity_len, uint32_t version);

// The next step into ClientTable.event and its board into the view: 1, or 0
// when every step has been read, or a refusal.
int client_push_next(ClientTable *c);

// The push's committed board into the view, and the push is closed: CLIENT_OK
// or a refusal.
int client_push_final(ClientTable *c);

// The identity to keep for this table (the roster trailer layout): its length,
// 0 when the slot holds no roster, or CLIENT_E_CAP.
int client_identity(const ClientTable *c, uint8_t *out, int cap);

// Where the identity to keep already is: the offset of the roster trailer in the
// envelope or push the last read took its roster from, to the end of that input
// (the bytes client_identity would write, give or take the goods and status it
// does not read). -1 when the roster did not come from the input read.
int client_identity_at(const ClientTable *c);

// Identity from parts, for a push whose roster arrived as JSON beside it (the
// as2 lobby broadcasts of servers before Phase 5b): a table id and title, then
// one call per seat. The slot's roster is replaced; client_identity then writes
// it. Names are trimmed like every roster name.
int client_identity_begin(ClientTable *c, const char *gid, int gid_len, const char *title, int title_len);
int client_identity_seat(ClientTable *c, const char *id, int id_len, const char *name, int name_len, int is_ai);

// The display rules of a view (ViewRules). The view is any board a screen holds,
// not only the slot's: one a host changed (an optimistic move, a board between
// two animation steps, a replay's board before its deal), written back through
// the generated writer. `from_deck` cards are in flight out of the stock, and
// `to_flipped` of them are on their way to the trump's slot. CLIENT_OK, or
// CLIENT_E_FORMAT for a view that is not one: a count past its capacity, a
// viewer that is not a seat, a negative flight.
int client_view_rules(const TableView *v, int from_deck, int to_flipped, ViewRules *out);

// ---------- the boards a client makes (docs/C_GAME_SHAPE_MIGRATION.md Phase 6b) ----------
//
// Not every board a screen holds is a read of bytes a server wrote. A move the
// viewer made stands on the board before the server confirms it; a push's boards
// keep the viewer's still-pending cards on them; a card flying home lands on the
// board it left; the rematch's lobby shows before its reset arrives. Each of
// those boards is made here, from a board the host holds written back through
// the generated writer, in place - so a host holds boards and decides when to
// show them, and never what they are.
//
// A board is read as the rules read it by importing it as a masked game: the
// viewer's hand is real, every card the viewer cannot see (the stock, the other
// hands) is a placeholder that is counted and never named, and the whole is
// judged as a masked import is (game_validate, GAME_VALIDATE_MASKED). The rules
// never name another seat's card, so a gate on that game is the server's verdict.

// The viewer's move on `v`, dry-run by the engine (awire_apply on the board as a
// game, nothing kept): 0 legal, the ENGINE_REJECT_* that refuses it,
// CLIENT_E_MOVE for a wire that is not a move, or the GAME_INVALID_* the board
// itself is refused for. A spectator's move is judged for seat 0.
int client_validate(ClientTable *c, const TableView *v, const uint8_t *awire, int len);

// The board the viewer's move leaves until the server says otherwise, from the
// move's own wire. It is a prediction of what the viewer will see, not a
// judgement: the host applies a move it judged (client_validate) when it made
// it, and the seats' hand counts stay the server's.
//   attack, pass: the cards join the table as attacks and leave the hand; a pass
//     also hands the shield to the next seat in play (get_next_player_index).
//   cover: each battle whose attack the wire names takes the cover paired with it,
//     and the covers leave the hand.
//   pickup: the table joins the hand, each attack before its cover, and the lead
//     and the shield move on as the server's rotation will (get_next_player_index
//     twice) - unless the refill after it could put a seat out (another seat in
//     play holds no card), which would move them somewhere the board cannot know.
//   good: nothing moves.
// A move the board already shows - its confirmation, or a push that kept its
// cards, landed first - leaves the board as it is: a card the table holds is not
// laid twice, a pass with no card to lay hands the shield on no further, and a
// pickup of an empty table moves no turn.
// CLIENT_OK, CLIENT_E_MOVE, CLIENT_E_FORMAT for a view that is not one,
// CLIENT_E_STATE when the rotation's board is refused (detail: GAME_INVALID_*),
// CLIENT_E_CAP for a table or a hand past its capacity.
int client_optimistic_apply(ClientTable *c, TableView *v, const uint8_t *awire, int len);

// The board after `e` (CLIENT_EDIT_*):
//   KEEP    each card, in order, unless the table already holds it on either side:
//           as the cover of the battle whose attack is `target` while that battle
//           is uncovered, or as a new attack when `target` is CARD_NONE; and out of
//           the viewer's hand either way. A spectator's board is left as it is.
//   TURN    the lead and the shield are `first_attacker` and `defender`.
//   TABLE   the table is `cards`, each an uncovered attack.
//   LIFT    every battle whose attack or cover is one of `cards` leaves the table.
//   RETURN  each of `cards` not in a seated viewer's hand is appended to it.
//   LOBBY   game_reset_to_lobby, the seats the roster marks bots coming back ready.
//   WITHDRAW each of `cards` leaves the table - an attack with its battle, a cover
//           from over its attack - and is in a seated viewer's hand once.
// CLIENT_OK, CLIENT_E_FORMAT for a view or an edit that is not one, CLIENT_E_STATE
// when the lobby's board is refused (detail: GAME_INVALID_*), CLIENT_E_CAP.
int client_board_edit(ClientTable *c, TableView *v, const BoardEdit *e);

// The viewer's hand in the order `idx` gives (game_rearrange_hand): CLIENT_OK,
// CLIENT_E_MISMATCH when the viewer holds no hand to order (a spectator, an empty
// hand), CLIENT_E_MOVE when `idx` is not a permutation of the hand, CLIENT_E_STATE
// when the board is refused.
int client_rearrange_hand(ClientTable *c, TableView *v, const uint8_t *idx, int n);

// The conflict verdict for each of `q`'s motions against a push (anim_plan.h
// anim_conflict_sweep, anim_conflict_facts, anim_conflict_verdict; the server
// transport): the push's events are what it moves; `open` - the push's last
// board - is where its cards stand, on the table and in the viewer's hand; the
// server's hope reads the hand of `q->defender_seat` on `final` and the uncovered
// attacks of `final` or `open`. Fills `out`; the motion count, or
// CLIENT_E_FORMAT for a view or a question that is not one, or the negative
// ANIM_E* the rule returns.
int client_conflict_verdicts(const TableView *open, const TableView *final, const ClientConflict *q, ConflictVerdicts *out);

#endif
