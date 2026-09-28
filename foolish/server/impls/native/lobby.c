// The lobby verbs - see lobby.h.
#define _GNU_SOURCE
#include "lobby.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "bot_drive.h"
#include "bot_roster.h"
#include "bots.h"       // start_bot_loop: the game-loop paces bot play from the deal on
#include "ctl_wire.h"
#include "game.h"
#include "push.h"       // epoll_notify_game_changed: a lobby transition is a state change too
#include "registry.h"
#include "session.h"
#include "srvthread.h"  // next_rand: seasoning for the deal seed
#include "strategy.h"   // STRAT_RANDOM - see h_meta's add-bot branch (Stage 4 strategy_key fix)

void h_create(Req *r, Conn *conn) {
    if (!ratelimit_allow(client_ip_key(r, conn))) { respond_ctl_error(conn, 429, CTL_ERR_RATE_LIMITED); return; }
    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(r->token);
    if (!u) { pthread_mutex_unlock(&g_registry_lock); respond_ctl_error(conn, 401, CTL_ERR_AUTH); return; }
    char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
    char username[24]; snprintf(username, sizeof username, "%s", u->username);
    // A user recovered from an older row may carry a name the roster refuses.
    if (!roster_name_ok(username)) { pthread_mutex_unlock(&g_registry_lock); respond_ctl_error(conn, 400, CTL_ERR_USERNAME); return; }

    int gidx;
    GameSlot *s = game_alloc_slot(&gidx);           // cleared slot with lock/cond ready (reused or fresh); bounded memory
    if (!s) { pthread_mutex_unlock(&g_registry_lock); respond_ctl_error(conn, 400, CTL_ERR_FULL); return; }
    s->used = true;
    s->last_active_us = now_us();
    // s->version starts at 0 (memset); view_cache_version must start at a
    // value that can NEVER equal it, or state_put_cached's very first call
    // for a seat would see version==cache_version (both zeroed) and return
    // the also-zeroed, never-computed view_cache_len (0 bytes) instead of
    // actually serializing - a silent "client gets an empty state" bug. The
    // +1 covers the shared spectator cache slot too (index MAX_PLAYERS -
    // Stage 4, SPECTATOR_CACHE_IDX).
    for (int i = 0; i < MAX_PLAYERS + 1; i++) s->view_cache_version[i] = (uint32_t)-1;
    gen_id(s->id, ID_LEN);
    // LOCK ORDER: registry, then this fresh slot's own lock - never the
    // reverse (see registry.h's "Locking" block). No other thread can find
    // this slot before game_ht_insert runs, so taking s->lock here is
    // uncontended; it's only here for symmetry with every other handler's
    // registry->game handoff, so nothing outside this function ever touches
    // a GameSlot without its lock held.
    pthread_mutex_lock(&s->lock);
    game_ht_insert(s);
    pthread_mutex_unlock(&g_registry_lock);   // registry work done; the rest is s->lock-only

    snprintf(s->owner, sizeof s->owner, "%s", user_id);
    // Seat 0 = creator, seated by the same kernel call every other join uses.
    // Identity lives here; the board is dealt at start.
    Game *g = &s->game;
    g->status = GAME_STATUS_WAITING;   // a fresh slot is zeroed, so say what it is
    seat_add(s, STRATEGY_KEY_HUMAN, user_id, username, "");   // the name passed roster_name_ok above
    game_mark_dirty(s);
    unsigned char out[CTL_FRAME_MAX];
    int n = ctl_enc_game(s->id, out, (int)sizeof out);
    pthread_mutex_unlock(&s->lock);
    respond_ctl(conn, 200, out, n);
}

void h_meta(Req *r, Conn *conn) {
    // The verb and its arguments are one packed frame (ctl_wire.h CTL_META).
    // A body that does not decode leaves `m` zeroed, which is exactly what a
    // JSON body missing its keys used to leave behind: no verb matches, and
    // the empty game_id finds no game.
    CtlMeta m;
    memset(&m, 0, sizeof m);
    ctl_dec_meta((const unsigned char *)r->body, r->body_len, &m);

    pthread_mutex_lock(&g_registry_lock);
    User *u = user_by_token(r->token);
    GameSlot *s = game_by_id(m.game_id);
    if (!u) { pthread_mutex_unlock(&g_registry_lock); respond_ctl_error(conn, 401, CTL_ERR_AUTH); return; }
    if (!s) { pthread_mutex_unlock(&g_registry_lock); respond_ctl_error(conn, 404, CTL_ERR_NO_GAME); return; }
    char user_id[ID_LEN + 1]; snprintf(user_id, sizeof user_id, "%s", u->user_id);
    char username[24]; snprintf(username, sizeof username, "%s", u->username);
    pthread_mutex_lock(&s->lock);
    pthread_mutex_unlock(&g_registry_lock);

    Game *g = &s->game;
    if (m.verb == CTL_META_JOIN) {
        // Whether there is room, and what a human seat starts as, are the
        // kernel's (game_lobby_seat). Identity is this server's.
        // A second join by a seated player is refused by the roster itself
        // (ROSTER_E_DUPLICATE), before the kernel is asked.
        seat_add(s, STRATEGY_KEY_HUMAN, user_id, username, "");
    } else if (m.verb == CTL_META_ADD_BOT) {
        const char *skey = m.strategy[0] ? m.strategy : "random";
        int ridx = bot_roster_find(skey);
        if (ridx < 0) ridx = bot_roster_find("random");
        const BotRosterEntry *entry = bot_roster_at(ridx);
        // Same seating rule, the other kind: game_lobby_seat lands a bot READY
        // because a bot always is. That asymmetry used to be written out here.
        //
        // The kind is the roster entry's OWN `.strat` - a STRAT_* brain id by
        // kernel-wide convention (strategy.h), NOT the roster ARRAY INDEX
        // bot_roster_find returns. Passing the index here once meant
        // bot_drive.c read it back as a brain: a seat that matched no entry
        // never moved at all (octogen), and one that collided with another
        // entry's id silently played that bot instead (gunpowder's index 6 is
        // STRAT_BLACKPOWDER).
        //
        // The bot's roster seat: id "bot<seat>" (seats are only appended, so
        // unique), a "%<key> <seat>" name, and the entry's own key as its brain.
        const int seat = g->num_players;
        char bot_id[16], bot_name[48];
        snprintf(bot_id, sizeof bot_id, "bot%d", seat);
        snprintf(bot_name, sizeof bot_name, "%%%s %d", skey, seat);
        const int i = seat_add(s, entry ? (int)entry->strat : (int)STRAT_RANDOM, bot_id, bot_name,
                               entry ? entry->key : "random");
        if (i >= 0) s->seat_ready[i] = true;
    } else if (m.verb == CTL_META_START) {
        const int me = seat_of(s, user_id);
        if (me >= 0) { s->seat_ready[me] = true; game_lobby_ready(g, me); }
        // Whether a lobby may deal is game_lobby_can_deal's. This used to be
        // ">= 2 seats and every non-bot seat's seat_ready flag", which is the
        // same answer only because bots are seated READY - a coincidence of
        // two implementations that one implementation need not rely on.
        if (game_lobby_can_deal(g)) {
            unsigned char seed[32]; for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(next_rand() ^ (i * 131 + (int)g_seq));
            // No g_kernel_lock (Stage 5, see registry.h's "Locking" block): the
            // deal RNG state game_set_deal_seed_bytes/game_seat_and_deal touch
            // is _Thread_local, and every other kernel static reachable from
            // game_seat_and_deal's apply/refill path is too - s->lock
            // (already held here) is the only serialization this needs.
            game_set_deal_seed_bytes(seed, 32);
            s->rng_base = bot_drive_seed_base(seed, 32);
            // Seats were wired in the lobby (strategy_key per seat), so pass NULL:
            // the kernel owns marking them + the deal (+ g->status = PLAYING).
            game_seat_and_deal(g, NULL, g->num_players);
            start_bot_loop(s);             // the game-loop paces bot play from here
        }
    } else if (m.verb == CTL_META_CONTINUE) {
        // The rematch reset is game_reset_to_lobby's (game.c) - identities kept,
        // re-dealt on start. This branch used to spell out its own, and spelled
        // it SHORT: it set the seat statuses and the game status and left the
        // board fields (deck, discard, flipped, trump, battles, eliminations,
        // goods) standing until the next deal cleared them. Between the two, /ws
        // serves that state to the lobby, so the finished round's table was
        // still on screen. The host keeps only what is genuinely host state:
        // seat_ready.
        unsigned int bot_mask = 0;
        for (int i = 0; i < g->num_players; i++) {
            bool ai = seat_is_bot(g, i);
            if (ai) bot_mask |= 1u << i;
            s->seat_ready[i] = ai;
        }
        game_reset_to_lobby(g, bot_mask);
    }
    // Every branch above either mutates the roster/lobby state or is a no-op
    // (an already-seated join, a re-ready, an unrecognized verb), and bumping
    // on a no-op is harmless (worst case: one extra state_put on the next /ws
    // poll) - see GameSlot.version's doc. Unconditional beats re-deriving "did
    // this branch actually change anything" per-branch for a rarely-called path.
    s->version++;
    game_mark_dirty(s);
    // PROFILE_HOTPATH.md "T1f" (push-only protocol): a lobby transition
    // (join/add-bot/start-and-deal/continue) changes the board exactly like
    // a human move or a bot decision does, and any live /ws connection for
    // this game - most importantly a seat's own connection sitting blocked
    // on its next push right after THIS SAME client issued the /meta
    // continue+start rematch pair over HTTP - needs to hear about it
    // without polling. Same cross-thread wakeup bot_thread already uses at
    // its own version-bump site (see epoll_notify_game_changed's doc); a
    // no-op under --tls (thread-per-connection fallback, no epoll worker to
    // wake - see that doc's g_epoll_active guard). Before T1f this was
    // masked by every /ws client polling every ~1ms regardless, so a lobby
    // transition was noticed within a poll cycle even with no explicit
    // wakeup; a push-only client has no such fallback.
    epoll_notify_game_changed(s);
    unsigned char out[CTL_FRAME_MAX];
    int n = ctl_enc_lobby(s->id, g->status, out, (int)sizeof out);
    pthread_mutex_unlock(&s->lock);
    respond_ctl(conn, 200, out, n);
}
