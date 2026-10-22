// board.c - board.h.
#include "board.h"

#include "lib_client.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

static be_w_card wcard(Card c) {
    be_w_card w = { (int8_t)c.suit, (int8_t)c.value };
    return w;
}

static bool all_covered(const TableView *v) {
    for (int i = 0; i < v->num_battles; i++)
        if (card_is_none(v->battles[i].defense)) return false;
    return true;
}

void be_board_fill(be_world *wd, be_table *bt, const TableView *v, int n_seats, uint32_t moves) {
    be_w_table *w = &bt->w;
    w->n_seats = (uint8_t)n_seats;
    w->num_players = (uint8_t)v->num_players;
    w->podkidnoy = (uint8_t)!v->passing;
    w->power_suit = v->power_suit;
    w->has_flipped = (uint8_t)v->has_flipped;
    w->flipped = wcard(v->flipped);
    w->first_attacker = v->first_attacker;
    w->defender = v->defender;
    w->fool = v->fool;
    int nb = v->num_battles > BE_BATTLES ? BE_BATTLES : v->num_battles;
    w->num_battles = (uint8_t)nb;
    for (int i = 0; i < nb; i++) {
        w->attack[i] = wcard(v->battles[i].attack);
        w->defense[i] = wcard(v->battles[i].defense);
    }
    w->deck_count = v->deck_count;
    w->discard_count = v->discard_pile_length;
    w->version = v->version;
    w->moves = moves;
    bool covered = all_covered(v);
    for (int i = 0; i < v->num_players && i < BE_SEATS; i++) {
        const ViewSeat *s = &v->seats[i];
        be_w_seat *ws = &w->seats[i];
        int nl = s->name_len > BE_NAME ? BE_NAME : s->name_len;
        ws->name_len = (uint8_t)nl;
        memcpy(ws->name, s->name, (size_t)nl);
        ws->status = (uint8_t)s->status;
        ws->hand_count = (uint8_t)(s->hand_count < 0 ? 0 : s->hand_count);
        ws->flags = s->is_ai ? BE_SEAT_BOT : BE_SEAT_HUMAN_SIM;
        if (v->good_mask & (1u << i)) ws->flags |= BE_SEAT_GOOD;
        if (s->status == PLAYER_STATUS_OUT) ws->flags |= BE_SEAT_OUT;
        if (v->fool == i) ws->flags |= BE_SEAT_FOOL;
        if (turn_may_act(v->status, s->status, i, v->num_battles, covered, v->first_attacker, v->defender, v->good_mask))
            ws->flags |= BE_SEAT_TO_MOVE;
        ws->brain_len = 0;
        // A bot's seat: its strategy key, as GET /v1/bots said it.
        int b = s->is_ai ? be_bots_by_id(s->id, s->id_len) : -1;
        if (b >= 0) {
            size_t kl = strlen(be_bots_at(b)->key);
            if (kl > sizeof ws->brain) kl = sizeof ws->brain;
            ws->brain_len = (uint8_t)kl;
            memcpy(ws->brain, be_bots_at(b)->key, kl);
        }
    }
    uint8_t phase = v->status == GAME_STATUS_PLAYING ? BE_PHASE_PLAYING : v->status == GAME_STATUS_GAME_OVER ? BE_PHASE_OVER : BE_PHASE_LOBBY;
    if (bt->w.phase != BE_PHASE_GONE) be_world_set_phase(wd, bt, phase);
    be_world_changed(wd, bt);
}

int be_board_event_kind(const PushEvent *e) {
    switch (e->type) {
    case EVW_T_DEAL: return BE_EV_DEALT;
    case EVW_T_ATTACK_PASS: return e->msg == EVW_MSG_PASSED ? BE_EV_PASS : BE_EV_ATTACK;
    case EVW_T_COVER: return BE_EV_COVER;
    case EVW_T_PICKUP: return BE_EV_PICKUP;
    case EVW_T_DISCARD: return BE_EV_GOOD;
    case EVW_T_OUT: return BE_EV_OUT;
    default: return 0;
    }
}

bool be_move_raced(const TableResponse *r, uint32_t intent) {
    if (r->status == TABLE_STATUS_MOOT) return true;
    if (r->status != TABLE_STATUS_REJECTED) return false;
    return r->reject_code == TABLE_REJECT_STALE_ROUND || r->version > intent;
}

// ---- the bots ----------------------------------------------------------------------------------

static be_bot g_bots[BE_BOTS_MAX];
static int    g_n;

int be_bots_n(void) { return g_n; }
const be_bot *be_bots_at(int i) { return i >= 0 && i < g_n ? &g_bots[i] : NULL; }

int be_bots_by_name(const char *name) {
    for (int i = 0; i < g_n; i++)
        if (strcmp(g_bots[i].name, name) == 0) return i;
    return -1;
}

int be_bots_by_id(const char *id, int id_len) {
    for (int i = 0; i < g_n; i++)
        if ((int)strlen(g_bots[i].id) == id_len && memcmp(g_bots[i].id, id, (size_t)id_len) == 0) return i;
    return -1;
}

int be_bots_fetch(int port, const char *token) {
    char headers[NW_ACCESS_MAX + 64];
    headers[0] = 0;
    if (token && *token) snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\n", token);
    tc_response r;
    if (tc_http(port, "GET", "/v1/bots", headers[0] ? headers : NULL, NULL, 0, &r) != 0) return -1;
    int n = r.status == 200 ? nw_list_open(r.body.p, (int)r.body.len, NW_BOTS) : -1;
    int got = 0;
    for (int i = 0; i < n && got < BE_BOTS_MAX; i++) {
        NwBot b;
        if (nw_get_bot(r.body.p, (int)r.body.len, i, &b) != NW_OK) break;
        be_bot *o = &g_bots[got++];
        snprintf(o->id, sizeof o->id, "%.*s", (int)b.id_len, b.id);
        snprintf(o->key, sizeof o->key, "%.*s", (int)b.key_len, b.key);
        snprintf(o->name, sizeof o->name, "%.*s", (int)b.nickname_len, b.nickname);
    }
    int status = r.status;
    tc_response_free(&r);
    if (n < 0) {
        CB_WARN("birdseye", "GET /v1/bots answered %d", status);
        return -1;
    }
    g_n = got;
    return got;
}
