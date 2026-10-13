// client_table.c - the web client's slot. See client_table.h.
#include "client_table.h"
#include "anim_plan.h"
#include "awire.h"
#include "evwire.h"
#include "view.h"
#include "../wasm/wire.h"
#include "../../../shared/c/le_bytes.h"
#include <string.h>

void client_init(ClientTable *c, ClientSlot *slot) {
    memset(c, 0, sizeof(*c));
    c->g = (Game *)(void *)slot->bytes;
}

// ---------- the view ------------------------------------------------------------

// The one seat the elimination order does not name, once the game is over (its
// seats are parked, so game_done cannot say); game_done while it runs.
static int fool_of(const Game *g) {
    if (g->status != GAME_STATUS_GAME_OVER) return game_done(g);
    uint32_t out = 0;
    for (int i = 0; i < g->num_eliminated; i++) out |= 1u << g->elimination_order[i];
    for (int s = 0; s < g->num_players; s++) if (!((out >> s) & 1u)) return s;
    return -1;
}

static void put_text(char *dst, uint8_t *dst_len, const char *src, int len) {
    memcpy(dst, src, (size_t)len);
    dst[len] = 0;
    *dst_len = (uint8_t)len;
}

// A view of a game that carries no rating stamp: the flag AND the numbers, so a
// slot that showed a rated game a moment ago hands back nothing of it. The
// snapshot is copied out whole, and a stale number beside a false flag is still
// a stale number in somebody's store.
static void view_unrate(TableView *v) {
    v->rated = false;
    v->rating_known = false;
    memset(v->rating_before, 0, sizeof(v->rating_before));
    memset(v->rating_delta, 0, sizeof(v->rating_delta));
    memset(v->rating_after, 0, sizeof(v->rating_after));
}

// A board's game fields, as `viewer` sees it: everything but who sits where, the
// table's name and the version. Only what the counts cover is ever read, so
// nothing past them is cleared.
static void view_board(TableView *v, const Game *g, int viewer, int status) {
    v->status = (int8_t)status;
    v->num_players = g->num_players;
    v->power_suit = g->power_suit;
    v->first_attacker = g->first_attacker;
    v->defender = g->defender;
    v->num_battles = g->num_battles;
    v->num_eliminated = g->num_eliminated;
    v->my_seat = (int8_t)viewer;
    // A stamp is a finished game's: a board edited out of one (the rematch's
    // lobby, CLIENT_EDIT_LOBBY) leaves it behind.
    if (status != GAME_STATUS_GAME_OVER) view_unrate(v);
    v->fool = (int8_t)fool_of(g);
    v->deck_count = g->deck_count;
    v->discard_pile_length = g->discard_pile_length;
    v->has_flipped = g->has_flipped;
    v->has_good_timestamp = g->has_good_timestamp;
    v->flipped = g->has_flipped ? g->flipped : CARD_NONE;
    v->good_mask = g->good_players_mask;
    memcpy(v->battles, g->table_battles, sizeof(Battle) * (size_t)g->num_battles);
    memcpy(v->elimination, g->elimination_order, (size_t)g->num_eliminated);
    for (int s = 0; s < g->num_players; s++) {
        ViewSeat *vs = &v->seats[s];
        vs->status = g->players[s].status;
        vs->hand_count = g->players[s].hand_count;
        vs->awaiting_attack = s == viewer && g->players[s].awaiting_attack;
    }
    v->my_hand_count = viewer >= 0 ? g->players[viewer].hand_count : 0;
    if (viewer >= 0) memcpy(v->my_hand, g->players[viewer].hand, (size_t)v->my_hand_count);
}

// The slot's board, as `viewer` sees it, joined to the roster when there is one.
static void view_fill(ClientTable *c, int viewer, int status) {
    const Game *g = c->g;
    TableView *v = &c->view;
    view_board(v, g, viewer, status);
    view_unrate(v);   // an envelope's stamp is set by the one reader of it (client_adopt_envelope)
    v->version = c->version;
    v->passing = (c->table_rules & GAME_RULE_NO_PASS) == 0;
    for (int s = 0; s < g->num_players; s++) {
        ViewSeat *vs = &v->seats[s];
        vs->is_ai = c->has_roster && ((c->ai_mask >> s) & 1u) != 0;
        if (c->has_roster) {
            put_text(vs->id, &vs->id_len, c->r.seats[s].id, c->r.seats[s].id_len);
            put_text(vs->name, &vs->name_len, c->r.seats[s].name, c->r.seats[s].name_len);
        } else {
            put_text(vs->id, &vs->id_len, "", 0);
            put_text(vs->name, &vs->name_len, "", 0);
        }
    }
    if (c->has_roster) {
        put_text(v->game_id, &v->gid_len, c->gid, c->gid_len);
        put_text(v->title, &v->title_len, c->r.title, c->r.title_len);
    } else {
        put_text(v->game_id, &v->gid_len, "", 0);
        put_text(v->title, &v->title_len, "", 0);
    }
}

// A masked board off the wire into the slot: read and judged exactly as
// state_import does (state_get, which measures `len` before it reads a field,
// then game_validate). The slot is not saved and put back on a refusal, as
// state_import would: after a refusal it is not read, and every read here would
// pay the copy.
static int board_import(ClientTable *c, const uint8_t *p, int len) {
    int v = state_get(c->g, p, len, 1);
    if (v == GAME_VALID) v = game_validate(c->g, GAME_VALIDATE_MASKED);
    if (v != GAME_VALID) { c->detail = v; return CLIENT_E_STATE; }
    return CLIENT_OK;
}

// A roster trailer that must fill exactly `len` bytes, read straight into the
// slot's roster (a refused read leaves the slot naming no one). CLIENT_OK, or
// `refusal` with the ROSTER_E_* in detail.
static int identity_read(ClientTable *c, const uint8_t *p, int len, int *status, int refusal) {
    int used = 0, gl = 0, passing = 1;
    c->has_roster = false;
    c->table_rules = 0;
    const int rc = roster_trailer_read(&c->r, c->gid, &gl, status, &c->ai_mask, &passing, p, len, &used);
    if (rc != ROSTER_OK) { c->detail = rc; return refusal; }
    if (used != len) { c->detail = ROSTER_E_PADDING; return refusal; }
    c->gid_len = (uint8_t)gl;
    c->gid[gl] = 0;
    c->table_rules = (int8_t)(passing ? 0 : GAME_RULE_NO_PASS);
    return CLIENT_OK;
}

// ---------- the envelope --------------------------------------------------------

int client_adopt_envelope(ClientTable *c, const uint8_t *p, int len) {
    c->open = false;
    c->detail = 0;
    // The header, the island skip and the view's bounds are view.h's, shared
    // with the writer (table.c table_envelope) so the two cannot drift.
    EnvHeader h;
    if (!env_header_read(p, len, &h)) return CLIENT_E_FORMAT;
    if (h.trailer_at >= len) return CLIENT_E_TRAILER;

    // The slot names the table only once the envelope has read whole.
    int rc, status = 0;
    c->identity_at = -1;
    if ((rc = identity_read(c, p + h.trailer_at, len - h.trailer_at, &status, CLIENT_E_TRAILER)) != CLIENT_OK) return rc;
    if ((rc = board_import(c, p + h.state_at, h.state_len)) != CLIENT_OK) return rc;
    if (c->g->num_players < 1 || c->r.n != c->g->num_players || h.seat >= c->g->num_players) return CLIENT_E_MISMATCH;
    c->has_roster = true;
    c->identity_at = h.trailer_at;
    // The game's rating stamp, when the island carries one: it counts this
    // board's seats on a board that is over, as the server's own row must
    // (view.h state_blob_load_rated), or the envelope is not one a kernel wrote.
    GameRatings stamp;
    if (!env_island_ratings(p, &h, &stamp)) return CLIENT_E_FORMAT;
    if (stamp.n && (stamp.n != c->g->num_players || status != GAME_STATUS_GAME_OVER)) return CLIENT_E_FORMAT;
    c->version = h.version;
    view_fill(c, h.seat, status);
    c->view.rating_known = true;
    if (stamp.n) {
        c->view.rated = true;
        memcpy(c->view.rating_before, stamp.before, sizeof(stamp.before));
        memcpy(c->view.rating_delta, stamp.delta, sizeof(stamp.delta));
        for (int s = 0; s < stamp.n; s++) c->view.rating_after[s] = stamp.before[s] + stamp.delta[s];
    }
    return CLIENT_OK;
}

int client_adopt_board(ClientTable *c, const Game *g, int viewer) {
    unsigned char buf[24 + MAX_DECK + 2 * MAX_BATTLES + MAX_PLAYERS * (3 + MAX_HAND_SIZE) + 1 + MAX_PLAYERS];
    c->open = false;
    c->detail = 0;
    if (g->num_players < 1 || g->num_players > MAX_PLAYERS || viewer >= g->num_players) return CLIENT_E_MISMATCH;
    const int n = state_put(g, viewer < 0 ? VIEW_SPECTATOR : viewer, buf);
    const int rc = board_import(c, buf, n);
    if (rc != CLIENT_OK) return rc;
    c->has_roster = false;
    c->table_rules = g->rules;   // the module's own game says what it plays
    c->version = 0;
    view_fill(c, viewer < 0 ? -1 : viewer, c->g->status);
    return CLIENT_OK;
}

int client_adopt_state(ClientTable *c, const uint8_t *p, int len, int viewer) {
    c->open = false;
    c->detail = 0;
    if (!p || len < 0 || viewer >= MAX_PLAYERS) return CLIENT_E_FORMAT;
    const int rc = board_import(c, p, len);
    if (rc != CLIENT_OK) return rc;
    if (viewer >= c->g->num_players) return CLIENT_E_MISMATCH;
    c->has_roster = false;
    c->table_rules = 0;   // a board alone names no rules: the classic game
    c->version = 0;
    view_fill(c, viewer < 0 ? -1 : viewer, c->g->status);
    return CLIENT_OK;
}

// ---------- the push --------------------------------------------------------------

typedef struct {
    int n_seats;       // every board's seat count, -1 until the first
    int ok;
} PushCheck;

static int card_byte_ok(unsigned b) { return b <= 51 || b == WIRE_CARD_HIDDEN; }

static int board_seats(const unsigned char *snap, int len) {
    return state_measure(snap, len) == len ? (int8_t)snap[1] : -1;
}

static void push_check(void *ctx, int index, const EvwRead *ev) {
    PushCheck *k = (PushCheck *)ctx;
    (void)index;
    int ok = ev->type <= EVW_T_CARDS_TO_TRASH && ev->msg <= EVW_MSG_FIRST_ATTACKER
          && (ev->from <= EVW_LOC_FLIPPED || ev->from == EVW_LOC_NONE)
          && (ev->to <= EVW_LOC_FLIPPED || ev->to == EVW_LOC_NONE)
          && ev->n_cards <= MAX_HAND_SIZE && (!ev->has_target || ev->target_wire <= 51)
          && (!ev->has_battle || ev->battle < MAX_BATTLES);
    for (int i = 0; ok && i < ev->n_cards; i++) ok = card_byte_ok(ev->cards_wire[i]);
    const int seats = board_seats(ev->snap, ev->snap_len);
    ok = ok && seats >= 0 && (k->n_seats < 0 || seats == k->n_seats) && (ev->seat < 0 || ev->seat < seats);
    if (k->n_seats < 0) k->n_seats = seats;
    k->ok &= ok;
}

int client_push_open(ClientTable *c, const uint8_t *p, int len, int as3,
                     const uint8_t *identity, int identity_len, uint32_t version) {
    c->open = false;
    c->detail = 0;
    if (!p || len < 4) return CLIENT_E_PUSH;
    int seq = len, flags = 0, block = 0, rc, status = 0;
    c->identity_at = -1;
    c->has_roster = false;
    c->table_rules = 0;   // the identity below, when there is one, says otherwise
    if (as3) {
        if (evwire_as3_split(p, len, &seq, &flags, &block) != 0) return CLIENT_E_PUSH;
    } else {
        // An as2 payload is the sequence alone - or the whole as3 push, which a
        // server since Phase 4b sends labelled as2 until Phase 5b: then the bytes
        // after the sequence must be exactly an as3 block, and are read as one.
        const unsigned char *end = 0;
        int end_len = 0;
        if (evwire_read(p, len, 0, &end, &end_len, 0, 0) < 0) return CLIENT_E_PUSH;
        if ((int)(end - p) + end_len != len && evwire_as3_split(p, len, &seq, &flags, &block) != 0) return CLIENT_E_PUSH;
    }
    const int named = (flags & EVW_AS3_ROSTER) || identity_len > 0;
    if (flags & EVW_AS3_ROSTER) {
        if ((rc = identity_read(c, p + block, len - block, &status, CLIENT_E_TRAILER)) != CLIENT_OK) return rc;
    } else if (identity_len > 0) {
        if ((rc = identity_read(c, identity, identity_len, &status, CLIENT_E_IDENTITY)) != CLIENT_OK) return rc;
    }

    EvwHeader h;
    const unsigned char *fin = 0;
    int fin_len = 0;
    PushCheck k = { -1, 1 };
    const int n = evwire_read(p, seq, &h, &fin, &fin_len, push_check, &k);
    if (n < 0 || !k.ok || (int)(fin - p) + fin_len != seq) return CLIENT_E_PUSH;
    const int seats = board_seats(fin, fin_len);
    if (seats < 1 || (k.n_seats >= 0 && seats != k.n_seats)) return CLIENT_E_PUSH;
    if ((named && c->r.n != seats) || h.viewer >= seats) return CLIENT_E_MISMATCH;

    c->has_roster = named;
    if (flags & EVW_AS3_ROSTER) c->identity_at = block;
    c->push = p;
    c->final = fin;
    c->final_len = fin_len;
    c->at = 4;
    c->index = 0;
    c->n_events = n;
    c->viewer = h.viewer;
    c->n_seats = seats;
    c->version = version;
    c->open = true;
    return CLIENT_OK;
}

static Card event_card(unsigned b) {
    if (b == WIRE_CARD_HIDDEN) { Card x; x.suit = -1; x.value = -1; return x; }
    return card_of_id((int)b);
}

int client_cast_open(ClientTable *c, const uint8_t *p, int len, const uint8_t *identity, int identity_len) {
    uint32_t version = 0;
    const int at = evwire_cast_header_get(p, len, &version);
    if (at < 0) {
        c->open = false;
        c->detail = 0;
        return CLIENT_E_PUSH;
    }
    const int rc = client_push_open(c, p + at, len - at, 1, identity, identity_len, version);
    // client_identity_at answers in the bytes the host handed over, and those
    // begin at the header.
    if (rc == CLIENT_OK && c->identity_at >= 0) c->identity_at += at;
    return rc;
}

int client_push_next(ClientTable *c) {
    if (!c->open) return CLIENT_E_ORDER;
    if (c->index >= c->n_events) return 0;
    // The bytes were read whole at open; this is the same walk, one event on.
    const uint8_t *q = c->push + c->at;
    PushEvent *e = &c->event;
    const int flags = q[5], n = q[6];
    e->type = (int8_t)q[0];
    e->seat = q[1] == EVW_SEAT_NONE ? -1 : (int8_t)q[1];
    e->msg = (int8_t)q[2];
    e->from = q[3] == EVW_LOC_NONE ? -1 : (int8_t)q[3];
    e->to = q[4] == EVW_LOC_NONE ? -1 : (int8_t)q[4];
    e->n_cards = (uint8_t)n;
    for (int i = 0; i < n; i++) e->cards[i] = event_card(q[7 + i]);
    int at = 7 + n;
    e->has_target = (flags & 1) != 0;
    e->target = e->has_target ? card_of_id(q[at++]) : CARD_NONE;
    e->battle = (flags & 2) ? (int8_t)q[at++] : -1;
    const int snap_len = le_get_u16(q + at);
    at += 2;
    const int rc = board_import(c, q + at, snap_len);
    if (rc != CLIENT_OK) { c->open = false; return rc; }
    c->at += at + snap_len;
    c->index++;
    view_fill(c, c->viewer, c->g->status);
    return 1;
}

int client_push_final(ClientTable *c) {
    if (!c->open) return CLIENT_E_ORDER;
    c->open = false;
    const int rc = board_import(c, c->final, c->final_len);
    if (rc != CLIENT_OK) return rc;
    view_fill(c, c->viewer, c->g->status);
    return CLIENT_OK;
}

// ---------- identity ---------------------------------------------------------------

int client_identity(const ClientTable *c, uint8_t *out, int cap) {
    if (!c->has_roster) return 0;
    const int n = roster_trailer_write_ai(&c->r, c->gid, c->gid_len, 0, 0, c->ai_mask,
                                          (c->table_rules & GAME_RULE_NO_PASS) == 0, out, cap);
    return n == ROSTER_E_CAP ? CLIENT_E_CAP : n < 0 ? CLIENT_E_IDENTITY : n;
}

int client_identity_at(const ClientTable *c) { return c->identity_at; }

int client_identity_begin(ClientTable *c, const char *gid, int gid_len, const char *title, int title_len) {
    Roster r;
    memset(&r, 0, sizeof(r));
    if (gid_len < 0 || gid_len > ROSTER_GAME_ID_MAX || (gid_len > 0 && !gid)) return CLIENT_E_IDENTITY;
    const int rc = roster_set_title(&r, title, title_len);
    if (rc != ROSTER_OK) { c->detail = rc; return CLIENT_E_IDENTITY; }
    c->r = r;
    c->ai_mask = 0;
    c->table_rules = 0;   // parts from a server that predates the rules: the classic game
    memcpy(c->gid, gid, (size_t)gid_len);
    c->gid[gid_len] = 0;
    c->gid_len = (uint8_t)gid_len;
    c->has_roster = true;
    c->identity_at = -1;
    return CLIENT_OK;
}

int client_identity_seat(ClientTable *c, const char *id, int id_len, const char *name, int name_len, int is_ai) {
    if (!c->has_roster) return CLIENT_E_ORDER;
    const int s = roster_seat_add(&c->r, id, id_len, name, name_len, "", 0);
    if (s < 0) { c->detail = s; return CLIENT_E_IDENTITY; }
    if (is_ai) c->ai_mask |= 1u << s;
    return CLIENT_OK;
}

// ---------- what a board shows ------------------------------------------------------

int client_view_rules(const TableView *v, int from_deck, int to_flipped, int trump_out, ViewRules *out) {
    const int n = v->num_players;
    if (n < 0 || n > MAX_PLAYERS || v->num_battles < 0 || v->num_battles > MAX_BATTLES
        || v->my_seat < -1 || v->my_seat >= n || from_deck < 0 || to_flipped < 0
        || trump_out < 0 || trump_out > 1) return CLIENT_E_FORMAT;
    memset(out, 0, sizeof(*out));

    // A seat is marked when the board names it. Until the trump has turned the
    // kernel names nobody (GAME_SEAT_NONE, game.h): the lobby and every board of
    // the opening deal before START_DEFENDER, so no guess about "is it dealt
    // yet" is made here.
    out->first_attacker_badge = (int8_t)(v->num_battles == 0 && v->first_attacker >= 0 && v->first_attacker < n
                                         ? v->first_attacker : -1);
    out->defender_badge = (int8_t)(v->defender >= 0 && v->defender < n ? v->defender : -1);

    // Good is handle_good's to allow (a playing game, a seat still in, not the
    // defender, not said already), and it is offered once the bout could close
    // on it: every attack on the table covered.
    const int me = v->my_seat;
    bool covered = v->num_battles > 0;
    for (int i = 0; covered && i < v->num_battles; i++) covered = !card_is_none(v->battles[i].defense);
    out->can_say_good = me >= 0 && v->status == GAME_STATUS_PLAYING && v->seats[me].status == PLAYER_STATUS_IN
        && me != v->defender && !((v->good_mask >> me) & 1u) && covered;
    out->can_set_rules = game_lobby_can_set_rules(me, v->status == GAME_STATUS_WAITING) != 0;
    out->can_join = me < 0 && game_lobby_has_room(n, v->status == GAME_STATUS_WAITING);

    for (int s = 0; s < n && !out->needs_bots; s++)
        out->needs_bots = seat_needs_bot(v->status, v->seats[s].status, v->seats[s].is_ai);
    for (int s = 0; s < n && !out->bot_to_move; s++)
        out->bot_to_move = v->seats[s].is_ai && turn_may_act(v->status, v->seats[s].status, s, v->num_battles, covered,
                                                             v->first_attacker, v->defender, v->good_mask);

    // The stock: what is flying out of it has left the pile, and a card on its
    // way to the trump's slot is still the stock's, so it stays on the count.
    // The trump on its way to a hand has left both its slot and the count.
    const int pile = v->deck_count - from_deck;
    const bool trump_held = v->has_flipped && !trump_out;
    out->deck_pile = (int16_t)(pile > 0 ? pile : 0);
    out->deck_badge = (int16_t)(out->deck_pile + (trump_held ? 1 : 0) + to_flipped);
    out->show_deck_pile = out->deck_pile > 0;
    // THE PLACE OUTLIVES THE PILE while cards are leaving it: the draw that
    // takes the stock's last cards empties the pile the instant it opens, and
    // those cards still fly from where the pile lay. The slot, likewise, stays
    // while the trump leaves it (the board keeps has_flipped until it lands).
    out->show_deck_spot = out->show_deck_pile || from_deck > 0;
    out->wants_envelope = v->status == GAME_STATUS_GAME_OVER && !v->rating_known;
    out->show_flipped_slot = out->show_deck_pile || v->has_flipped || to_flipped > 0;
    out->show_flipped_card = trump_held;
    out->show_trump_icon = !out->show_deck_pile && !v->has_flipped && to_flipped == 0;
    return CLIENT_OK;
}

// ---------- the boards a client makes ------------------------------------------------

// A view whose counts fit what it holds and whose viewer is a seat or nobody.
static int view_holds(const TableView *v) {
    return v->num_players >= 0 && v->num_players <= MAX_PLAYERS && v->num_battles >= 0 && v->num_battles <= MAX_BATTLES
        && v->num_eliminated >= 0 && v->num_eliminated <= MAX_PLAYERS && v->my_hand_count >= 0 && v->my_hand_count <= MAX_HAND_SIZE
        && v->my_seat >= -1 && v->my_seat < v->num_players;
}

static Game *rules_game(ClientTable *c) { return (Game *)(void *)c->rules.bytes; }

// The view as the rules read it (see client_table.h): the masked game a server's
// view of this board would import as, the viewer's hand real and every card it
// cannot see the placeholder a masked import holds. GAME_VALID, or the
// GAME_INVALID_* the board is refused for.
static int board_game(ClientTable *c, const TableView *v) {
    if (!view_holds(v) || v->deck_count < 0 || v->deck_count > MAX_DECK) return GAME_INVALID_COUNT;
    Game *g = rules_game(c);
    const Card hidden = { .suit = 0, .value = 1 };   // view.c card_from_wire_masked
    memset(g, 0, sizeof c->rules.bytes);
    g->status = v->status;
    g->num_players = v->num_players;
    g->power_suit = v->power_suit;
    g->first_attacker = v->first_attacker;
    g->defender = v->defender;
    g->discard_pile_length = v->discard_pile_length;
    g->has_flipped = v->has_flipped;
    g->flipped = v->has_flipped ? v->flipped : (Card){ .suit = 0, .value = 0 };
    g->good_players_mask = v->good_mask;
    g->has_good_timestamp = v->has_good_timestamp;
    g->deck_count = v->deck_count;
    for (int i = 0; i < v->deck_count; i++) g->deck[i] = hidden;
    g->num_battles = v->num_battles;
    memcpy(g->table_battles, v->battles, sizeof(Battle) * (size_t)v->num_battles);
    for (int s = 0; s < v->num_players; s++) {
        Player *p = &g->players[s];
        const int mine = s == v->my_seat;
        const int n = mine ? v->my_hand_count : v->seats[s].hand_count;
        if (n < 0 || n > MAX_HAND_SIZE) return GAME_INVALID_COUNT;
        p->status = v->seats[s].status;
        p->awaiting_attack = v->seats[s].awaiting_attack;
        p->hand_count = (int8_t)n;
        for (int i = 0; i < n; i++) p->hand[i] = mine ? v->my_hand[i] : hidden;
    }
    g->num_eliminated = v->num_eliminated;
    memcpy(g->elimination_order, v->elimination, (size_t)v->num_eliminated);
    // The board plays the table's rules, so a gate or a menu on it is the one the
    // server's game would give (no transfer is offered or allowed at podkidnoy).
    g->rules = (int8_t)(v->passing ? 0 : GAME_RULE_NO_PASS);
    // A dry run's draws take the stock's top (no random draw, nothing of the
    // module's generator spent) and its log records land in the one slot.
    g->deterministic_deck = true;
    g->log_cap = 1;
    return game_validate(g, GAME_VALIDATE_MASKED);
}

int client_validate(ClientTable *c, const TableView *v, const uint8_t *awire, int len) {
    const int valid = board_game(c, v);
    if (valid != GAME_VALID) return valid;
    AwireAction a;
    if (!awire || len < 0 || !awire_decode(awire, len, &a)) return CLIENT_E_MOVE;
    const int seat = v->my_seat < 0 ? 0 : v->my_seat;
    if (seat >= v->num_players) return CLIENT_E_MOVE;
    // A gate must not reach the host's animation snapshots.
    void (*hook)(const Game *, int, int) = engine_snap_hook;
    engine_snap_hook = 0;
    engine_last_reject = ENGINE_REJECT_NONE;
    const bool ok = awire_apply(rules_game(c), seat, &a);
    engine_snap_hook = hook;
    return ok ? 0 : engine_last_reject;
}

int client_move_wire(const ClientMove *m, uint8_t *out, int cap) {
    if (!m || !out || m->n_cards > AWIRE_MAX_CARDS) return CLIENT_E_MOVE;
    const bool cover = m->kind == AWIRE_COVER;
    if (cover && m->n_attacks != m->n_cards) return CLIENT_E_MOVE;
    AwireAction a;
    a.kind = m->kind;
    a.n = m->n_cards;
    for (int i = 0; i < a.n; i++) {
        if (!card_in_range(m->cards[i], 1, ACE_VALUE)) return CLIENT_E_MOVE;
        a.cards[i] = m->cards[i];
        if (!cover) continue;
        if (!card_in_range(m->attack_cards[i], 1, ACE_VALUE)) return CLIENT_E_MOVE;
        a.attacks[i] = m->attack_cards[i];
    }
    // Into a frame of its own first: awire_encode answers 0 for a move that is
    // not one and for a buffer too small alike, and a host is told which.
    unsigned char frame[2 + 2 * AWIRE_MAX_CARDS];
    const int n = awire_encode(&a, frame, sizeof(frame));
    if (n <= 0) return CLIENT_E_MOVE;
    if (n > cap) return CLIENT_E_CAP;
    memcpy(out, frame, (size_t)n);
    return n;
}

// The viewer's hand without any of `cards`.
static void hand_without(TableView *v, const Card *cards, int n) {
    int kept = 0;
    for (int i = 0; i < v->my_hand_count; i++) {
        int gone = 0;
        for (int k = 0; k < n && !gone; k++) gone = card_eq(v->my_hand[i], cards[k]);
        if (!gone) v->my_hand[kept++] = v->my_hand[i];
    }
    v->my_hand_count = (int8_t)kept;
}

static int card_on_table(const TableView *v, Card c) {
    for (int i = 0; i < v->num_battles; i++)
        if (card_eq(v->battles[i].attack, c) || (!card_is_none(v->battles[i].defense) && card_eq(v->battles[i].defense, c))) return 1;
    return 0;
}

int client_optimistic_apply(ClientTable *c, TableView *v, const uint8_t *awire, int len) {
    if (!view_holds(v)) return CLIENT_E_FORMAT;
    AwireAction a;
    if (!awire || len < 0 || !awire_decode(awire, len, &a)) return CLIENT_E_MOVE;
    switch (a.kind) {
        case AWIRE_ATTACK:
        case AWIRE_PASS: {
            // A card the table already shows was laid by this move's confirmation, or
            // kept there by a push, and is not laid twice; a move none of whose cards
            // is new has already happened, so a pass hands the shield on no further.
            Card fresh[AWIRE_MAX_CARDS];
            int n = 0;
            for (int i = 0; i < a.n; i++) if (!card_on_table(v, a.cards[i])) fresh[n++] = a.cards[i];
            if (n == 0) return CLIENT_OK;
            int defender = v->defender;
            if (a.kind == AWIRE_PASS) {
                const int valid = board_game(c, v);
                if (valid != GAME_VALID) { c->detail = valid; return CLIENT_E_STATE; }
                defender = get_next_player_index(rules_game(c), v->defender);
            }
            if (v->num_battles + n > MAX_BATTLES) return CLIENT_E_CAP;
            for (int i = 0; i < n; i++) v->battles[v->num_battles++] = (Battle){ .attack = fresh[i], .defense = CARD_NONE };
            hand_without(v, a.cards, a.n);
            v->defender = (int8_t)defender;
            return CLIENT_OK;
        }
        case AWIRE_COVER:
            for (int b = 0; b < v->num_battles; b++)
                for (int i = 0; i < a.n; i++)
                    if (card_eq(a.attacks[i], v->battles[b].attack)) { v->battles[b].defense = a.cards[i]; break; }
            hand_without(v, a.cards, a.n);
            return CLIENT_OK;
        case AWIRE_PICKUP: {
            if (v->num_battles == 0) return CLIENT_OK;   // the table is taken already: the turn has moved
            int table = 0;
            for (int b = 0; b < v->num_battles; b++) table += card_is_none(v->battles[b].defense) ? 1 : 2;
            if (v->my_hand_count + table > MAX_HAND_SIZE) return CLIENT_E_CAP;
            // The server rotates after the refill; the board knows that rotation
            // only while the refill cannot put a seat out.
            int knowable = 1;
            for (int s = 0; s < v->num_players; s++)
                if (s != v->my_seat && v->seats[s].status == PLAYER_STATUS_IN && v->seats[s].hand_count <= 0) knowable = 0;
            if (knowable) {
                const int valid = board_game(c, v);
                if (valid != GAME_VALID) { c->detail = valid; return CLIENT_E_STATE; }
                const int lead = get_next_player_index(rules_game(c), v->defender);
                v->defender = (int8_t)get_next_player_index(rules_game(c), lead);
                v->first_attacker = (int8_t)lead;
            }
            for (int b = 0; b < v->num_battles; b++) {
                v->my_hand[v->my_hand_count++] = v->battles[b].attack;
                if (!card_is_none(v->battles[b].defense)) v->my_hand[v->my_hand_count++] = v->battles[b].defense;
            }
            v->num_battles = 0;
            return CLIENT_OK;
        }
        default:   // good: nothing moves until the server says the bout closed
            return CLIENT_OK;
    }
}

int client_board_edit(ClientTable *c, TableView *v, const BoardEdit *e) {
    if (!view_holds(v) || e->n_cards > MAX_HAND_SIZE) return CLIENT_E_FORMAT;
    const int n = e->n_cards;
    switch (e->op) {
        case CLIENT_EDIT_KEEP:
            if (v->my_seat < 0) return CLIENT_OK;   // a spectator makes no move to keep
            for (int i = 0; i < n; i++) {
                const Card card = e->cards[i];
                if (!card_on_table(v, card)) {
                    if (!card_is_none(e->target)) {
                        for (int b = 0; b < v->num_battles; b++) {
                            if (!card_eq(v->battles[b].attack, e->target)) continue;
                            if (card_is_none(v->battles[b].defense)) v->battles[b].defense = card;
                            break;
                        }
                    } else {
                        if (v->num_battles >= MAX_BATTLES) return CLIENT_E_CAP;
                        v->battles[v->num_battles++] = (Battle){ .attack = card, .defense = CARD_NONE };
                    }
                }
                hand_without(v, &card, 1);
            }
            return CLIENT_OK;
        case CLIENT_EDIT_TURN:
            v->first_attacker = e->first_attacker;
            v->defender = e->defender;
            return CLIENT_OK;
        case CLIENT_EDIT_TABLE:
            if (n > MAX_BATTLES) return CLIENT_E_CAP;
            for (int i = 0; i < n; i++) v->battles[i] = (Battle){ .attack = e->cards[i], .defense = CARD_NONE };
            v->num_battles = (int8_t)n;
            return CLIENT_OK;
        case CLIENT_EDIT_LIFT: {
            int kept = 0;
            for (int b = 0; b < v->num_battles; b++) {
                const Battle bt = v->battles[b];
                int lifted = 0;
                for (int i = 0; i < n && !lifted; i++)
                    lifted = card_eq(bt.attack, e->cards[i]) || (!card_is_none(bt.defense) && card_eq(bt.defense, e->cards[i]));
                if (!lifted) v->battles[kept++] = bt;
            }
            v->num_battles = (int8_t)kept;
            return CLIENT_OK;
        }
        case CLIENT_EDIT_RETURN:
            if (v->my_seat < 0) return CLIENT_OK;
            for (int i = 0; i < n; i++) {
                int held = 0;
                for (int h = 0; h < v->my_hand_count && !held; h++) held = card_eq(v->my_hand[h], e->cards[i]);
                if (held) continue;
                if (v->my_hand_count >= MAX_HAND_SIZE) return CLIENT_E_CAP;
                v->my_hand[v->my_hand_count++] = e->cards[i];
            }
            return CLIENT_OK;
        case CLIENT_EDIT_WITHDRAW:
            if (v->my_seat < 0) return CLIENT_OK;   // a spectator makes no move to withdraw
            for (int i = 0; i < n; i++) {
                const Card card = e->cards[i];
                int kept = 0;
                for (int b = 0; b < v->num_battles; b++) {
                    Battle bt = v->battles[b];
                    if (card_eq(bt.attack, card)) continue;                                   // an attack takes its battle
                    if (!card_is_none(bt.defense) && card_eq(bt.defense, card)) bt.defense = CARD_NONE;   // a cover uncovers
                    v->battles[kept++] = bt;
                }
                v->num_battles = (int8_t)kept;
                int held = 0;
                for (int h = 0; h < v->my_hand_count && !held; h++) held = card_eq(v->my_hand[h], card);
                if (held) continue;
                if (v->my_hand_count >= MAX_HAND_SIZE) return CLIENT_E_CAP;
                v->my_hand[v->my_hand_count++] = card;
            }
            return CLIENT_OK;
        case CLIENT_EDIT_UNDEAL:
            v->deck_count = (int16_t)(NUM_SUITS * (ACE_VALUE - min_value_for(v->num_players) + 1));
            v->has_flipped = false;
            v->flipped = CARD_NONE;
            v->num_battles = 0;
            v->my_hand_count = 0;
            for (int s = 0; s < v->num_players; s++) v->seats[s].hand_count = 0;
            // The deal's START_MAGIC board, so nobody leads or defends on it yet
            // (game.c start_game_dealt): the roles arrive with the deal's last
            // board, START_DEFENDER, never before it.
            v->first_attacker = GAME_SEAT_NONE;
            v->defender = GAME_SEAT_NONE;
            return CLIENT_OK;
        case CLIENT_EDIT_LOBBY: {
            const int valid = board_game(c, v);
            if (valid != GAME_VALID) { c->detail = valid; return CLIENT_E_STATE; }
            uint32_t bots = 0;
            for (int s = 0; s < v->num_players; s++) if (v->seats[s].is_ai) bots |= 1u << s;
            Game *g = rules_game(c);
            game_reset_to_lobby(g, bots);
            view_board(v, g, v->my_seat, g->status);
            return CLIENT_OK;
        }
        default:
            return CLIENT_E_FORMAT;
    }
}

int client_rearrange_hand(ClientTable *c, TableView *v, const uint8_t *idx, int n) {
    if (!view_holds(v)) return CLIENT_E_FORMAT;
    if (v->my_seat < 0 || v->my_hand_count == 0) return CLIENT_E_MISMATCH;
    const int valid = board_game(c, v);
    if (valid != GAME_VALID) { c->detail = valid; return CLIENT_E_STATE; }
    Game *g = rules_game(c);
    if (!game_rearrange_hand(g, v->my_seat, idx, n)) return CLIENT_E_MOVE;
    memcpy(v->my_hand, g->players[v->my_seat].hand, (size_t)v->my_hand_count);
    return CLIENT_OK;
}

int client_ask_apply(TableView *v, const TableAsk *a) {
    if (!a || !view_holds(v)) return CLIENT_E_FORMAT;
    if (a->kind != ASK_UPDATE_NAME && a->kind != ASK_SET_RULES && a->kind != ASK_REARRANGE_PLAYERS) return CLIENT_OK;
    if (v->my_seat < 0 || v->status != GAME_STATUS_WAITING) return CLIENT_E_MISMATCH;
    if (a->kind == ASK_UPDATE_NAME) {
        int lo;
        const int keep = a->title_len > ASK_TITLE_MAX ? ROSTER_E_TITLE : roster_title_keep(a->title, a->title_len, &lo);
        if (keep < 0 || keep > ROSTER_TITLE_MAX) return CLIENT_E_MOVE;
        memcpy(v->title, a->title + lo, (size_t)keep);
        v->title[keep] = 0;
        v->title_len = (uint8_t)keep;
        return CLIENT_OK;
    }
    if (a->kind == ASK_SET_RULES) {
        if (a->passing > 1) return CLIENT_E_MOVE;
        v->passing = a->passing == 1;
        return CLIENT_OK;
    }
    // New seat i is the seat the ask's i-th id names, each seat named once.
    const int n = v->num_players;
    ViewSeat old[MAX_PLAYERS];
    int8_t from[MAX_PLAYERS];
    uint32_t seen = 0;
    if (a->n_order != n) return CLIENT_E_MOVE;
    for (int i = 0; i < n; i++) {
        int s = 0;
        if (a->order[i].len > ROSTER_ID_MAX) return CLIENT_E_MOVE;
        while (s < n && (v->seats[s].id_len != a->order[i].len
                         || memcmp(v->seats[s].id, a->order[i].id, a->order[i].len) != 0)) s++;
        if (s == n || (seen & (1u << s))) return CLIENT_E_MOVE;
        seen |= 1u << s;
        from[i] = (int8_t)s;
    }
    memcpy(old, v->seats, sizeof old);
    const int me = v->my_seat;
    for (int i = 0; i < n; i++) {
        v->seats[i] = old[from[i]];
        if (from[i] == me) v->my_seat = (int8_t)i;
    }
    return CLIENT_OK;
}

static int uncovered(const TableView *v) {
    int n = 0;
    for (int b = 0; b < v->num_battles; b++) n += card_is_none(v->battles[b].defense);
    return n;
}

// A card as the conflict rule names it: its dense id, or ANIM_CARD_NONE for a
// back or no card at all.
// ---- the plan, asked with boards (client_table.h) ----------------------------

// A row cell: the card's dense id, or ANIM_TABLE_UNKNOWN for one that is there
// and cannot be named.
static unsigned char plan_row_id(Card c) {
    return card_in_range(c, 1, ACE_VALUE) ? (unsigned char)card_to_id(c) : (unsigned char)ANIM_TABLE_UNKNOWN;
}

// The four numbers and the row the plan reads off a board. `hand` is written
// for every seat a table can have, so a narrower board reads as empty seats.
static int plan_board(const TableView *v, uint8_t *deck, uint8_t *discard, Card *flipped, int *hand,
                      uint8_t *row, uint8_t *n_row) {
    if (!view_holds(v)) return ANIM_EBADARG;
    if (row && 2 * v->num_battles > ANIM_ASK_MAX_ROW) return ANIM_ECAP;
    *deck = (uint8_t)v->deck_count;
    *discard = (uint8_t)v->discard_pile_length;
    *flipped = v->has_flipped ? v->flipped : CARD_NONE;
    for (int s = 0; s < MAX_PLAYERS; s++) hand[s] = s < v->num_players ? v->seats[s].hand_count : 0;
    if (!row) return ANIM_EOK;
    for (int b = 0; b < v->num_battles; b++) {
        row[2 * b] = plan_row_id(v->battles[b].attack);
        row[2 * b + 1] = card_is_none(v->battles[b].defense) ? (unsigned char)ANIM_TABLE_NONE
                                                             : plan_row_id(v->battles[b].defense);
    }
    *n_row = (uint8_t)(2 * v->num_battles);
    return ANIM_EOK;
}

int client_plan_step(AnimPlanAsk *ask, int at, const ClientPlanStep *step, const TableView *board) {
    if (!ask || !step || at < 0) return ANIM_EBADARG;
    if (at == 0) ask->n_events = ask->n_players = 0;
    if (at != ask->n_events) return ANIM_EBADARG;
    if (at >= ANIM_MAX_STEPS || step->n_cards > ANIM_ASK_MAX_CARDS) return ANIM_ECAP;
    AnimPlanAskEvent *ev = &ask->events[at];
    ev->type = step->type;
    ev->seat = step->seat;
    ev->from = step->from;
    ev->to = step->to;
    ev->n_cards = step->n_cards;
    int backs = 0;
    for (int i = 0; i < step->n_cards; i++) {
        ev->cards[i] = step->cards[i];
        if (step->cards[i].suit < 0) backs++;
    }
    ev->mask_cards = step->n_cards > 0 && backs == step->n_cards;
    ev->has_counts = board != NULL;
    ev->n_row = 0;
    if (board) {
        const int rc = plan_board(board, &ev->deck, &ev->discard, &ev->flipped, ev->hand, ev->row, &ev->n_row);
        if (rc != ANIM_EOK) return rc;
        if (board->num_players > ask->n_players) ask->n_players = (uint8_t)board->num_players;
    }
    ask->n_events = (uint8_t)(at + 1);
    return ANIM_EOK;
}

int client_plan(AnimPlanAsk *ask, int n, const TableView *final, AnimPlan *out) {
    if (!ask) return ANIM_EBADARG;
    if (n == 0) ask->n_events = ask->n_players = 0;
    if (n != ask->n_events) return ANIM_EBADARG;
    if (final) {
        const int rc = plan_board(final, &ask->final_deck, &ask->final_discard, &ask->final_flipped,
                                  ask->final_hand, NULL, NULL);
        if (rc != ANIM_EOK) return rc;
        ask->n_players = (uint8_t)final->num_players;
    } else {
        ask->final_deck = ask->final_discard = 0;
        ask->final_flipped = CARD_NONE;
        memset(ask->final_hand, 0, sizeof ask->final_hand);
    }
    return anim_plan_ask(ask, out);
}

static int conflict_id(Card c) {
    // card.h's range test: the suit's upper bound is the bitfield's, which gcc
    // was answering itself (-Wtype-limits).
    return card_in_range(c, 1, ACE_VALUE) ? card_to_id(c) : ANIM_CARD_NONE;
}

// The verdicts for the `n` motions `idx` picks out of `q`, against the events
// `played` does not mark (NULL: every event), under `h`.
static int conflict_verdicts(const TableView *open, const TableView *final, const ClientConflict *q,
                             const uint8_t *played, const uint8_t *idx, int n, const ConflictHope *h, uint8_t *verdicts) {
    // Module storage, like the slot: a host asks one question at a time.
    static AnimEvent events[CLIENT_CONFLICT_MAX_EVENTS];
    static int moved[CLIENT_CONFLICT_MAX_EVENTS * MAX_HAND_SIZE];
    int n_events = 0;
    for (int e = 0; e < q->n_events; e++) {
        const ConflictEvent *ce = &q->events[e];
        if (ce->n_cards > MAX_HAND_SIZE) return CLIENT_E_FORMAT;
        if (played && played[e]) continue;
        events[n_events++] = (AnimEvent){ .type = ce->type, .seat = ANIM_SEAT_NONE, .from = ANIM_LOC_NONE, .to = ANIM_LOC_NONE,
                                          .cards = ce->cards, .n_cards = ce->n_cards, .mask_cards = ce->masked ? 1 : 0 };
    }
    int table_cleared = 0;
    const int n_moved = anim_conflict_sweep(events, n_events, moved, (int)(sizeof moved / sizeof moved[0]), &table_cleared);
    if (n_moved < 0) return n_moved;

    // Where the push's cards stand: both sides of every battle on its last board, and the viewer's hand there.
    unsigned char table[2 * MAX_BATTLES], hand[MAX_HAND_SIZE];
    for (int b = 0; b < open->num_battles; b++) {
        const int attack = conflict_id(open->battles[b].attack), cover = conflict_id(open->battles[b].defense);
        table[2 * b] = attack < 0 ? (unsigned char)ANIM_TABLE_NONE : (unsigned char)attack;
        table[2 * b + 1] = cover < 0 ? (unsigned char)ANIM_TABLE_NONE : (unsigned char)cover;
    }
    int n_hand = 0;
    for (int i = 0; i < open->my_hand_count; i++) {
        const int id = conflict_id(open->my_hand[i]);
        if (id >= 0) hand[n_hand++] = (unsigned char)id;
    }
    AnimConflictFacts facts;
    const int fr = anim_conflict_facts(moved, n_moved, table, open->num_battles, hand, n_hand, &facts);
    if (fr != ANIM_EOK) return fr;

    int pending = h->pending_attacks;
    if (pending < 0) {
        pending = 0;
        for (int i = 0; i < n; i++) pending += !q->motions[idx[i]].is_cover;
    }
    AnimServerHope hope;
    hope.table_cleared = table_cleared;
    hope.pending_attacks = pending;
    hope.defender_hand = h->defender_seat >= 0 && h->defender_seat < final->num_players ? final->seats[h->defender_seat].hand_count : 0;
    hope.final_uncovered = uncovered(h->uncovered_on_final ? final : open);
    for (int i = 0; i < n; i++) {
        const ConflictMotion *m = &q->motions[idx[i]];
        hope.is_cover = m->is_cover;
        const int v = anim_conflict_verdict(conflict_id(m->card), m->dest, &facts, &hope);
        if (v < 0) return v;
        verdicts[i] = (uint8_t)v;
    }
    return n;
}

int client_conflict_verdicts(const TableView *open, const TableView *final, const ClientConflict *q,
                             const ConflictHope *h, ConflictVerdicts *out) {
    if (!view_holds(open) || !view_holds(final) || q->n_events > CLIENT_CONFLICT_MAX_EVENTS
        || q->n_motions > CLIENT_CONFLICT_MAX_MOTIONS) return CLIENT_E_FORMAT;
    out->n = 0;
    uint8_t all[CLIENT_CONFLICT_MAX_MOTIONS];
    for (int i = 0; i < q->n_motions; i++) all[i] = (uint8_t)i;
    const int n = conflict_verdicts(open, final, q, 0, all, q->n_motions, h, out->verdicts);
    if (n >= 0) out->n = (uint8_t)n;
    return n;
}

// ---------- a push against my unanswered moves ----------------------------------

// The event a pending motion comes back as: an attack and a pass are one type
// on the wire, hand to table; a cover is hand to table; a pickup table to hand.
static uint64_t motion_key(const ConflictMotion *m, int seat) {
    const int to_hand = m->dest == ANIM_DEST_MY_HAND;
    return anim_event_key(to_hand ? ANIM_EVT_PICKUP : m->is_cover ? ANIM_EVT_COVER : ANIM_EVT_ATTACK_PASS, m->card,
                          to_hand ? ANIM_LOC_TABLE : ANIM_LOC_HAND, to_hand ? ANIM_LOC_HAND : ANIM_LOC_TABLE, seat);
}

// One return flight: `n` motions of `idx`, each of them doomed.
static void fly_home(PushSettle *out, const uint8_t *idx, int n) {
    if (n <= 0) return;
    out->flight_count[out->n_flights++] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        out->order[out->n_order++] = idx[i];
        out->fates[idx[i]] = CLIENT_FATE_REVERT;
    }
}

// `idx` without the motions that are no longer pending; the new count.
static int still_pending(const PushSettle *out, uint8_t *idx, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) if (out->fates[idx[i]] == CLIENT_FATE_PENDING) idx[k++] = idx[i];
    return k;
}

int client_push_settle(ClientTable *c, const TableView *open, const TableView *final, const ClientConflict *q,
                       const uint8_t *pass_wire, int pass_len, PushSettle *out) {
    memset(out, 0, sizeof(*out));
    out->open_event = out->home_event = out->magic_event = CLIENT_EVENT_NONE;
    if (!view_holds(final) || q->n_events > CLIENT_CONFLICT_MAX_EVENTS || q->n_motions > CLIENT_CONFLICT_MAX_MOTIONS) return CLIENT_E_FORMAT;
    for (int e = 0; e < q->n_events; e++) if (q->events[e].n_cards > MAX_HAND_SIZE) return CLIENT_E_FORMAT;
    const int n_events = q->n_events, n_motions = q->n_motions, me = final->my_seat;
    out->n_events = q->n_events;
    out->n_motions = q->n_motions;

    // 1. Confirmed by a push the gate dropped: on the final table, named by no event.
    if (q->versioned) {
        Card table[2 * MAX_BATTLES];
        int n_table = 0;
        for (int b = 0; b < final->num_battles; b++) {
            table[n_table++] = final->battles[b].attack;
            if (!card_is_none(final->battles[b].defense)) table[n_table++] = final->battles[b].defense;
        }
        for (int i = 0; i < n_motions; i++) {
            int at, shown = anim_stale_optimistic_on_table(&q->motions[i].card, 1, table, n_table, 0, 0, &at, 1) == 1;
            for (int e = 0; e < n_events && shown; e++)
                shown = anim_stale_optimistic_on_table(&q->motions[i].card, 1, table, n_table, q->events[e].cards, q->events[e].n_cards, &at, 1) == 1;
            if (shown) out->fates[i] = CLIENT_FATE_SETTLED;
        }
    }

    // 2. My own move coming back: every card of the event is a pending motion of its kind.
    int first = CLIENT_EVENT_NONE, last = CLIENT_EVENT_NONE;
    for (int e = 0; e < n_events; e++) {
        const ConflictEvent *ev = &q->events[e];
        uint8_t mine[MAX_HAND_SIZE];
        int all = ev->n_cards > 0;
        for (int k = 0; k < ev->n_cards && all; k++) {
            const uint64_t key = anim_event_key(ev->type, ev->cards[k], ev->from, ev->to, ev->seat);
            int at = -1;
            for (int i = 0; i < n_motions && at < 0; i++)
                if (out->fates[i] == CLIENT_FATE_PENDING && motion_key(&q->motions[i], me) == key) at = i;
            if (at < 0) all = 0; else mine[k] = (uint8_t)at;
        }
        if (all) {
            out->played[e] = 1;
            for (int k = 0; k < ev->n_cards; k++) out->fates[mine[k]] = CLIENT_FATE_SETTLED;
        } else {
            if (first < 0) first = e;
            last = e;
        }
    }

    // 3. What is left, judged on the board of the last event not played.
    uint8_t table_bound[CLIENT_CONFLICT_MAX_MOTIONS], hand_bound[CLIENT_CONFLICT_MAX_MOTIONS], pick[CLIENT_CONFLICT_MAX_MOTIONS];
    uint8_t verdicts[CLIENT_CONFLICT_MAX_MOTIONS];
    int n_table_bound = 0, n_hand_bound = 0, pass_pending = 0;
    for (int i = 0; i < n_motions; i++) {
        if (out->fates[i] != CLIENT_FATE_PENDING) continue;
        if (q->motions[i].dest == ANIM_DEST_MY_HAND) hand_bound[n_hand_bound++] = (uint8_t)i;
        else if (q->motions[i].dest == ANIM_DEST_TABLE) table_bound[n_table_bound++] = (uint8_t)i;
        pass_pending |= q->motions[i].is_pass;
    }
    if (last < 0 || n_table_bound + n_hand_bound == 0 || me < 0) return CLIENT_OK;
    out->open_event = (int8_t)last;
    if (q->open_event != last) return CLIENT_OK;
    if (!view_holds(open)) return CLIENT_E_FORMAT;

    int accepted = 0, lays = 0;
    for (int i = 0; i < n_table_bound; i++) accepted |= card_on_table(open, q->motions[table_bound[i]].card);
    for (int e = 0; e < n_events; e++) lays |= !out->played[e] && q->events[e].type == ANIM_EVT_ATTACK_PASS;

    // My pending pass, against the hand of the seat the shield lands on. A cover is no pass.
    if (pass_pending && pass_len > 0 && lays) {
        TableView passed = *open;
        if (client_optimistic_apply(c, &passed, pass_wire, pass_len) == CLIENT_OK) {
            int n = 0;
            for (int i = 0; i < n_table_bound; i++) if (!q->motions[table_bound[i]].is_cover) pick[n++] = table_bound[i];
            const ConflictHope hope = { .defender_seat = passed.defender, .uncovered_on_final = false, .pending_attacks = -1 };
            const int rc = conflict_verdicts(open, final, q, out->played, pick, n, &hope, verdicts);
            if (rc < 0) return rc;
            int doomed = 0;
            for (int i = 0; i < n; i++) if (verdicts[i] == ANIM_CONFLICT_REVERT) pick[doomed++] = pick[i];
            fly_home(out, pick, doomed);
            out->pass_home = doomed > 0;
        }
    }
    n_table_bound = still_pending(out, table_bound, n_table_bound);

    // The push moved the shield under my cards: to me, or to a hand that cannot take them.
    if (n_table_bound > 0 && open->defender != final->defender && lays) {
        int doomed = final->my_seat == final->defender;
        if (!doomed) {
            const ConflictHope hope = { .defender_seat = final->defender, .uncovered_on_final = false, .pending_attacks = -1 };
            const int rc = conflict_verdicts(open, final, q, out->played, table_bound, n_table_bound, &hope, verdicts);
            if (rc < 0) return rc;
            for (int i = 0; i < n_table_bound; i++) doomed |= verdicts[i] == ANIM_CONFLICT_REVERT;
        }
        if (doomed) {
            for (int i = 0; i < n_table_bound; i++) fly_home(out, &table_bound[i], 1);
            n_table_bound = 0;
        }
    }

    // A pickup of mine: what my hand on `open` does not hold goes back, and so does what the push's sweep overtakes.
    if (n_hand_bound > 0) {
        const ConflictHope hope = { .defender_seat = -1, .uncovered_on_final = false, .pending_attacks = 0 };
        const int rc = conflict_verdicts(open, final, q, out->played, hand_bound, n_hand_bound, &hope, verdicts);
        if (rc < 0) return rc;
        int n = 0;
        for (int i = 0; i < n_hand_bound; i++) if (verdicts[i] == ANIM_CONFLICT_REVERT) pick[n++] = hand_bound[i];
        for (int i = 0; i < n_hand_bound; i++) if (verdicts[i] == ANIM_CONFLICT_CLEAR) pick[n++] = hand_bound[i];
        fly_home(out, pick, n);
    }

    // My attacks and covers the board shows none of.
    if (n_table_bound > 0 && !accepted) {
        const ConflictHope hope = { .defender_seat = final->defender, .uncovered_on_final = true, .pending_attacks = -1 };
        const int rc = conflict_verdicts(open, final, q, out->played, table_bound, n_table_bound, &hope, verdicts);
        if (rc < 0) return rc;
        for (int i = 0; i < n_table_bound; i++) {
            if (verdicts[i] == ANIM_CONFLICT_CLEAR) out->fates[table_bound[i]] = CLIENT_FATE_SETTLED;
            else if (verdicts[i] == ANIM_CONFLICT_KEEP) out->fates[table_bound[i]] = CLIENT_FATE_KEEP;
            else fly_home(out, &table_bound[i], 1);
        }
    }
    if (out->n_flights == 0) return CLIENT_OK;

    // 4. The cards travel back the way they came: last flight first.
    {
        unsigned char doomed[CLIENT_CONFLICT_MAX_MOTIONS];
        int ones[CLIENT_CONFLICT_MAX_MOTIONS];
        uint8_t count[CLIENT_CONFLICT_MAX_MOTIONS], order[CLIENT_CONFLICT_MAX_MOTIONS], at[CLIENT_CONFLICT_MAX_MOTIONS];
        const int n = out->n_flights;
        for (int i = 0, sum = 0; i < n; i++) { doomed[i] = ANIM_CONFLICT_REVERT; ones[i] = 1; at[i] = (uint8_t)sum; sum += out->flight_count[i]; }
        AnimConflictPlan plan;
        const int steps = anim_reversal_order(doomed, n, ones, n, &plan);
        if (steps < 0) return steps;
        int n_order = 0;
        for (int i = 0; i < plan.n_order; i++) {
            const int f = plan.order[i];
            count[i] = out->flight_count[f];
            for (int k = 0; k < out->flight_count[f]; k++) order[n_order++] = out->order[at[f] + k];
        }
        memcpy(out->flight_count, count, (size_t)n);
        memcpy(out->order, order, (size_t)n_order);
    }
    out->home_event = (int8_t)first;
    for (int i = 0; i < out->n_order && out->magic_event < 0; i++) {
        if (q->motions[out->order[i]].dest != ANIM_DEST_MY_HAND) continue;
        for (int e = 0; e < n_events && out->magic_event < 0; e++)
            if (!out->played[e] && q->events[e].type == ANIM_EVT_MAGIC_TRANSITION) out->magic_event = (int8_t)e;
        break;
    }
    return CLIENT_OK;
}

// One edit of `v` with `n` cards.
static int edit_cards(ClientTable *c, TableView *v, int op, const Card *cards, int n) {
    BoardEdit e = { .op = (int8_t)op, .n_cards = (uint8_t)n, .target = CARD_NONE };
    memcpy(e.cards, cards, (size_t)n * sizeof(Card));
    return client_board_edit(c, v, &e);
}

int client_revert_board(ClientTable *c, TableView *v, const TableView *magic, const ClientConflict *q, const PushSettle *s) {
    if (!view_holds(v) || s->n_events != q->n_events || s->n_motions != q->n_motions || s->n_order > CLIENT_CONFLICT_MAX_MOTIONS
        || q->n_events > CLIENT_CONFLICT_MAX_EVENTS || q->n_motions > CLIENT_CONFLICT_MAX_MOTIONS) return CLIENT_E_FORMAT;
    for (int e = 0; e < q->n_events; e++) if (q->events[e].n_cards > MAX_HAND_SIZE) return CLIENT_E_FORMAT;
    const int8_t lead = v->first_attacker, shield = v->defender;
    Card to_table[CLIENT_CONFLICT_MAX_MOTIONS], to_hand[CLIENT_CONFLICT_MAX_MOTIONS];
    int n_to_table = 0, n_to_hand = 0, sweep = CLIENT_EVENT_NONE, rc = CLIENT_OK;
    for (int i = 0; i < s->n_order; i++) {
        if (s->order[i] >= q->n_motions) return CLIENT_E_FORMAT;
        const ConflictMotion *m = &q->motions[s->order[i]];
        if (m->dest == ANIM_DEST_MY_HAND) to_table[n_to_table++] = m->card; else to_hand[n_to_hand++] = m->card;
    }
    for (int e = 0; e < q->n_events && sweep < 0; e++)
        if (!s->played[e] && (q->events[e].type == ANIM_EVT_PICKUP || q->events[e].type == ANIM_EVT_CARDS_TO_TRASH)) sweep = e;

    // The table a pickup is laid back on.
    if (n_to_table > 0 && s->magic_event >= 0) {
        if (!magic || !view_holds(magic)) return CLIENT_E_FORMAT;
        *v = *magic;
    } else if (n_to_table > 0 && sweep >= 0) {
        rc = edit_cards(c, v, CLIENT_EDIT_TABLE, q->events[sweep].cards, q->events[sweep].n_cards);
    }
    const int pass_home = s->pass_home && n_to_hand > 0;
    if (rc == CLIENT_OK && pass_home) {
        const BoardEdit turn = { .op = CLIENT_EDIT_TURN, .first_attacker = lead, .defender = shield, .target = CARD_NONE };
        rc = client_board_edit(c, v, &turn);
    }
    // What leaves the table: the cards flying home, the push's own attacks, or both.
    const int lift_mine = n_to_table == 0, lift_theirs = n_to_table > 0 || !(sweep >= 0 && n_to_hand > 0);
    if (rc == CLIENT_OK && lift_mine) rc = edit_cards(c, v, CLIENT_EDIT_LIFT, to_hand, n_to_hand);
    for (int e = 0; e < q->n_events && rc == CLIENT_OK && lift_theirs; e++) {
        const ConflictEvent *ev = &q->events[e];
        if (s->played[e] || ev->type != ANIM_EVT_ATTACK_PASS || ev->from != ANIM_LOC_HAND) continue;
        rc = edit_cards(c, v, CLIENT_EDIT_LIFT, ev->cards, ev->n_cards);
    }
    if (rc == CLIENT_OK && pass_home) rc = edit_cards(c, v, CLIENT_EDIT_RETURN, to_hand, n_to_hand);
    return rc;
}

int client_keep_board(ClientTable *c, TableView *v, const ClientConflict *q, const PushSettle *s,
                      const uint8_t *pass_wire, int pass_len) {
    if (!view_holds(v) || s->n_motions != q->n_motions || q->n_motions > CLIENT_CONFLICT_MAX_MOTIONS) return CLIENT_E_FORMAT;
    int pass_stands = 0;
    for (int i = 0; i < q->n_motions; i++)
        pass_stands |= q->motions[i].is_pass && (s->fates[i] == CLIENT_FATE_PENDING || s->fates[i] == CLIENT_FATE_KEEP);
    TableView kept = *v;
    if (pass_stands && pass_len > 0 && client_optimistic_apply(c, &kept, pass_wire, pass_len) != CLIENT_OK) kept = *v;
    for (int i = 0; i < q->n_motions; i++) {
        if (s->fates[i] != CLIENT_FATE_KEEP) continue;
        BoardEdit e = { .op = CLIENT_EDIT_KEEP, .n_cards = 1, .target = q->motions[i].target };
        e.cards[0] = q->motions[i].card;
        if (client_board_edit(c, &kept, &e) != CLIENT_OK) return CLIENT_OK;   // refused: the board stays as it was
    }
    *v = kept;
    return CLIENT_OK;
}

// ---------- what a gesture on a board means -----------------------------------

int client_play(ClientTable *c, const TableView *v, const ClientGesture *g,
                const ClientPlayScratch *s, ClientPlay *out) {
    if (!c || !v || !g || !s || !s->moves || !s->wire || !out) return CLIENT_E_FORMAT;
    memset(out, 0, sizeof(*out));
    out->move_type = -1;
    out->best_cover = -1;
    if (g->n_cards < 0 || g->n_cards > MAX_MOVE_CARDS) return CLIENT_E_FORMAT;

    const int valid = board_game(c, v);
    if (valid != GAME_VALID) return valid;
    // A spectator makes no gesture: there is no seat whose menu this would be.
    if (v->my_seat < 0) return CLIENT_E_MISMATCH;
    const int seat = v->my_seat;

    // The published pair. The menu is the kernel's full legal set for the seat,
    // not play_human_menu's narrowing: the narrowing is play_can_say_good, which
    // is answered below, and every other rule here wants the whole set.
    calculate_legal_moves(rules_game(c), seat, s->moves);
    const int menu_len = legal_menu_write(s->moves, 0, -1, s->wire, s->wire_cap);
    if (menu_len == LEGAL_WIRE_ECAP) return CLIENT_E_CAP;
    if (menu_len < 0) return CLIENT_E_FORMAT;

    unsigned char table[2 * MAX_BATTLES];
    for (int i = 0; i < v->num_battles; i++) {
        table[2 * i] = (unsigned char)card_to_id(v->battles[i].attack);
        table[2 * i + 1] = card_is_none(v->battles[i].defense)
            ? (unsigned char)LEGAL_WIRE_NONE : (unsigned char)card_to_id(v->battles[i].defense);
    }
    const PlayBoard b = {
        .menu = s->wire, .menu_len = menu_len,
        .table = table, .n_battles = v->num_battles,
        .power_suit = v->power_suit,
        .my_seat = seat, .defender = v->defender,
        .my_status = v->seats[seat].status, .game_status = v->status,
    };

    unsigned char sel[MAX_MOVE_CARDS];
    for (int i = 0; i < g->n_cards; i++) sel[i] = (unsigned char)card_to_id(g->cards[i]);

    const uint64_t mask = play_coverable_battles(&b, sel, g->n_cards);
    for (int i = 0; i < v->num_battles && i < 64; i++)
        if (mask & ((uint64_t)1 << i)) out->coverable[out->n_coverable++] = (int8_t)i;
    out->best_cover = (int8_t)play_best_cover_target(&b, sel, g->n_cards);
    out->can_say_good = play_can_say_good(&b) != 0;
    out->pills = (uint8_t)play_pills(&b, sel, g->n_cards, g->host);

    // The Cover button aims itself; every other gesture named its own target. A
    // button with nothing to aim at resolves to nothing rather than falling
    // through to PLAY_TARGET_TABLE, which -1 would otherwise mean.
    int target = g->target;
    if (target == CLIENT_PLAY_COVER_BUTTON) {
        if (out->best_cover < 0) return CLIENT_OK;
        target = out->best_cover;
    }
    const int idx = play_resolve(&b, sel, g->n_cards, target);
    if (idx < 0) return CLIENT_OK;

    MenuWalk w;
    MenuMove m;
    if (legal_menu_begin(&w, b.menu, b.menu_len) < 0) return CLIENT_OK;
    while (legal_menu_next(&w, &m) == 1) {
        if (w.index != idx) continue;
        if (m.n_cards > MAX_MOVE_CARDS) return CLIENT_E_CAP;
        out->move_type = (int8_t)m.type;
        out->n_cards = (int8_t)m.n_cards;
        // A move that covers nothing names no attack. legal_menu_write pads the
        // attack bytes of a non-cover with card_to_id of a zeroed Card, which is
        // no card at all, so anything but a cover's real id is the no-card here
        // rather than a byte decoded into a suit that does not exist.
        for (int i = 0; i < m.n_cards; i++) {
            out->cards[i] = card_of_id(m.cards[i]);
            out->attack_cards[i] = m.type == MOVE_COVER && m.attacks[i] <= 51
                ? card_of_id(m.attacks[i]) : CARD_NONE;
        }
        return CLIENT_OK;
    }
    return CLIENT_OK;
}
