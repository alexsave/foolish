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
    int16_t deck_pile;             // cards drawn in the stock pile
    int16_t deck_badge;            // the count on the pile: the stock, the trump, and cards in flight to the trump
} ViewRules;

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

#endif
