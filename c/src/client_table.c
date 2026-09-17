// client_table.c - the web client's slot. See client_table.h.
#include "client_table.h"
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

// The slot's board, as `viewer` sees it, joined to the roster when there is one.
static void view_fill(ClientTable *c, int viewer, int status) {
    const Game *g = c->g;
    TableView *v = &c->view;
    memset(v, 0, sizeof(*v));
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
    v->version = c->version;
    memcpy(v->battles, g->table_battles, sizeof(Battle) * (size_t)g->num_battles);
    memcpy(v->elimination, g->elimination_order, (size_t)g->num_eliminated);
    for (int s = 0; s < g->num_players; s++) {
        ViewSeat *vs = &v->seats[s];
        vs->status = g->players[s].status;
        vs->hand_count = g->players[s].hand_count;
        vs->awaiting_attack = s == viewer && g->players[s].awaiting_attack;
        if (!c->has_roster) continue;
        vs->is_ai = ((c->ai_mask >> s) & 1u) != 0;
        put_text(vs->id, &vs->id_len, c->r.seats[s].id, c->r.seats[s].id_len);
        put_text(vs->name, &vs->name_len, c->r.seats[s].name, c->r.seats[s].name_len);
    }
    if (viewer >= 0) {
        v->my_hand_count = g->players[viewer].hand_count;
        memcpy(v->my_hand, g->players[viewer].hand, (size_t)v->my_hand_count);
    }
    if (c->has_roster) {
        put_text(v->game_id, &v->gid_len, c->gid, c->gid_len);
        put_text(v->title, &v->title_len, c->r.title, c->r.title_len);
    }
}

// A masked board off the wire into the slot: measured whole, then imported
// (game_validate). CLIENT_OK, or CLIENT_E_STATE with the reason in detail.
static int board_import(ClientTable *c, const uint8_t *p, int len) {
    if (state_measure(p, len) != len) { c->detail = GAME_INVALID_COUNT; return CLIENT_E_STATE; }
    const int v = state_import(c->g, p, 1);
    if (v != GAME_VALID) { c->detail = v; return CLIENT_E_STATE; }
    return CLIENT_OK;
}

// A table's identity as a roster trailer carries it.
typedef struct {
    Roster   r;
    uint32_t ai;
    int      status, gid_len;
    char     gid[ROSTER_GAME_ID_MAX + 1];
} Identity;

// A roster trailer that must fill exactly `len` bytes. CLIENT_OK, or `refusal`
// with the ROSTER_E_* in detail.
static int identity_read(ClientTable *c, Identity *id, const uint8_t *p, int len, int refusal) {
    int used = 0;
    const int rc = roster_trailer_read(&id->r, id->gid, &id->gid_len, &id->status, &id->ai, p, len, &used);
    if (rc != ROSTER_OK) { c->detail = rc; return refusal; }
    if (used != len) { c->detail = ROSTER_E_PADDING; return refusal; }
    return CLIENT_OK;
}

static void identity_adopt(ClientTable *c, const Identity *id) {
    c->r = id->r;
    c->ai_mask = id->ai;
    memcpy(c->gid, id->gid, (size_t)id->gid_len);
    c->gid[id->gid_len] = 0;
    c->gid_len = (uint8_t)id->gid_len;
    c->has_roster = true;
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

    // The identity is kept only from an envelope that reads whole.
    Identity id;
    int rc;
    if ((rc = identity_read(c, &id, p + trailer, len - trailer, CLIENT_E_TRAILER)) != CLIENT_OK) return rc;
    if ((rc = board_import(c, p + q + 2, view_len - 2)) != CLIENT_OK) return rc;
    if (c->g->num_players < 1 || id.r.n != c->g->num_players || seat >= c->g->num_players) return CLIENT_E_MISMATCH;
    identity_adopt(c, &id);
    const int status = id.status;
    c->version = (uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 24);
    view_fill(c, seat, status);
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
    int seq = len, flags = 0, block = 0, rc;
    Identity id;
    if (as3 && evwire_as3_split(p, len, &seq, &flags, &block) != 0) return CLIENT_E_PUSH;
    const int named = (flags & EVW_AS3_ROSTER) || identity_len > 0;
    if (flags & EVW_AS3_ROSTER) {
        if ((rc = identity_read(c, &id, p + block, len - block, CLIENT_E_TRAILER)) != CLIENT_OK) return rc;
    } else if (identity_len > 0) {
        if ((rc = identity_read(c, &id, identity, identity_len, CLIENT_E_IDENTITY)) != CLIENT_OK) return rc;
    }

    EvwHeader h;
    const unsigned char *fin = 0;
    int fin_len = 0;
    PushCheck k = { -1, 1 };
    const int n = evwire_read(p, seq, &h, &fin, &fin_len, push_check, &k);
    if (n < 0 || !k.ok || (int)(fin - p) + fin_len != seq) return CLIENT_E_PUSH;
    const int seats = board_seats(fin, fin_len);
    if (seats < 1 || (k.n_seats >= 0 && seats != k.n_seats)) return CLIENT_E_PUSH;
    if ((named && id.r.n != seats) || h.viewer >= seats) return CLIENT_E_MISMATCH;

    if (named) identity_adopt(c, &id);
    else c->has_roster = false;
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
    return CLIENT_OK;
}

int client_identity_seat(ClientTable *c, const char *id, int id_len, const char *name, int name_len, int is_ai) {
    if (!c->has_roster) return CLIENT_E_ORDER;
    const int s = roster_seat_add(&c->r, id, id_len, name, name_len, "", 0);
    if (s < 0) { c->detail = s; return CLIENT_E_IDENTITY; }
    if (is_ai) c->ai_mask |= 1u << s;
    return CLIENT_OK;
}
