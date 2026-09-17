// client_table.c - the web client's slot. See client_table.h.
#include "client_table.h"
#include "anim_plan.h"
#include "awire.h"
#include "evwire.h"
#include "view.h"
#include "../wasm/wire.h"
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
    v->version = c->version;
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

// A masked board off the wire into the slot: measured whole, then read and
// judged exactly as state_import does (state_get, game_validate). The slot is not
// saved and put back on a refusal, as state_import would: after a refusal it is
// not read, and every read here would pay the copy.
static int board_import(ClientTable *c, const uint8_t *p, int len) {
    if (state_measure(p, len) != len) { c->detail = GAME_INVALID_COUNT; return CLIENT_E_STATE; }
    int v = state_get(c->g, p, 1);
    if (v == GAME_VALID) v = game_validate(c->g, GAME_VALIDATE_MASKED);
    if (v != GAME_VALID) { c->detail = v; return CLIENT_E_STATE; }
    return CLIENT_OK;
}

// A roster trailer that must fill exactly `len` bytes, read straight into the
// slot's roster (a refused read leaves the slot naming no one). CLIENT_OK, or
// `refusal` with the ROSTER_E_* in detail.
static int identity_read(ClientTable *c, const uint8_t *p, int len, int *status, int refusal) {
    int used = 0, gl = 0;
    c->has_roster = false;
    const int rc = roster_trailer_read(&c->r, c->gid, &gl, status, &c->ai_mask, p, len, &used);
    if (rc != ROSTER_OK) { c->detail = rc; return refusal; }
    if (used != len) { c->detail = ROSTER_E_PADDING; return refusal; }
    c->gid_len = (uint8_t)gl;
    c->gid[gl] = 0;
    return CLIENT_OK;
}

// ---------- the envelope --------------------------------------------------------

#define ENV_FORMAT      1
#define ENV_FLAG_SEATED 0x01
#define ENV_FLAG_ROSTER 0x02

int client_adopt_envelope(ClientTable *c, const uint8_t *p, int len) {
    c->open = false;
    c->detail = 0;
    if (!p || len < 13 || p[0] != ENV_FORMAT) return CLIENT_E_FORMAT;
    const int flags = p[1];
    if ((flags & ~(ENV_FLAG_SEATED | ENV_FLAG_ROSTER)) || !(flags & ENV_FLAG_ROSTER)) return CLIENT_E_FORMAT;
    const int seated = (flags & ENV_FLAG_SEATED) != 0;
    const int seat = seated ? p[2] : -1;
    if (seated ? seat >= MAX_PLAYERS : p[2] != 0xFF) return CLIENT_E_FORMAT;
    // The retired JSON roster island (view.ts): empty, but skipped if a server wrote one.
    int q = 9 + (p[7] | (p[8] << 8));
    if (q + 2 > len) return CLIENT_E_FORMAT;
    const int view_len = p[q] | (p[q + 1] << 8);
    q += 2;
    if (view_len < 2 || q + view_len > len) return CLIENT_E_FORMAT;
    if (p[q] != VIEW_FORMAT_VERSION || p[q + 1] != (seated ? seat : 0xFF)) return CLIENT_E_FORMAT;
    const int trailer = q + view_len;
    if (trailer >= len) return CLIENT_E_TRAILER;

    // The slot names the table only once the envelope has read whole.
    int rc, status = 0;
    c->identity_at = -1;
    if ((rc = identity_read(c, p + trailer, len - trailer, &status, CLIENT_E_TRAILER)) != CLIENT_OK) return rc;
    if ((rc = board_import(c, p + q + 2, view_len - 2)) != CLIENT_OK) return rc;
    if (c->g->num_players < 1 || c->r.n != c->g->num_players || seat >= c->g->num_players) return CLIENT_E_MISMATCH;
    c->has_roster = true;
    c->identity_at = trailer;
    c->version = (uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 24);
    view_fill(c, seat, status);
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
    const int snap_len = q[at] | (q[at + 1] << 8);
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
    const int n = roster_trailer_write_ai(&c->r, c->gid, c->gid_len, 0, 0, c->ai_mask, out, cap);
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

int client_view_rules(const TableView *v, int from_deck, int to_flipped, ViewRules *out) {
    const int n = v->num_players;
    if (n < 0 || n > MAX_PLAYERS || v->num_battles < 0 || v->num_battles > MAX_BATTLES
        || v->my_seat < -1 || v->my_seat >= n || from_deck < 0 || to_flipped < 0) return CLIENT_E_FORMAT;
    memset(out, 0, sizeof(*out));

    // The deal lays the stock out before it turns the trump: until then nobody
    // leads or defends anything yet.
    const bool dealt = !(v->deck_count > 0 && !v->has_flipped);
    out->first_attacker_badge = (int8_t)(dealt && v->num_battles == 0 && v->first_attacker >= 0 && v->first_attacker < n
                                         ? v->first_attacker : -1);
    out->defender_badge = (int8_t)(dealt && v->defender >= 0 && v->defender < n ? v->defender : -1);

    // Good is handle_good's to allow (a playing game, a seat still in, not the
    // defender, not said already), and it is offered once the bout could close
    // on it: every attack on the table covered.
    const int me = v->my_seat;
    bool covered = v->num_battles > 0;
    for (int i = 0; covered && i < v->num_battles; i++) covered = !card_is_none(v->battles[i].defense);
    out->can_say_good = me >= 0 && v->status == GAME_STATUS_PLAYING && v->seats[me].status == PLAYER_STATUS_IN
        && me != v->defender && !((v->good_mask >> me) & 1u) && covered;

    for (int s = 0; s < n && !out->bot_to_move; s++)
        out->bot_to_move = v->seats[s].is_ai && turn_may_act(v->status, v->seats[s].status, s, v->num_battles, covered,
                                                             v->first_attacker, v->defender, v->good_mask);

    // The stock: what is flying out of it has left the pile, and a card on its
    // way to the trump's slot is still the stock's, so it stays on the count.
    const int pile = v->deck_count - from_deck;
    out->deck_pile = (int16_t)(pile > 0 ? pile : 0);
    out->deck_badge = (int16_t)(out->deck_pile + (v->has_flipped ? 1 : 0) + to_flipped);
    out->show_deck_pile = out->deck_pile > 0;
    out->show_flipped_slot = out->show_deck_pile || v->has_flipped || to_flipped > 0;
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
            int defender = v->defender;
            if (a.kind == AWIRE_PASS) {
                const int valid = board_game(c, v);
                if (valid != GAME_VALID) { c->detail = valid; return CLIENT_E_STATE; }
                defender = get_next_player_index(rules_game(c), v->defender);
            }
            if (v->num_battles + a.n > MAX_BATTLES) return CLIENT_E_CAP;
            for (int i = 0; i < a.n; i++) v->battles[v->num_battles++] = (Battle){ .attack = a.cards[i], .defense = CARD_NONE };
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

static int uncovered(const TableView *v) {
    int n = 0;
    for (int b = 0; b < v->num_battles; b++) n += card_is_none(v->battles[b].defense);
    return n;
}

// A card as the conflict rule names it: its dense id, or ANIM_CARD_NONE for a
// back or no card at all.
static int conflict_id(Card c) {
    return c.suit >= 0 && c.suit < NUM_SUITS && c.value >= 1 && c.value <= ACE_VALUE ? card_to_id(c) : ANIM_CARD_NONE;
}

int client_conflict_verdicts(const TableView *open, const TableView *final, const ClientConflict *q, ConflictVerdicts *out) {
    if (!view_holds(open) || !view_holds(final) || q->n_events > CLIENT_CONFLICT_MAX_EVENTS
        || q->n_motions > CLIENT_CONFLICT_MAX_MOTIONS) return CLIENT_E_FORMAT;
    out->n = 0;
    // Module storage, like the slot: a host asks one question at a time.
    static AnimEvent events[CLIENT_CONFLICT_MAX_EVENTS];
    static int moved[CLIENT_CONFLICT_MAX_EVENTS * MAX_HAND_SIZE];
    for (int e = 0; e < q->n_events; e++) {
        const ConflictEvent *ce = &q->events[e];
        if (ce->n_cards > MAX_HAND_SIZE) return CLIENT_E_FORMAT;
        events[e] = (AnimEvent){ .type = ce->type, .seat = ANIM_SEAT_NONE, .from = ANIM_LOC_NONE, .to = ANIM_LOC_NONE,
                                 .cards = ce->cards, .n_cards = ce->n_cards, .mask_cards = ce->masked ? 1 : 0 };
    }
    int table_cleared = 0;
    const int n_moved = anim_conflict_sweep(events, q->n_events, moved, (int)(sizeof moved / sizeof moved[0]), &table_cleared);
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

    int pending = q->pending_attacks;
    if (pending < 0) {
        pending = 0;
        for (int i = 0; i < q->n_motions; i++) pending += !q->motions[i].is_cover;
    }
    AnimServerHope hope;
    hope.table_cleared = table_cleared;
    hope.pending_attacks = pending;
    hope.defender_hand = q->defender_seat >= 0 && q->defender_seat < final->num_players ? final->seats[q->defender_seat].hand_count : 0;
    hope.final_uncovered = uncovered(q->uncovered_on_final ? final : open);
    for (int i = 0; i < q->n_motions; i++) {
        hope.is_cover = q->motions[i].is_cover;
        const int v = anim_conflict_verdict(conflict_id(q->motions[i].card), q->motions[i].dest, &facts, &hope);
        if (v < 0) return v;
        out->verdicts[i] = (uint8_t)v;
    }
    out->n = q->n_motions;
    return q->n_motions;
}
