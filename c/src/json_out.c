#include "json_out.h"
#include "view.h"
#include "evwire.h"
#include "../wasm/wire.h"

// No <stdio.h> here. This emitter began life in ios/ios_api.c, which links a
// real libc and used snprintf for its integers; the wasm build is
// `-nostdlib -ffreestanding` and its stdio shim declares fprintf and nothing
// else. Moving the emitter into the shared core is what took the dependency
// away, so the formatting below is hand-rolled. It is a fair price: the
// alternative is two emitters, which is the thing this file exists to stop.

// ---------- the bounded builder --------------------------------------------

void j_init(J *j, char *buf, int cap) { j->buf = buf; j->cap = cap; j->w = 0; j->ok = (buf && cap > 0); }

void j_putc(J *j, char c) {
    if (!j->ok) return;
    // >= leaves room for the NUL j_finish writes.
    if (j->w + 1 >= j->cap) { j->ok = 0; return; }
    j->buf[j->w++] = c;
}

void j_puts(J *j, const char *s) { while (*s) j_putc(j, *s++); }

void j_puti(J *j, long v) {
    // Digits are generated least-significant first into t, then reversed. The
    // negation goes through unsigned so LONG_MIN does not overflow on the way.
    char t[24];
    int n = 0;
    unsigned long m;
    if (v < 0) { j_putc(j, '-'); m = (unsigned long)(-(v + 1)) + 1UL; }
    else m = (unsigned long)v;
    do { t[n++] = (char)('0' + (int)(m % 10)); m /= 10; } while (m);
    while (n > 0) j_putc(j, t[--n]);
}

static const char J_HEX[] = "0123456789abcdef";

void j_putstr(J *j, const char *s) {
    j_putc(j, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { j_putc(j, '\\'); j_putc(j, (char)c); }
        else if (c == '\n') { j_putc(j, '\\'); j_putc(j, 'n'); }
        else if (c == '\t') { j_putc(j, '\\'); j_putc(j, 't'); }
        else if (c < 0x20) {
            // Control characters always fit \u00XX.
            j_puts(j, "\\u00");
            j_putc(j, J_HEX[(c >> 4) & 0xf]);
            j_putc(j, J_HEX[c & 0xf]);
        }
        else j_putc(j, (char)c);
    }
    j_putc(j, '"');
}

void j_card(J *j, Card c) {
    j_puts(j, "{\"s\":"); j_puti(j, c.suit);
    j_puts(j, ",\"v\":"); j_puti(j, c.value); j_putc(j, '}');
}

int j_finish(J *j) {
    if (!j->ok) return JSON_ECAP;
    j->buf[j->w] = 0;
    return j->w;
}

// ---------- the masked board ------------------------------------------------

void json_state(J *j, const Game *g, int viewer) {
    j_puts(j, "{\"status\":");        j_puti(j, g->status);
    j_puts(j, ",\"numPlayers\":");    j_puti(j, g->num_players);
    j_puts(j, ",\"powerSuit\":");     j_puti(j, g->power_suit);
    j_puts(j, ",\"deckCount\":");     j_puti(j, g->deck_count);
    j_puts(j, ",\"discardCount\":");  j_puti(j, g->discard_pile_length);
    j_puts(j, ",\"hasFlipped\":");    j_puts(j, g->has_flipped ? "true" : "false");
    j_puts(j, ",\"firstAttacker\":"); j_puti(j, g->first_attacker);
    j_puts(j, ",\"defender\":");      j_puti(j, g->defender);
    j_puts(j, ",\"viewer\":");        j_puti(j, viewer);
    j_puts(j, ",\"goodMask\":");      j_puti(j, (long)g->good_players_mask);
    // Whether a good_timestamp EXISTS, not its value: the value is a host clock
    // reading the kernel never took (view.c carries the same single byte). The
    // web needs it to decide between `null` and its own remembered timestamp.
    j_puts(j, ",\"hasGoodTs\":");     j_puts(j, g->has_good_timestamp ? "true" : "false");
    j_puts(j, ",\"gameOver\":");      j_puti(j, game_done(g));

    j_puts(j, ",\"flipped\":");
    if (g->has_flipped) j_card(j, g->flipped); else j_puts(j, "null");

    j_puts(j, ",\"battles\":[");
    for (int i = 0; i < g->num_battles; i++) {
        if (i) j_putc(j, ',');
        const Battle *b = &g->table_battles[i];
        j_puts(j, "{\"attack\":"); j_card(j, b->attack);
        j_puts(j, ",\"defense\":");
        if (card_is_none(b->defense)) j_puts(j, "null"); else j_card(j, b->defense);
        j_putc(j, '}');
    }
    j_putc(j, ']');

    j_puts(j, ",\"eliminationOrder\":[");
    for (int i = 0; i < g->num_eliminated; i++) { if (i) j_putc(j, ','); j_puti(j, g->elimination_order[i]); }
    j_putc(j, ']');

    // Every seat exposes seat/name/status/handCount/awaitingAttack/strategyKey;
    // ONLY the viewer seat exposes its real `hand`. Others emit "hand":null so a
    // host renders card backs from handCount and cannot leak what it never got.
    //
    // `name`/`strategyKey` are empty/0 on a from-packed decode: identity is
    // deliberately not in the state blob (game.h), so it is the caller that owns
    // them. Emitted anyway because a resident Game HAS them, and one shape beats
    // two.
    j_puts(j, ",\"players\":[");
    for (int p = 0; p < g->num_players; p++) {
        if (p) j_putc(j, ',');
        const Player *pl = &g->players[p];
        int is_viewer = (viewer == p);
        j_puts(j, "{\"seat\":");         j_puti(j, p);
        j_puts(j, ",\"name\":");         j_putstr(j, pl->name);
        j_puts(j, ",\"status\":");       j_puti(j, pl->status);
        j_puts(j, ",\"handCount\":");    j_puti(j, pl->hand_count);
        j_puts(j, ",\"awaitingAttack\":");
        j_puts(j, (is_viewer && pl->awaiting_attack) ? "true" : "false");
        j_puts(j, ",\"strategyKey\":");  j_puti(j, pl->strategy_key);
        j_puts(j, ",\"hand\":");
        if (is_viewer) {
            j_putc(j, '[');
            for (int c = 0; c < pl->hand_count; c++) { if (c) j_putc(j, ','); j_card(j, pl->hand[c]); }
            j_putc(j, ']');
        } else {
            j_puts(j, "null");
        }
        j_putc(j, '}');
    }
    j_putc(j, ']');
    j_putc(j, '}');
}

int json_state_of(const Game *g, int viewer, char *out, int cap) {
    J j; j_init(&j, out, cap);
    json_state(&j, g, viewer);
    return j_finish(&j);
}

// ---------- decoding packed blobs ------------------------------------------
//
// state_get writes nothing at or beyond Game.logs — it fills the prefix, then
// sets num_logs/log_cap/log_virt and stops — and json_state and game_done read
// only prefix fields. So a decode target needs the bytes up to logs[], not a
// whole log-laden Game: ~1.1 KB against ~136 KB in the bots wasm build, where
// MAX_LOGS is 1024 and MAX_LOG_PAIRS 64. That difference is the whole reason
// folding the web's decode in here does not re-open the page budget
// docs/BOTS_WASM_MEMORY_PLAN.md just finished closing. The asserts below are
// what make it safe rather than lucky: move a field past logs[] and the build
// stops.

#define JSON_SLOT_SIZE ((int)__builtin_offsetof(Game, logs))
typedef struct { _Alignas(_Alignof(Game)) unsigned char bytes[JSON_SLOT_SIZE]; } JsonSlot;

_Static_assert(__builtin_offsetof(Game, num_logs) < JSON_SLOT_SIZE,
               "state_get writes num_logs past the slot");
_Static_assert(__builtin_offsetof(Game, log_cap) < JSON_SLOT_SIZE,
               "state_get writes log_cap past the slot");
_Static_assert(__builtin_offsetof(Game, log_virt) < JSON_SLOT_SIZE,
               "state_get writes log_virt past the slot");

// One slot, reused: every decode is written-then-read inside a single call and
// no two boards are ever live at once (an event's state is emitted before the
// next one is decoded). Single-threaded module, non-reentrant exports.
static JsonSlot g_slot;

// Zeroing is load-bearing, not hygiene: state_get does NOT write Player.name or
// Player.strategy_key (identity is not in the blob), so without this j_putstr
// would walk uninitialized bytes looking for a NUL.
static Game *slot_decode(const unsigned char *buf) {
    __builtin_memset(&g_slot, 0, sizeof g_slot);
    Game *g = (Game *)(void *)&g_slot;
    state_get(g, buf, /*masked=*/1);
    return g;
}

int json_view_from_packed(const unsigned char *buf, int len, int viewer,
                          char *out, int cap) {
    if (!buf || len <= 0) return JSON_EBADARG;
    Game *g = slot_decode(buf);
    // A waiting lobby legitimately holds a single player (the creator, before
    // anyone joins) — rendering that lobby needs its view, so only 0/negative or
    // an over-full table is malformed. (Playing needs ≥2, but that is the play
    // path's concern, not this decoder's.)
    if (g->num_players < 1 || g->num_players > MAX_PLAYERS) return JSON_EPARSE;
    if (viewer != VIEW_SPECTATOR && (viewer < 0 || viewer >= g->num_players)) return JSON_EBADARG;
    return json_state_of(g, viewer, out, cap);
}

// ---------- the packed evwire reader ----------------------------------------
//
// The counterpart of evwire.c's sink_packed, and deliberately in the same
// repository sentence as it: a format with a writer here and a reader in
// TypeScript is two formats wearing one name, which is exactly how the web's
// mirror came to need a parity test to stay true.
//
// Field order and value conventions match ios_api.c's fio_ev_sink exactly (raw
// ints, EVW_LOC_NONE as 255, seat -1 for none, a masked card as null), so the
// two hosts read one shape.

int json_events_from_packed(const unsigned char *buf, int len, char *out, int cap) {
    if (!buf || len < 4) return JSON_EBADARG;
    if (buf[0] != EVWIRE_FORMAT_VERSION) return JSON_EPARSE;

    const int viewer = (buf[1] == EVW_SEAT_NONE) ? -1 : (int)buf[1];
    const int actor  = (buf[2] == EVW_SEAT_NONE) ? -1 : (int)buf[2];
    const int n_events = (int)buf[3];
    int q = 4;

    J j; j_init(&j, out, cap);
    j_puts(&j, "{\"viewer\":"); j_puti(&j, viewer);
    j_puts(&j, ",\"actor\":");  j_puti(&j, actor);
    j_puts(&j, ",\"events\":[");

    for (int i = 0; i < n_events; i++) {
        if (q + 7 > len) return JSON_EPARSE;
        const int type    = buf[q++];
        const int seat_b  = buf[q++];
        const int msg     = buf[q++];
        const int from    = buf[q++];
        const int to      = buf[q++];
        const int flags   = buf[q++];
        const int n_cards = buf[q++];
        const int has_target = (flags & 1) != 0;
        const int has_battle = (flags & 2) != 0;
        if (q + n_cards + has_target + has_battle + 2 > len) return JSON_EPARSE;

        if (i) j_putc(&j, ',');
        j_puts(&j, "{\"type\":");  j_puti(&j, type);
        j_puts(&j, ",\"seat\":");  j_puti(&j, seat_b == EVW_SEAT_NONE ? -1 : seat_b);
        j_puts(&j, ",\"msg\":");   j_puti(&j, msg);
        j_puts(&j, ",\"from\":");  j_puti(&j, from);
        j_puts(&j, ",\"to\":");    j_puti(&j, to);
        j_puts(&j, ",\"cards\":[");
        for (int c = 0; c < n_cards; c++) {
            if (c) j_putc(&j, ',');
            const unsigned char b = buf[q++];
            // The DEAL/REFILL redaction, already applied by the writer: a card
            // bound for someone else's hand crossed as WIRE_CARD_HIDDEN and
            // stays a card back here. There is nothing to leak — it never came.
            if (b == WIRE_CARD_HIDDEN) j_puts(&j, "null");
            else j_card(&j, card_from_wire_state(b));
        }
        j_putc(&j, ']');
        if (has_target) { j_puts(&j, ",\"target\":"); j_card(&j, card_from_wire_state(buf[q++])); }
        if (has_battle) { j_puts(&j, ",\"battle\":"); j_puti(&j, buf[q++]); }

        const int snap_len = buf[q] | (buf[q + 1] << 8); q += 2;
        if (snap_len < 0 || q + snap_len > len) return JSON_EPARSE;
        j_puts(&j, ",\"state\":");
        json_state(&j, slot_decode(buf + q), viewer);
        q += snap_len;
        j_putc(&j, '}');
    }
    j_putc(&j, ']');

    // Trailer: the committed final board — the sequence's `game`.
    if (q + 2 > len) return JSON_EPARSE;
    const int fin_len = buf[q] | (buf[q + 1] << 8); q += 2;
    if (fin_len < 0 || q + fin_len > len) return JSON_EPARSE;
    j_puts(&j, ",\"game\":");
    json_state(&j, slot_decode(buf + q), viewer);
    j_putc(&j, '}');

    return j_finish(&j);
}
