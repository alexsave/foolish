// replay_move.c - see replay_move.h.
#include "replay_move.h"

#include "legal.h"
#include "thread_heap.h"

#include <string.h>

// The menu, per thread (thread_heap.h): REPLAY_MOVE_MENU_CAP moves, taken on a
// thread's first move and freed when it exits.
static _Thread_local LegalMove *g_menu;

static int menu_of(const Game *g, int seat) {
    if (!g_menu) g_menu = thread_heap_calloc(REPLAY_MOVE_MENU_CAP, sizeof *g_menu);
    if (!g_menu) return -REPLAY_ECAP;
    return legal_moves_into(g, seat, g_menu, REPLAY_MOVE_MENU_CAP);
}

// The menu's move as awire's action (the kinds are the same numbers: legal.h
// MOVE_* and awire.h AWIRE_*). false: a kind or a length awire has no frame for.
static bool action_of(const LegalMove *m, AwireAction *a) {
    if (m->type < AWIRE_ATTACK || m->type > AWIRE_GOOD || m->n_cards < 0 || m->n_cards > AWIRE_MAX_CARDS) return false;
    memset(a, 0, sizeof *a);
    a->kind = m->type;
    a->n = m->n_cards;
    for (int i = 0; i < m->n_cards; i++) {
        a->cards[i] = m->cards[i];
        if (m->type == MOVE_COVER) a->attacks[i] = m->attack_cards[i];
    }
    return true;
}

static bool same_action(const AwireAction *x, const AwireAction *y) {
    if (x->kind != y->kind || x->n != y->n) return false;
    for (int i = 0; i < x->n; i++) {
        if (!card_eq(x->cards[i], y->cards[i])) return false;
        if (x->kind == AWIRE_COVER && !card_eq(x->attacks[i], y->attacks[i])) return false;
    }
    return true;
}

int replay_move_of(const Game *g, int seat, const uint8_t *wire, int wire_len, ReplayMove *out) {
    memset(out, 0, sizeof *out);
    AwireAction want;
    if (seat < 0 || seat >= MAX_PLAYERS || wire_len < 0 || wire_len > REPLAY_MOVE_WIRE_MAX || !awire_decode(wire, wire_len, &want))
        return -REPLAY_EINPUT;
    out->seat = seat;
    int n = menu_of(g, seat);
    if (n < 0) return n;
    out->menu_n = n;
    out->index = n;
    for (int i = 0; i < n; i++) {
        AwireAction a;
        if (action_of(&g_menu[i], &a) && same_action(&a, &want)) {
            out->index = i;
            return 0;
        }
    }
    memcpy(out->wire, wire, (size_t)wire_len);   // not on the menu as sent: the move whole
    out->wire_len = wire_len;
    return 0;
}

int replay_move_code(ReplayCoder *c, ReplayMove *m) {
    const bool enc = c->encode;
    m->seat = replay_coder_uniform(c, MAX_PLAYERS, enc ? m->seat : -1);
    m->menu_n = (int)replay_coder_varint(c, enc ? (uint32_t)m->menu_n : 0);
    if (!c->err && (m->menu_n < 0 || m->menu_n > REPLAY_MOVE_MENU_CAP)) c->err = REPLAY_EINPUT;
    if (c->err) return -c->err;
    m->index = replay_coder_uniform(c, m->menu_n + 1, enc ? m->index : -1);
    if (!c->err && m->index == m->menu_n) {
        m->wire_len = (int)replay_coder_varint(c, enc ? (uint32_t)m->wire_len : 0);
        if (!c->err && (m->wire_len < 0 || m->wire_len > REPLAY_MOVE_WIRE_MAX)) c->err = REPLAY_EINPUT;
        for (int i = 0; !c->err && i < m->wire_len; i++) m->wire[i] = (uint8_t)replay_coder_uniform(c, 256, enc ? m->wire[i] : -1);
    } else if (!enc) {
        m->wire_len = 0;
    }
    return c->err ? -c->err : 0;
}

int replay_move_wire(const Game *g, const ReplayMove *m, uint8_t *wire, int cap) {
    if (m->seat < 0 || m->seat >= MAX_PLAYERS || m->index < 0 || m->index > m->menu_n) return -REPLAY_EINPUT;
    int n = menu_of(g, m->seat);
    if (n < 0) return n;
    if (n != m->menu_n) return -REPLAY_ENOTINMENU;
    if (m->index == m->menu_n) {
        if (m->wire_len > cap) return -REPLAY_ECAP;
        memcpy(wire, m->wire, (size_t)m->wire_len);
        return m->wire_len;
    }
    AwireAction a;
    if (!action_of(&g_menu[m->index], &a)) return -REPLAY_EINPUT;
    int len = awire_encode(&a, wire, cap);
    return len > 0 ? len : -REPLAY_EINPUT;
}

// awire's frame of a menu move. 0: none fits.
static int wire_of(const LegalMove *m, uint8_t *wire, int cap) {
    AwireAction a;
    return action_of(m, &a) ? awire_encode(&a, wire, cap) : 0;
}

int replay_move_drive_walk(Game *g, uint32_t rng_base, uint32_t log_offset, int n, BotDriveAction *acts,
                           ReplayMove *mv, bool encode) {
    if (n < 0 || n > BOT_DRIVE_MAX_ACTIONS) return -REPLAY_EINPUT;
    void (*const prev)(const Game *, int, int) = engine_snap_hook;
    engine_snap_hook = 0;   // a scratch board records nothing
    int rc = 0;
    for (int k = 0; rc == 0 && k < n; k++) {
        uint8_t wire[REPLAY_MOVE_WIRE_MAX];
        int len;
        if (encode) {
            len = wire_of(&acts[k].move, wire, sizeof wire);
            rc = len > 0 ? replay_move_of(g, acts[k].seat, wire, len, &mv[k]) : -REPLAY_EINPUT;
        } else {
            AwireAction a;
            len = replay_move_wire(g, &mv[k], wire, sizeof wire);
            if (len < 0) rc = len;
            else if (!awire_decode(wire, len, &a)) rc = -REPLAY_EINPUT;
            else {
                memset(&acts[k], 0, sizeof acts[k]);
                acts[k].seat = (int8_t)mv[k].seat;
                acts[k].move.type = (int8_t)a.kind;
                acts[k].move.n_cards = (int8_t)a.n;
                for (int c = 0; c < a.n && c < MAX_MOVE_CARDS; c++) {
                    acts[k].move.cards[c] = a.cards[c];
                    if (a.kind == AWIRE_COVER) acts[k].move.attack_cards[c] = a.attacks[c];
                }
                if (a.n > MAX_MOVE_CARDS) rc = -REPLAY_EINPUT;
            }
        }
        if (rc) break;
        bot_drive_seed_decision(g, rng_base, log_offset, BOT_DRIVE_PHASE_APPLY);
        if (!legal_move_apply(g, acts[k].seat, &acts[k].move)) rc = -REPLAY_ENOTINMENU;
        else if (game_done(g) >= 0) game_settle_status(g);
    }
    engine_snap_hook = prev;
    return rc;
}
