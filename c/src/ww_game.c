#include "ww_game.h"
#include "deal_rng.h"

// Freestanding, like every other file the phone and the wasm build share: no
// libc beyond the two functions clang lowers struct copies to.
static void ww_zero(void *p, int n) {
    unsigned char *q = (unsigned char *)p;
    for (int i = 0; i < n; i++) q[i] = 0;
}

int ww_wolf_count(int n_players) {
    if (n_players < WW_MIN_PLAYERS || n_players > WW_MAX_PLAYERS) return 0;
    if (n_players <= 6) return 1;
    if (n_players <= 9) return 2;
    return 3;
}

int ww_team_of(int role) {
    return role == WW_ROLE_WOLF ? WW_TEAM_WOLVES : WW_TEAM_VILLAGE;
}

int ww_is_alive(const WwGame *g, int seat) {
    if (seat < 0 || seat >= g->n_players) return 0;
    return (g->alive >> seat) & 1;
}

int ww_alive_count(const WwGame *g) {
    int n = 0;
    for (int i = 0; i < g->n_players; i++) n += (g->alive >> i) & 1;
    return n;
}

int ww_deal(WwGame *g, const uint8_t seed[32], int n_players) {
    if (n_players < WW_MIN_PLAYERS || n_players > WW_MAX_PLAYERS) return WW_ECOUNT;
    ww_zero(g, (int)sizeof *g);
    g->n_players = (uint8_t)n_players;
    g->phase = WW_PHASE_NIGHT;
    g->night = 0;
    g->winner = WW_TEAM_NONE;
    for (int i = 0; i < WW_MAX_NIGHTS; i++) {
        g->victim[i] = WW_NO_SEAT;
        g->lynched[i] = WW_NO_SEAT;
    }
    for (int i = 0; i < n_players; i++) g->alive |= (uint16_t)(1u << i);

    // The bag, then a shuffle. Building the bag in a fixed order and shuffling
    // it is what keeps the deal a pure function of the seed: assigning roles by
    // drawing seats instead would make the result depend on the loop's
    // rejection pattern, which is the kind of thing that differs between a
    // refactor and its predecessor while both look right.
    uint8_t bag[WW_MAX_PLAYERS];
    const int wolves = ww_wolf_count(n_players);
    int k = 0;
    for (int i = 0; i < wolves; i++) bag[k++] = WW_ROLE_WOLF;
    bag[k++] = WW_ROLE_SEER;               // exactly one, at every table size
    while (k < n_players) bag[k++] = WW_ROLE_VILLAGER;

    DealRng r;
    deal_rng_seed(&r, seed);
    for (int i = n_players - 1; i > 0; i--) {
        const uint32_t j = deal_rng_bounded(&r, (uint32_t)(i + 1));
        const uint8_t t = bag[i]; bag[i] = bag[j]; bag[j] = t;
    }
    for (int i = 0; i < n_players; i++) g->role[i] = bag[i];
    return WW_OK;
}

int ww_night_order(const WwGame *g, int night, uint8_t *out) {
    if (g->n_players <= 0) return 0;
    const int start = night % g->n_players;
    int n = 0;
    for (int i = 0; i < g->n_players; i++) {
        const int seat = (start + i) % g->n_players;
        if (ww_is_alive(g, seat)) out[n++] = (uint8_t)seat;
    }
    return n;
}

int ww_night_decider(const WwGame *g) {
    // THE LAST WOLF IN TONIGHT'S ROTATION. The rotation's start advances every
    // night, so which wolf that is moves with it and the same voice is never
    // the one deciding two nights running.
    //
    // Last rather than first, and that is not arbitrary: the decider is also the
    // wolf who has read every other wolf's line before they choose, because the
    // lines ride in the records and the records arrive in rotation order. Making
    // the first wolf decide would make the wolves' channel useless to the only
    // wolf whose choice counts.
    uint8_t order[WW_MAX_PLAYERS];
    const int n = ww_night_order(g, g->night, order);
    int last = WW_NO_SEAT;
    for (int i = 0; i < n; i++)
        if (g->role[order[i]] == WW_ROLE_WOLF) last = order[i];
    return last;
}

// Where in tonight's rotation does `seat` sit? -1 if it is not in it.
static int rotation_index(const WwGame *g, int seat) {
    uint8_t order[WW_MAX_PLAYERS];
    const int n = ww_night_order(g, g->night, order);
    for (int i = 0; i < n; i++) if (order[i] == seat) return i;
    return -1;
}

// The record this seat holds for the night in play, or 0.
static const WwRecord *record_of(const WwGame *g, int seat, int night) {
    for (int i = 0; i < g->n_records; i++) {
        const WwRecord *r = &g->rec[i];
        if (r->night == night && r->seat == seat && !(r->flags & WW_REC_LYNCH)) return r;
    }
    return 0;
}

int ww_night_has_record(const WwGame *g, int seat) {
    return record_of(g, seat, g->night) != 0;
}

// Append one record. The only writer of g->rec and g->turn, so "turn counts
// accepted records" is true by construction rather than by every caller
// remembering - and Rule P clause 2 reads that count.
static int push(WwGame *g, int seat, int target, int night, int flags,
                const char *chat, int chat_len) {
    if (g->n_records >= WW_MAX_RECORDS) return WW_ECOUNT;
    WwRecord *r = &g->rec[g->n_records++];
    ww_zero(r, (int)sizeof *r);
    r->seat = (uint8_t)seat;
    r->target = (uint8_t)target;
    r->night = (uint8_t)night;
    r->flags = (uint8_t)flags;
    if (flags & WW_REC_HAS_CHAT) {
        r->chat_len = (uint8_t)chat_len;
        for (int i = 0; i < chat_len; i++) r->chat[i] = chat[i];
    }
    g->turn++;
    return WW_OK;
}

// Did every living seat send for the night in play?
static int night_complete(const WwGame *g) {
    for (int i = 0; i < g->n_players; i++)
        if (ww_is_alive(g, i) && !record_of(g, i, g->night)) return 0;
    return 1;
}

static void check_winner(WwGame *g) {
    int wolves = 0, others = 0;
    for (int i = 0; i < g->n_players; i++) {
        if (!ww_is_alive(g, i)) continue;
        if (g->role[i] == WW_ROLE_WOLF) wolves++; else others++;
    }
    if (wolves == 0) { g->winner = WW_TEAM_VILLAGE; g->phase = WW_PHASE_OVER; return; }
    // Parity, not elimination: once the wolves are half the room they can force
    // every remaining vote, so the game is decided and playing it out only
    // wastes the table's evenings.
    if (wolves >= others) { g->winner = WW_TEAM_WOLVES; g->phase = WW_PHASE_OVER; }
}

// THE KILL.
//
// Tonight's deciding wolf makes the call. When that wolf was auto-passed - the
// night moved on without him, which the skip guarantees can happen - the kill
// falls to THE MOST RECENT WOLF WHO DID CHOOSE, meaning the living wolf whose
// record landed latest in the chain.
//
// The alternative, holding the night open for him, is the one thing this design
// cannot do: a night that is waiting on exactly one player has named that
// player to everybody watching, and the player it names is a wolf. Falling back
// to the latest wolf is deterministic, invisible from outside (the night ended
// the way every night ends), and keeps the kill under wolf control rather than
// handing it to the dice or dropping it.
static void resolve_night(WwGame *g) {
    const int decider = ww_night_decider(g);
    int victim = WW_NO_SEAT;
    if (decider != WW_NO_SEAT) {
        const WwRecord *d = record_of(g, decider, g->night);
        if (d && !(d->flags & WW_REC_AUTO_PASS) && d->target != WW_NO_SEAT) {
            victim = d->target;
        } else {
            // Walk the chain forward and keep the last wolf choice, so "most
            // recent" is the chain's order and not the seat numbering.
            for (int i = 0; i < g->n_records; i++) {
                const WwRecord *r = &g->rec[i];
                if (r->night != g->night || (r->flags & WW_REC_LYNCH)) continue;
                if (r->flags & WW_REC_AUTO_PASS) continue;
                if (r->target == WW_NO_SEAT) continue;
                if (!ww_is_alive(g, r->seat) || g->role[r->seat] != WW_ROLE_WOLF) continue;
                victim = r->target;
            }
        }
    }
    if (victim != WW_NO_SEAT && ww_is_alive(g, victim)) {
        g->alive &= (uint16_t)~(1u << victim);
        g->victim[g->night] = (uint8_t)victim;
    }
    g->phase = WW_PHASE_DAY;
    check_winner(g);
}

int ww_night_pass_for(WwGame *g, int seat) {
    if (g->phase != WW_PHASE_NIGHT) return WW_EPHASE;
    if (!ww_is_alive(g, seat)) return WW_ESEAT;
    if (record_of(g, seat, g->night)) return WW_EDUP;
    const int rc = push(g, seat, WW_NO_SEAT, g->night, WW_REC_AUTO_PASS, 0, 0);
    if (rc != WW_OK) return rc;
    // A carried pass can be the record that completes the night, and it must be
    // - the whole point of the carry is that the night ends without waiting on
    // whoever is asleep.
    if (night_complete(g)) resolve_night(g);
    return WW_OK;
}

int ww_night_act(WwGame *g, int seat, int target,
                 const char *chat, int chat_len, int carry) {
    if (g->phase != WW_PHASE_NIGHT) return WW_EPHASE;
    if (!ww_is_alive(g, seat)) return WW_ESEAT;
    if (record_of(g, seat, g->night)) return WW_EDUP;
    if (target != WW_NO_SEAT) {
        if (!ww_is_alive(g, target) || target == seat) return WW_ETARGET;
    }
    const int wants_chat = chat && chat_len > 0;
    if (wants_chat) {
        if (g->role[seat] != WW_ROLE_WOLF) return WW_ECHAT;
        if (chat_len > WW_CHAT_MAX) return WW_ECHAT;
    }

    if (carry) {
        // Everyone ahead of this seat in tonight's rotation who has not sent.
        // Ahead only: a carry that reached forward would let one player end the
        // night by themselves, and the whole point of "everyone must send" is
        // that the night belongs to all of it.
        uint8_t order[WW_MAX_PLAYERS];
        const int n = ww_night_order(g, g->night, order);
        const int me = rotation_index(g, seat);
        for (int i = 0; i < n && i < me; i++) {
            if (record_of(g, order[i], g->night)) continue;
            const int rc = ww_night_pass_for(g, order[i]);
            if (rc != WW_OK) return rc;
        }
        // A carry that completed the night leaves nothing for this seat to say.
        // It cannot happen from a carry of seats AHEAD of this one - this seat is
        // still missing - so reaching here would mean the rotation and the
        // completeness test disagree, and refusing is how that gets found.
        if (g->phase != WW_PHASE_NIGHT) return WW_EPHASE;
    }

    const int rc = push(g, seat, target, g->night,
                        wants_chat ? WW_REC_HAS_CHAT : 0, chat, chat_len);
    if (rc != WW_OK) return rc;
    if (night_complete(g)) resolve_night(g);
    return WW_OK;
}

int ww_day_lynch(WwGame *g, int target) {
    if (g->phase != WW_PHASE_DAY) return WW_EPHASE;
    if (target != WW_NO_SEAT && !ww_is_alive(g, target)) return WW_ETARGET;
    const int rc = push(g, WW_NO_SEAT, target, g->night, WW_REC_LYNCH, 0, 0);
    if (rc != WW_OK) return rc;
    if (target != WW_NO_SEAT) {
        g->alive &= (uint16_t)~(1u << target);
        g->lynched[g->night] = (uint8_t)target;
    }
    check_winner(g);
    if (g->phase == WW_PHASE_OVER) return WW_OK;
    if (g->night + 1 >= WW_MAX_NIGHTS) {
        // Unreachable by the arithmetic in ww_game.h (each night and day take
        // one player, so ten cannot survive ten nights) and refused anyway, so
        // a future role that saves somebody cannot silently wrap the night
        // index into last night's records.
        return WW_ECOUNT;
    }
    g->night++;
    g->phase = WW_PHASE_NIGHT;
    return WW_OK;
}

// Both clocks are unix seconds mod 65536. The subtraction is done in uint16
// arithmetic on purpose: it wraps exactly once per 18.2 hours and the wrap
// cancels, so an interval that straddles the roll is still the right interval.
static uint16_t elapsed(uint16_t from, uint16_t now) {
    return (uint16_t)(now - from);
}

int ww_send_floor_remaining(uint16_t opened_at, uint16_t now) {
    const uint16_t d = elapsed(opened_at, now);
    return d >= WW_SEND_FLOOR_S ? 0 : (int)(WW_SEND_FLOOR_S - d);
}

int ww_may_carry(uint16_t last_seal_at, uint16_t now) {
    return elapsed(last_seal_at, now) >= WW_CARRY_AFTER_S;
}
