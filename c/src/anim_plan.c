// anim_plan.c — the animation core (see anim_plan.h). Pure data transformation:
// no allocation, no I/O, no rules question. Every function here is the single C
// port of a web behaviour the glitch-fixing hardened, with the iOS re-derivation
// noted where it agrees.

#include "anim_plan.h"

// ---- small helpers --------------------------------------------------------

// getCardKey parity: a real card's identity, ignoring event fields. Kept local
// (the header inlines anim_card_key for callers).
static int card_id_or_neg(Card c) {
    // A viewer-masked back is {-1,-1}; the "no card" sentinel is {-2,-2}. Neither
    // has a dense id — return -1 so the veil skips it.
    if (c.suit < 0 || c.value < 1 || c.value > 13) return -1;
    return card_to_id(c);
}

// A tiny presence set over card keys (anim_card_key packs suit/value into <256),
// used by the linear scans below. Sequences are short, so a flat bool table is
// both the simplest and the fastest structure.
#define KEYSET_N 256
static void keyset_clear(unsigned char *s) {
    for (int i = 0; i < KEYSET_N; i++) s[i] = 0;
}
static void keyset_add_card(unsigned char *s, Card c) {
    int k = anim_card_key(c);
    if (k >= 0 && k < KEYSET_N) s[k] = 1;
}
static int keyset_has_card(const unsigned char *s, Card c) {
    int k = anim_card_key(c);
    return (k >= 0 && k < KEYSET_N) ? s[k] : 0;
}

// ---- timing policy --------------------------------------------------------

int anim_step_duration_ms(int event_type) {
    // Every kernel event paces at ANIMATION_TIME today; a revert is a client
    // synthesis that flies the same distance and gets the same beat. The switch
    // is the seam a future per-type rule lands in — a platform asks here.
    (void)event_type;
    return ANIM_TIME_MS;
}

// ---- plan building --------------------------------------------------------

// The per-event count delta, POSITIVE direction (forward in time):
//   deal/refill  deck -= n, acting hand += n   (deck -> a hand)
//   discard/trash  discard += n                (table -> the pile)
//   pickup       acting hand += n              (table -> a hand)
//   attack/cover/defender_move  acting hand -= n   (a hand -> the table)
//   flipped/magic_transition/out  no count change - the flipped (trump) card
//     stays in the deck badge (web: "cards bound for the flipped slot ... don't
//     affect the badge total"), and a transition/out moves no card.
static void apply_forward(const AnimPlanEvent *ev, AnimCounts *c) {
    const int n = ev->n_cards;
    const int s = ev->seat;
    switch (ev->type) {
        case ANIM_EVT_DEAL:
        case ANIM_EVT_REFILL:
            c->deck -= n;
            if (s >= 0 && s < c->n_players) c->hand[s] += n;
            break;
        case ANIM_EVT_DISCARD:
        case ANIM_EVT_CARDS_TO_TRASH:
            c->discard += n;
            break;
        case ANIM_EVT_PICKUP:
            if (s >= 0 && s < c->n_players) c->hand[s] += n;
            break;
        case ANIM_EVT_ATTACK_PASS:
        case ANIM_EVT_COVER:
        case ANIM_EVT_DEFENDER_MOVE:
            if (s >= 0 && s < c->n_players) c->hand[s] -= n;
            break;
        default:
            break;   // flipped, magic_transition, out, revert
    }
}

// The undo (negative direction): the exact inverse of apply_forward. Applied to
// ONE event (the first) in the normal path - see anim_build_plan.
static void apply_undo(const AnimPlanEvent *ev, AnimCounts *c) {
    const int n = ev->n_cards;
    const int s = ev->seat;
    switch (ev->type) {
        case ANIM_EVT_DEAL:
        case ANIM_EVT_REFILL:
            c->deck += n;
            if (s >= 0 && s < c->n_players) c->hand[s] -= n;
            break;
        case ANIM_EVT_DISCARD:
        case ANIM_EVT_CARDS_TO_TRASH:
            c->discard -= n;
            break;
        case ANIM_EVT_PICKUP:
            if (s >= 0 && s < c->n_players) c->hand[s] -= n;
            break;
        case ANIM_EVT_ATTACK_PASS:
        case ANIM_EVT_COVER:
        case ANIM_EVT_DEFENDER_MOVE:
            if (s >= 0 && s < c->n_players) c->hand[s] += n;
            break;
        default:
            break;
    }
}

// Adopt a step's own board, when it carries one.
static void adopt_counts(const AnimPlanEvent *ev, AnimCounts *c) {
    c->deck = ev->deck;
    c->discard = ev->discard;
    for (int s = 0; s < c->n_players; s++) c->hand[s] = ev->hand[s];
}

static int carries_counts(const AnimPlanEvent *ev) {
    return ev->has_counts && ev->hand != 0;
}

int anim_build_plan(const AnimPlanEvent *events, int n_events, int n_players,
                    int final_deck, int final_discard, const int *final_hand,
                    AnimPlan *out) {
    if (!out || !final_hand || n_players < 2 || n_players > MAX_PLAYERS) return ANIM_EBADARG;
    if (n_events < 0) return ANIM_EBADARG;
    if (n_events > ANIM_MAX_STEPS) return ANIM_ECAP;
    if (n_events > 0 && !events) return ANIM_EBADARG;

    out->n_steps = n_events;
    out->total_ms = 0;
    out->n_veil = 0;

    // The freeze. ONE undo off the first event's own board, because a refill
    // hands out cards the deck count never held (the flipped trump) and undoing
    // every event would put them all back - see anim_plan.h. The n-undo walk
    // back from the final board survives only for a stream carrying no boards.
    AnimCounts cur;
    cur.n_players = n_players;
    if (n_events > 0 && carries_counts(&events[0])) {
        adopt_counts(&events[0], &cur);
        apply_undo(&events[0], &cur);
    } else {
        cur.deck = final_deck;
        cur.discard = final_discard;
        for (int s = 0; s < n_players; s++) cur.hand[s] = final_hand[s];
        for (int i = n_events - 1; i >= 0; i--) apply_undo(&events[i], &cur);
    }
    out->pre = cur;   // cur is now the pre-sequence board

    // Forward walk from the freeze -> each step's post counts + timing + veil.
    unsigned char veil_seen[KEYSET_N];   // dense-id presence, dedups the veil
    keyset_clear(veil_seen);
    const int stride = ANIM_TIME_MS + ANIM_GAP_MS;
    for (int i = 0; i < n_events; i++) {
        const AnimPlanEvent *ev = &events[i];
        // A step's post counts ARE its own board; the delta only carries the
        // walk forward across a step that has none.
        if (carries_counts(ev)) adopt_counts(ev, &cur);
        else                    apply_forward(ev, &cur);

        AnimPlanStep *st = &out->steps[i];
        st->type = ev->type;
        st->seat = ev->seat;
        st->from = ev->from;
        st->to = ev->to;
        st->n_cards = ev->n_cards;
        st->duration_ms = anim_step_duration_ms(ev->type);
        st->start_ms = i * stride;
        st->deck = cur.deck;
        st->discard = cur.discard;
        for (int s = 0; s < n_players; s++) st->hand[s] = cur.hand[s];

        // in-flight-from-deck (web inFlightFromDeck / inFlightToFlipped): a step
        // whose cards leave the deck drops the deck badge NOW; the flipped subset
        // does not count against the badge.
        if (ev->from == ANIM_LOC_DECK && ev->n_cards > 0) {
            st->in_flight_from_deck = ev->n_cards;
            st->in_flight_to_flipped = (ev->to == ANIM_LOC_FLIPPED) ? ev->n_cards : 0;
        } else {
            st->in_flight_from_deck = 0;
            st->in_flight_to_flipped = 0;
        }

        // Veil: a real card landing in a hand or on the table is in transit until
        // this step; hide it so it flies rather than popping in. Masked backs and
        // cards headed to the discard pile (no rendered identity) are not veiled.
        if ((ev->to == ANIM_LOC_HAND || ev->to == ANIM_LOC_TABLE) && !ev->mask_cards
            && ev->cards && ev->n_cards > 0) {
            for (int k = 0; k < ev->n_cards; k++) {
                int id = card_id_or_neg(ev->cards[k]);
                if (id < 0 || id >= KEYSET_N || veil_seen[id]) continue;
                if (out->n_veil >= ANIM_MAX_VEIL) return ANIM_ECAP;
                veil_seen[id] = 1;
                out->veil_ids[out->n_veil++] = (unsigned char)id;
            }
        }
    }
    // Wall time: last step's start + its duration (the trailing gap is dead air
    // the queue does not wait on).
    out->total_ms = n_events > 0
        ? out->steps[n_events - 1].start_ms + out->steps[n_events - 1].duration_ms
        : 0;
    return ANIM_EOK;
}

// ---- beats ----------------------------------------------------------------

// The three types that put a card DOWN on the table. Not "the acting seat's
// hand shrank": a pickup shrinks the table, a discard the table too, and the
// cards a beat placed are what a later sweep is drawn from.
static int is_placement(int type) {
    return type == ANIM_EVT_ATTACK_PASS
        || type == ANIM_EVT_DEFENDER_MOVE
        || type == ANIM_EVT_COVER;
}

// The identities one event puts on the table, as dense-id bits. A masked back
// names nothing, which is why this can be empty for a non-empty event.
static uint64_t placed_of(const AnimBeatEvent *ev) {
    if (!is_placement(ev->type) || ev->mask_cards || !ev->cards) return 0;
    uint64_t ids = 0;
    for (int k = 0; k < ev->n_cards; k++) {
        const int id = card_id_or_neg(ev->cards[k]);
        if (id >= 0 && id < 52) ids |= (uint64_t)1 << id;
    }
    return ids;
}

// A beat that moved a card at all - the gate on adopting the out notices that
// follow it. Wider than "placed": taking the table, sweeping it to the pile and
// dealing all move cards and all can put a seat out.
static int moves_cards(int type) {
    switch (type) {
        case ANIM_EVT_PICKUP:
        case ANIM_EVT_DISCARD:
        case ANIM_EVT_CARDS_TO_TRASH:
        case ANIM_EVT_REFILL:
        case ANIM_EVT_DEAL:
            return 1;
        default:
            return 0;
    }
}

int anim_badge_drops_as_cards_leave(int type) {
    return type == ANIM_EVT_ATTACK_PASS || type == ANIM_EVT_COVER;
}

// A notice: no cards, no flight, no time of its own.
static int is_out_only(const AnimBeat *b, const AnimBeatEvent *events) {
    for (int k = 0; k < b->n_events; k++)
        if (events[b->first + k].type != ANIM_EVT_OUT) return 0;
    return 1;
}

int anim_build_beats(const AnimBeatEvent *events, int n_events, AnimBeats *out) {
    if (!out || n_events < 0) return ANIM_EBADARG;
    if (n_events > 0 && !events) return ANIM_EBADARG;
    if (n_events > ANIM_MAX_BEATS) return ANIM_ECAP;

    out->n_beats = 0;
    out->placed_ids = 0;
    out->first_good_mask = n_events > 0 ? events[0].good_mask : ANIM_NO_MASK;

    // Grouping. ONLY consecutive covers by one seat merge: an attack or a pass
    // already carries every card of its move in one event, deals and refills are
    // per seat, and a bout's closing discard/refill are the cover's consequences
    // rather than part of the same movement - they keep their own beats, which is
    // what makes the counts settle in the right order. Consecutive, so a bout
    // boundary splits a run: the discard between two covers ends it.
    for (int i = 0; i < n_events; i++) {
        AnimBeat *last = out->n_beats > 0 ? &out->beats[out->n_beats - 1] : 0;
        if (last && events[i].type == ANIM_EVT_COVER
            && last->type == ANIM_EVT_COVER && last->seat == events[i].seat) {
            last->n_events++;
        } else {
            AnimBeat *b = &out->beats[out->n_beats++];
            b->first = i;
            b->n_events = 1;
            b->type = events[i].type;
            b->seat = events[i].seat;
            b->flags = 0;
            b->outs_mask = 0;
            b->attack_pass_seats = 0;
            b->placed_ids = 0;
            b->good_mask = ANIM_NO_MASK;
        }
    }

    // Per-beat facts that read only the beat's own events.
    for (int g = 0; g < out->n_beats; g++) {
        AnimBeat *b = &out->beats[g];
        for (int k = 0; k < b->n_events; k++) {
            const AnimBeatEvent *ev = &events[b->first + k];
            b->placed_ids |= placed_of(ev);
            if (ev->type == ANIM_EVT_OUT && ev->seat >= 0 && ev->seat < 32)
                b->outs_mask |= 1u << ev->seat;
            if (ev->type == ANIM_EVT_ATTACK_PASS && ev->seat >= 0 && ev->seat < 32)
                b->attack_pass_seats |= 1u << ev->seat;
            if (moves_cards(ev->type)) b->flags |= ANIM_BEAT_MOVED;
        }
        // The board the beat settles to is its LAST event's: the intermediate
        // boards inside one move are boards nobody was ever shown.
        b->good_mask = events[b->first + b->n_events - 1].good_mask;
        if (b->placed_ids) b->flags |= ANIM_BEAT_PLACED | ANIM_BEAT_MOVED;
        if (anim_badge_drops_as_cards_leave(b->type)) b->flags |= ANIM_BEAT_DROPS;
        out->placed_ids |= b->placed_ids;
    }

    // The outs a beat adopts, and the hold after it. Both look FORWARD, so they
    // run once the beats above exist.
    for (int g = 0; g < out->n_beats; g++) {
        AnimBeat *b = &out->beats[g];

        // Only a beat that actually moved something may adopt what follows it -
        // an out belongs to the move that caused it, never to one two beats
        // later - and the lookahead stops at the first beat that is not purely
        // notices.
        if (b->flags & ANIM_BEAT_MOVED) {
            for (int j = g + 1; j < out->n_beats; j++) {
                if (!is_out_only(&out->beats[j], events)) break;
                b->outs_mask |= out->beats[j].outs_mask;
            }
        }

        // The hold: a COVER whose bout end follows. Not merely the next beat -
        // a bout that ends because the defender's last card went down puts their
        // OUT (and, at the end of a game, a magic transition) between the cover
        // and the trash, and those are notices, so they neither separate the
        // cover from its consequence nor earn a hold of their own. Anything that
        // DOES move a card ends the scan: a refill or a pickup after a cover
        // means the table did not close on it, and holding there would stall a
        // sequence that is still going somewhere.
        if (b->type != ANIM_EVT_COVER) continue;
        for (int j = g + 1; j < out->n_beats; j++) {
            const int t = out->beats[j].type;
            if (t == ANIM_EVT_DISCARD || t == ANIM_EVT_CARDS_TO_TRASH) {
                b->flags |= ANIM_BEAT_HOLDS;
                break;
            }
            if (t == ANIM_EVT_OUT || t == ANIM_EVT_MAGIC_TRANSITION
                || t == ANIM_EVT_FLIPPED) continue;
            break;
        }
    }
    return out->n_beats;
}

// ---- the role beat --------------------------------------------------------

int anim_goods_opening(AnimRoles shown, int first_good_mask, AnimRoles *out) {
    if (!out || first_good_mask == ANIM_NO_MASK) return 0;
    // ADDED goods only. A good being set is somebody's move and belongs in front
    // of the consequences it caused; one being cleared is the consequence of an
    // attack that reopened the bout and belongs at the back with the rest.
    const int added = first_good_mask & ~shown.good_mask;
    if (!added) return 0;
    out->defender = shown.defender;
    out->first_attacker = shown.first_attacker;
    out->good_mask = shown.good_mask | added;
    return 1;
}

int anim_goods_cleared(AnimRoles shown, int step_good_mask, AnimRoles *out) {
    if (!out || step_good_mask == ANIM_NO_MASK) return 0;
    const int removed = shown.good_mask & ~step_good_mask;
    if (!removed) return 0;
    out->defender = shown.defender;
    out->first_attacker = shown.first_attacker;
    out->good_mask = shown.good_mask & ~removed;
    return 1;
}

int anim_pass_hand_off(AnimRoles shown, unsigned attack_pass_seats,
                       int final_defender, AnimRoles *out) {
    if (!out || final_defender == shown.defender) return 0;
    if (shown.defender < 0 || shown.defender >= 32) return 0;
    if (!(attack_pass_seats & (1u << shown.defender))) return 0;
    // Only the defender moves. A pass never touches first_attacker, so taking
    // the final board wholesale would let a stream that ALSO ended a bout hand
    // the opening sword over with the transfer card instead of at its own beat.
    out->defender = final_defender;
    out->first_attacker = shown.first_attacker;
    out->good_mask = shown.good_mask;
    return 1;
}

// ---- the pre-bout table ---------------------------------------------------

// Does this board name EXACTLY the cards the sweep takes? Multiplicity does not
// come into it: a table holds 52 distinct cards at most, so a presence set over
// dense ids answers it, and the two counts settle "exactly" against "at least".
#define PRE_IDS 52
static int table_accounts_for(const unsigned char *bat, int n_bat,
                              const unsigned char *cards, int n_cards) {
    unsigned char seen[PRE_IDS];
    int n = 0;
    for (int i = 0; i < PRE_IDS; i++) seen[i] = 0;
    for (int i = 0; i < n_bat; i++) {
        for (int h = 0; h < 2; h++) {
            const unsigned char c = bat[2 * i + h];
            if (c >= PRE_IDS) continue;          // an uncovered half names nothing
            if (!seen[c]) { seen[c] = 1; n++; }
        }
    }
    if (n != n_cards) return 0;
    for (int i = 0; i < n_cards; i++) {
        if (cards[i] >= PRE_IDS || !seen[cards[i]]) return 0;
    }
    return 1;
}

static int pre_take_board(AnimPreTable *out, const unsigned char *bat, int n_bat) {
    if (n_bat > ANIM_MAX_PRE_BATTLES) return ANIM_ECAP;
    for (int i = 0; i < 2 * n_bat; i++) out->battles[i] = bat[i];
    out->n_battles = n_bat;
    out->paired = 1;
    return n_bat;
}

int anim_pre_bout_table(const AnimPreEvent *events, int n_events,
                        int n_prior, const unsigned char *prior,
                        AnimPreTable *out) {
    if (!out) return ANIM_EBADARG;
    out->n_battles = 0;
    out->paired = 0;
    if (n_events < 0 || (n_events > 0 && !events)) return ANIM_EBADARG;
    if (n_prior > 0 && !prior) return ANIM_EBADARG;

    // The step that takes the table away. The FIRST one: a stream can carry a
    // pickup and the trash of the next bout, and it is the first sweep the
    // board is about to play.
    int bi = -1;
    for (int i = 0; i < n_events && bi < 0; i++) {
        const int t = events[i].type;
        if (t == ANIM_EVT_PICKUP || t == ANIM_EVT_DISCARD || t == ANIM_EVT_CARDS_TO_TRASH) bi = i;
    }
    if (bi < 0) return 0;
    const AnimPreEvent *sweep = &events[bi];
    if (sweep->n_cards < 0 || (sweep->n_cards > 0 && !sweep->cards)) return ANIM_EBADARG;
    const int is_pickup = sweep->type == ANIM_EVT_PICKUP;

    // Back from the sweep to the last board that still had cards on it. That is
    // the table about to be taken. A clean defence is grouped from its last
    // cover, whose board still shows the whole covered table; the trash step's
    // own board is already empty.
    for (int i = bi; i >= 0; i--) {
        const AnimPreEvent *e = &events[i];
        if (e->n_battles <= 0) continue;
        if (!e->battles) return ANIM_EBADARG;
        // A pickup names its cards, so a candidate can be CHECKED rather than
        // trusted; stop at the first board that fails, since anything older
        // describes an even earlier moment.
        if (is_pickup && !table_accounts_for(e->battles, e->n_battles,
                                             sweep->cards, sweep->n_cards)) break;
        return pre_take_board(out, e->battles, e->n_battles);
    }

    // Only a pickup gets this far: a discard's table is always on some step of
    // its own stream (the cover that closed the bout), and a discard names the
    // pile rather than the table, so there is nothing to check a guess against.
    if (!is_pickup) return 0;

    // The board the whole stream OPENED on, under the same exact-account test -
    // for the single-action pickup turn, which carries no earlier step at all.
    if (n_prior > 0 && table_accounts_for(prior, n_prior, sweep->cards, sweep->n_cards))
        return pre_take_board(out, prior, n_prior);

    // The flat reading: one uncovered cell per picked-up card. The right SET of
    // cards every time and the wrong SHAPE about half the time, so it is
    // reported as unpaired and a caller may refuse it.
    if (sweep->n_cards > ANIM_MAX_PRE_BATTLES) return ANIM_ECAP;
    for (int i = 0; i < sweep->n_cards; i++) {
        if (sweep->cards[i] >= PRE_IDS) return ANIM_EBADARG;
        out->battles[2 * i] = sweep->cards[i];
        out->battles[2 * i + 1] = ANIM_TABLE_NONE;
    }
    out->n_battles = sweep->n_cards;
    out->paired = 0;
    return out->n_battles;
}

// ---- optimistic policy ----------------------------------------------------

uint64_t anim_event_key(int type, Card card, int from, int to, int seat) {
    // Pack the five discriminators createCardEventString serialises. Bytes, not
    // bit-fields, so the layout is obvious and two keys compare with a single
    // integer ==. suit/value are masked to a byte (a real card is in range; a
    // sentinel would still pack distinctly and never collide with a real one).
    uint64_t k = 0;
    k |= (uint64_t)(type & 0xff);
    k |= (uint64_t)(from & 0xff) << 8;
    k |= (uint64_t)(to   & 0xff) << 16;
    k |= (uint64_t)(seat & 0xff) << 24;
    k |= (uint64_t)((unsigned)(card.suit)  & 0xff) << 32;
    k |= (uint64_t)((unsigned)(card.value) & 0xff) << 40;
    return k;
}

int anim_should_drop_stale(int has_last, int last_version,
                           int has_incoming, int incoming_version) {
    // A replay sequence carries no version (has_incoming == 0) and is never gated.
    if (!has_incoming) return 0;
    if (!has_last) return 0;
    return incoming_version <= last_version ? 1 : 0;
}

int anim_stale_optimistic_on_table(const Card *opt_cards, int n_opt,
                                   const Card *table_cards, int n_table,
                                   const Card *named_cards, int n_named,
                                   int *out_release, int cap) {
    if (n_opt < 0 || n_table < 0 || n_named < 0) return ANIM_EBADARG;
    if (n_opt > 0 && !opt_cards) return ANIM_EBADARG;
    if (n_opt > 0 && !out_release) return ANIM_EBADARG;

    unsigned char on_table[KEYSET_N];
    unsigned char named[KEYSET_N];
    keyset_clear(on_table);
    keyset_clear(named);
    for (int i = 0; i < n_table; i++) keyset_add_card(on_table, table_cards[i]);
    for (int i = 0; i < n_named; i++) keyset_add_card(named, named_cards[i]);

    int n = 0;
    for (int i = 0; i < n_opt; i++) {
        // On the authoritative table AND not named by this broadcast -> release.
        if (keyset_has_card(on_table, opt_cards[i]) && !keyset_has_card(named, opt_cards[i])) {
            if (n >= cap) return ANIM_ECAP;
            out_release[n++] = i;
        }
    }
    return n;
}

static uint64_t conflict_bit(int id) {
    return (id >= 0 && id < 52) ? ((uint64_t)1 << id) : 0;
}

// THE TRANSPORT (anim_plan.h). Session-scoped, set once at initialization,
// deliberately unset until somebody says - see anim_conflict_verdict for the
// one question that reads it.
static int g_anim_transport = ANIM_TRANSPORT_UNSET;

int anim_set_transport(int transport) {
    if (transport != ANIM_TRANSPORT_UNSET && transport != ANIM_TRANSPORT_CHAIN
        && transport != ANIM_TRANSPORT_SERVER) return ANIM_EBADARG;
    g_anim_transport = transport;
    return ANIM_EOK;
}

int anim_transport(void) { return g_anim_transport; }

// THE SERVER'S EXTRA QUESTION. Asked only where the shared tests could not
// account for the card: could this card still be accepted, so that a red flight
// now would be undone by its own broadcast a moment later?
//
// Order matters. A sweep that took the table is evidence against anything it
// did not name, cover or not; and the capacity rule after that is an ATTACK
// rule (game.c handle_attack DEFENDER_CAPACITY), so a legal in-flight attack
// keeps total attacks <= the defender's hand and never false-reverts.
static int server_may_still_accept(const AnimServerHope *h) {
    if (h->table_cleared) return 0;
    if (h->is_cover)      return 1;
    if (h->pending_attacks <= 0) return 1;
    return h->final_uncovered + h->pending_attacks <= h->defender_hand;
}

int anim_resolve_unconfirmed_attack_covers(const AnimPending *pending, int n_pending,
                                           const Card *server_table, int n_server_table,
                                           const AnimEvent *events, int n_events,
                                           const AnimFinalState *fin,
                                           AnimResolve *out) {
    if (!out || !fin) return ANIM_EBADARG;
    if (n_pending < 0 || n_pending > ANIM_MAX_CARDS) return ANIM_EBADARG;
    if (n_pending > 0 && !pending) return ANIM_EBADARG;
    if (n_server_table < 0 || (n_server_table > 0 && !server_table)) return ANIM_EBADARG;
    if (n_events < 0 || (n_events > 0 && !events)) return ANIM_EBADARG;
    out->n_revert = out->n_merge = out->n_clear = 0;
    if (n_pending == 0) return ANIM_EOK;

    // THE SAME FACTS AN ARRIVING CHAIN BUILDS, said in the server's words. What
    // the stream MOVES is the sweep: a pickup or trash names the cards it
    // carries off, and those are exactly the ones a revert would fly home out
    // of somebody else's hand. What it VOUCHES for is the authoritative table.
    AnimConflictFacts f;
    f.incoming_moved = f.table_at_open = f.my_hand_at_open = 0;
    int table_cleared = 0;
    int moved[ANIM_MAX_CARD_POOL];
    const int n_moved = anim_conflict_sweep(events, n_events, moved,
                                            (int)(sizeof moved / sizeof moved[0]),
                                            &table_cleared);
    if (n_moved < 0) return n_moved;
    for (int i = 0; i < n_moved; i++) f.incoming_moved |= conflict_bit(moved[i]);
    for (int i = 0; i < n_server_table; i++)
        f.table_at_open |= conflict_bit(card_to_id(server_table[i]));

    // The capacity scalars. A broadcast that shows SOME of my pending cards is
    // judged card by card from here - the ones it shows are standing and the
    // ones it does not still have to answer for themselves. The rule this
    // replaces short-circuited the whole set on the first accepted card; no
    // caller can produce that shape (AnimationContext only asks when NONE is
    // accepted) and per-card is the question actually being asked.
    AnimServerHope hope;
    hope.table_cleared = table_cleared;
    hope.pending_attacks = 0;
    hope.defender_hand = 0;
    hope.final_uncovered = fin->final_uncovered_attacks;
    if (fin->defender >= 0 && fin->defender < fin->n_players
        && fin->n_players <= MAX_PLAYERS) {
        hope.defender_hand = fin->hand_length[fin->defender];
    }
    for (int i = 0; i < n_pending; i++) {
        const int id = card_to_id(pending[i].card);
        // One of my own plays always has an identity; a card with none is a
        // caller mistake, not a masked back that could be kept.
        if (id < 0 || id >= 52) return ANIM_EBADARG;
        if (!pending[i].is_cover) hope.pending_attacks++;
    }

    for (int i = 0; i < n_pending; i++) {
        const int id = card_to_id(pending[i].card);
        hope.is_cover = pending[i].is_cover;
        const int v = anim_conflict_verdict(id, ANIM_DEST_TABLE, &f, &hope);
        if (v < 0) return v;
        if (v == ANIM_CONFLICT_CLEAR)       out->clear[out->n_clear++] = i;
        else if (v == ANIM_CONFLICT_REVERT) out->revert[out->n_revert++] = i;
        // A KEEP the server table already shows needs no merging into a state
        // that has it - that is the per-event dedup's card, and putting it in
        // `merge` would make its own confirming event look un-optimistic and
        // animate it a second time.
        else if (!(f.table_at_open & conflict_bit(id))) out->merge[out->n_merge++] = i;
    }
    return ANIM_EOK;
}

// ---- the conflict model ---------------------------------------------------
// See anim_plan.h. The three sets are u64 bitsets over dense card ids, so every
// membership test here is one shift and one and.


// The sweep, from the arriving stream's own events. See anim_plan.h: the
// "pickup or trash" test is the rule that sets table_cleared, so it is written
// once here and both doors onto the conflict model come through it. It used to
// be inline in anim_resolve_unconfirmed_attack_covers, which meant the web's
// wasm entry re-derived it in TypeScript.
int anim_conflict_sweep(const AnimEvent *events, int n_events,
                        int *moved_out, int cap, int *table_cleared_out) {
    if (n_events < 0 || (n_events > 0 && !events)) return ANIM_EBADARG;
    if (cap < 0 || (cap > 0 && !moved_out)) return ANIM_EBADARG;
    int cleared = 0, n = 0;
    for (int e = 0; e < n_events; e++) {
        if (events[e].type != ANIM_EVT_PICKUP && events[e].type != ANIM_EVT_CARDS_TO_TRASH)
            continue;
        // The sweep happened even if it named no card this viewer can see.
        cleared = 1;
        if (!events[e].cards) continue;
        // A masked back names nothing, so it cannot make anything CLEAR.
        if (events[e].mask_cards) continue;
        for (int k = 0; k < events[e].n_cards; k++) {
            const int id = card_to_id(events[e].cards[k]);
            if (id < 0 || id >= 52) continue;
            if (n >= cap) return ANIM_ECAP;
            moved_out[n++] = id;
        }
    }
    if (table_cleared_out) *table_cleared_out = cleared;
    return n;
}

int anim_conflict_facts(const int *moved_ids, int n_moved,
                        const unsigned char *open_table, int n_open_battles,
                        const unsigned char *my_hand_ids, int n_my_hand,
                        AnimConflictFacts *out) {
    if (!out) return ANIM_EBADARG;
    if (n_moved < 0 || n_open_battles < 0 || n_my_hand < 0) return ANIM_EBADARG;
    if (n_moved > 0 && !moved_ids) return ANIM_EBADARG;
    if (n_open_battles > 0 && !open_table) return ANIM_EBADARG;
    if (n_my_hand > 0 && !my_hand_ids) return ANIM_EBADARG;
    out->incoming_moved = out->table_at_open = out->my_hand_at_open = 0;

    for (int i = 0; i < n_moved; i++) out->incoming_moved |= conflict_bit(moved_ids[i]);
    // Both sides of a battle STAND: a covered attack and the cover that covers
    // it are each at their post spot, and dropping the cover would false-revert
    // a standing one.
    for (int i = 0; i < n_open_battles; i++) {
        out->table_at_open |= conflict_bit(open_table[2 * i]);
        const unsigned char cover = open_table[2 * i + 1];
        if (cover != ANIM_TABLE_NONE) out->table_at_open |= conflict_bit(cover);
    }
    for (int i = 0; i < n_my_hand; i++) out->my_hand_at_open |= conflict_bit(my_hand_ids[i]);
    return ANIM_EOK;
}

int anim_conflict_verdict(int card_id, int dest, const AnimConflictFacts *facts,
                          const AnimServerHope *hope) {
    if (!facts) return ANIM_EBADARG;
    // A masked back names nothing and landed into a badge - there is no view to
    // fly home, so the newest sequence's count freeze owns it from here.
    if (card_id == ANIM_CARD_NONE) return ANIM_CONFLICT_KEEP;
    if (card_id < 0 || card_id >= 52) return ANIM_EBADARG;
    if (dest != ANIM_DEST_POOL && dest != ANIM_DEST_TABLE && dest != ANIM_DEST_MY_HAND)
        return ANIM_EBADARG;
    const uint64_t bit = conflict_bit(card_id);
    // CLEAR FIRST. A card the incoming stream moves may also stand on its
    // opening table (a pickup's do by definition); the replay taking it off IS
    // the animation, and a red flight first is the flicker.
    if (facts->incoming_moved & bit) return ANIM_CONFLICT_CLEAR;
    // A pool has no persistent per-card view to fly back from.
    if (dest == ANIM_DEST_POOL) return ANIM_CONFLICT_KEEP;
    // STANDING where the motion put it, on the board the newest truth vouches
    // for. The server calls that board the authoritative table and iMessage
    // calls it the arriving chain's opening board; same question, same set.
    const uint64_t standing = (dest == ANIM_DEST_TABLE) ? facts->table_at_open
                                                        : facts->my_hand_at_open;
    if (standing & bit) return ANIM_CONFLICT_KEEP;

    // NOT ACCOUNTED FOR - and this is the whole of the transport. No default:
    // guessing here is the A1 roster cutover's mistake in another costume.
    if (g_anim_transport == ANIM_TRANSPORT_UNSET) return ANIM_ETRANSPORT;
    if (g_anim_transport == ANIM_TRANSPORT_SERVER) {
        if (!hope) return ANIM_EBADARG;
        if (server_may_still_accept(hope)) return ANIM_CONFLICT_KEEP;
    }
    return ANIM_CONFLICT_REVERT;
}

int anim_conflict_dest(int event_type, int seat, int my_seat) {
    switch (event_type) {
        case ANIM_EVT_ATTACK_PASS:
        case ANIM_EVT_COVER:
        case ANIM_EVT_DEFENDER_MOVE:
            return ANIM_DEST_TABLE;
        case ANIM_EVT_DEAL:
        case ANIM_EVT_REFILL:
        case ANIM_EVT_PICKUP:
            return (seat == my_seat) ? ANIM_DEST_MY_HAND : ANIM_DEST_POOL;
        default:
            return ANIM_DEST_POOL;
    }
}

int anim_conflict_reversal(const AnimConflictMotion *motions, int n_motions,
                           const int *group_sizes, int n_groups,
                           const AnimConflictFacts *facts,
                           AnimConflictPlan *out) {
    if (!out || !facts) return ANIM_EBADARG;
    if (n_motions < 0 || n_motions > ANIM_MAX_CONFLICT_MOTIONS) return ANIM_ECAP;
    if (n_groups < 0 || n_groups > ANIM_MAX_CONFLICT_GROUPS) return ANIM_ECAP;
    if (n_motions > 0 && !motions) return ANIM_EBADARG;
    if (n_groups > 0 && !group_sizes) return ANIM_EBADARG;

    out->n_verdicts = 0;
    out->n_steps = 0;
    out->n_order = 0;

    // The groups must account for exactly the motions handed over - a slicing
    // that runs past the array, or leaves motions in no group, is describing
    // some other sequence.
    int total = 0;
    for (int g = 0; g < n_groups; g++) {
        if (group_sizes[g] < 0) return ANIM_EBADARG;
        total += group_sizes[g];
        if (total > n_motions) return ANIM_EBADARG;
    }
    if (total != n_motions) return ANIM_EBADARG;

    // A reversal is asked only about motions the caller ALREADY knows are
    // doomed, which is a thing only a total order over complete chains can
    // know. There is no AnimServerHope to pass and none would mean anything.
    if (anim_transport() == ANIM_TRANSPORT_SERVER) return ANIM_EBADARG;
    for (int i = 0; i < n_motions; i++) {
        const int v = anim_conflict_verdict(motions[i].card_id, motions[i].dest, facts, 0);
        if (v < 0) return v;
        out->verdicts[i] = (unsigned char)v;
    }
    out->n_verdicts = n_motions;

    // The starting index of each group, so the walk can run backwards.
    int start[ANIM_MAX_CONFLICT_GROUPS];
    int at = 0;
    for (int g = 0; g < n_groups; g++) { start[g] = at; at += group_sizes[g]; }

    for (int g = n_groups - 1; g >= 0; g--) {
        const int n_before = out->n_order;
        for (int k = 0; k < group_sizes[g]; k++) {
            const int i = start[g] + k;
            if (out->verdicts[i] == ANIM_CONFLICT_REVERT) out->order[out->n_order++] = i;
        }
        const int flown = out->n_order - n_before;
        // A group the verdicts emptied is dropped, not played as silence.
        if (flown > 0) out->step_count[out->n_steps++] = flown;
    }
    return out->n_steps;
}

// ---- the board's own sets and small rules ---------------------------------
// See anim_plan.h. Card sets are u64 bitsets over dense ids, so every one of
// these is set algebra a compiler turns into a handful of instructions.

// A dense id as a bit. An id outside the deck - a viewer-masked back, or a
// corrupt byte - contributes nothing, which is the same thing the conflict
// model says about it.
static uint64_t ap_bit(int id) {
    return (id >= 0 && id < 52) ? ((uint64_t)1 << id) : 0;
}

uint64_t anim_veil_veiled(uint64_t hidden, uint64_t pending_open,
                          int has_hand_before, uint64_t hand_before,
                          int has_my_hand, uint64_t my_hand) {
    uint64_t ids = hidden | pending_open;
    // Live play: cards this move just put in my hand, which nothing has
    // pre-hidden yet (that happens a paint late). Both halves are needed - no
    // hand is no fan to veil, and no "before" is nothing to diff against.
    if (has_hand_before && has_my_hand) ids |= (my_hand & ~hand_before);
    return ids;
}

uint64_t anim_veil_flying(uint64_t hidden, uint64_t pre_hidden) {
    return hidden & ~pre_hidden;
}

uint64_t anim_veil_hand_slot_deferred(uint64_t veiled, uint64_t flying,
                                      uint64_t holdback) {
    return veiled & ~flying & ~holdback;
}

uint64_t anim_veil_fan(uint64_t veiled, uint64_t holdback) {
    return veiled & ~holdback;
}

void anim_veil_grid(int sweeping, uint64_t veiled,
                    uint64_t swept_flown, uint64_t sweep_unplaced,
                    uint64_t sweep_arriving, uint64_t flying,
                    uint64_t *out_hidden, uint64_t *out_flying) {
    // A sweep answers off its own two sets, one for each end of the sequence:
    // what has left, and what has not arrived. The only thing that can be
    // coming DOWN onto a table everything else is leaving is sweep_arriving.
    const uint64_t h = sweeping ? (swept_flown | sweep_unplaced) : veiled;
    const uint64_t f = sweeping ? sweep_arriving : flying;
    if (out_hidden) *out_hidden = h;
    if (out_flying) *out_flying = f;
}

uint64_t anim_veil_sweep_unplaced(uint64_t placed, uint64_t table) {
    return placed & table;
}

void anim_veil_teardown(uint64_t opened, uint64_t orphaned, int is_newest,
                        uint64_t *out_reveal, uint64_t *out_carry) {
    if (out_reveal) *out_reveal = is_newest ? (opened | orphaned) : 0;
    if (out_carry)  *out_carry  = is_newest ? 0 : (orphaned | opened);
}

void anim_veil_handover(uint64_t standing, uint64_t placing,
                        uint64_t *out_reveal, uint64_t *out_veil) {
    if (out_reveal) *out_reveal = standing & ~placing;
    if (out_veil)   *out_veil   = placing;
}

int anim_veil_unstarted_replay(int replay_pending, int n_events) {
    return (replay_pending && n_events > 0) ? 1 : 0;
}

int anim_holdback_is_mine(int armed_at, int teardown_at) {
    return armed_at <= teardown_at ? 1 : 0;
}

uint64_t anim_selection_after_tap(uint64_t selection, int card_id, uint64_t hand) {
    // Sweep first: an id that has since left my hand is dropped whatever this
    // tap was for. A stale one can only ever go on to disable the action bar.
    uint64_t next = selection & hand;
    const uint64_t bit = ap_bit(card_id);
    if (!bit || !(hand & bit)) return next;
    return (next & bit) ? (next & ~bit) : (next | bit);
}

int anim_is_placement(int event_type) {
    switch (event_type) {
        case ANIM_EVT_ATTACK_PASS:
        case ANIM_EVT_DEFENDER_MOVE:
        case ANIM_EVT_COVER:
            return 1;
        default:
            return 0;
    }
}

int anim_is_my_placement(int event_type, int seat, int my_seat) {
    // A seatless viewer owns nothing. The kernel spends -1 on "no particular
    // player" as well as on "no seat", so seat == my_seat is only meaningful
    // once somebody is actually seated.
    if (my_seat < 0) return 0;
    return (seat == my_seat && anim_is_placement(event_type)) ? 1 : 0;
}

int anim_fan_cards(const unsigned char *hand, int n_hand,
                   const unsigned char *held, int n_held,
                   unsigned char *out, int cap) {
    if (!out || n_hand < 0 || n_held < 0) return ANIM_EBADARG;
    if (n_hand > 0 && !hand) return ANIM_EBADARG;
    if (n_held > 0 && !held) return ANIM_EBADARG;
    if (cap < n_hand) return ANIM_ECAP;
    uint64_t present = 0;
    int w = 0;
    for (int i = 0; i < n_hand; i++) { out[w++] = hand[i]; present |= ap_bit(hand[i]); }
    for (int i = 0; i < n_held; i++) {
        const uint64_t bit = ap_bit(held[i]);
        if (!bit || (present & bit)) continue;
        if (w >= cap) return ANIM_ECAP;
        out[w++] = held[i];
        present |= bit;
    }
    return w;
}

int anim_laid_count(const unsigned char *hand, int n_hand,
                    const unsigned char *held, int n_held, uint64_t deferred) {
    static unsigned char fan[52];
    const int n = anim_fan_cards(hand, n_hand, held, n_held, fan, (int)sizeof fan);
    if (n < 0) return n;
    int laid = 0;
    for (int i = 0; i < n; i++) if (!(deferred & ap_bit(fan[i]))) laid++;
    return laid;
}

int anim_hand_laid_out(const unsigned char *cards, int n_cards, uint64_t deferred,
                       const unsigned char *order, int n_order,
                       unsigned char *out, int cap) {
    if (!out || n_cards < 0 || n_order < 0) return ANIM_EBADARG;
    if (n_cards > 0 && !cards) return ANIM_EBADARG;
    if (n_order > 0 && !order) return ANIM_EBADARG;

    // Which ids are actually in the hand and not deferred. A deferred card
    // reserves nothing and must not be placed by `order` either.
    uint64_t live = 0;
    for (int i = 0; i < n_cards; i++) {
        const uint64_t bit = ap_bit(cards[i]);
        if (bit && !(deferred & bit)) live |= bit;
    }

    uint64_t seen = 0;
    int w = 0;
    for (int i = 0; i < n_order; i++) {
        const uint64_t bit = ap_bit(order[i]);
        // Stale ids (a played card the sticky memory still remembers) and
        // repeats drop out by construction.
        if (!bit || !(live & bit) || (seen & bit)) continue;
        if (w >= cap) return ANIM_ECAP;
        out[w++] = order[i];
        seen |= bit;
    }
    for (int i = 0; i < n_cards; i++) {
        const uint64_t bit = ap_bit(cards[i]);
        if (!bit || !(live & bit) || (seen & bit)) continue;
        if (w >= cap) return ANIM_ECAP;
        out[w++] = cards[i];
        seen |= bit;
    }
    return w;
}

uint64_t anim_table_card_ids(const unsigned char *table, int n_battles) {
    uint64_t ids = 0;
    if (!table || n_battles <= 0) return 0;
    // BOTH cells go through ap_bit and nothing else. An empty cell and a card
    // nobody can name are both outside the deck, so ap_bit already answers zero
    // for them; a second `!= ANIM_TABLE_NONE` branch beside it was unkillable
    // code - no input could tell the two spellings apart - and the static
    // assert in the header is what keeps that true.
    for (int i = 0; i < n_battles; i++) {
        ids |= ap_bit(table[2 * i]);
        ids |= ap_bit(table[2 * i + 1]);
    }
    return ids;
}

int anim_table_covers(const unsigned char *outer, int n_outer,
                      const unsigned char *inner, int n_inner) {
    if (n_outer < 0 || n_inner < 0) return ANIM_EBADARG;
    if (n_outer > 0 && !outer) return ANIM_EBADARG;
    if (n_inner > 0 && !inner) return ANIM_EBADARG;
    // A card the caller could not name is never accounted for. It has no bit,
    // so without this it drops out of the subset test entirely and the swap is
    // granted over a table that IS losing a card.
    for (int i = 0; i < 2 * n_inner; i++) if (inner[i] == ANIM_TABLE_UNKNOWN) return 0;
    const uint64_t have = anim_table_card_ids(outer, n_outer);
    const uint64_t need = anim_table_card_ids(inner, n_inner);
    return (need & ~have) == 0 ? 1 : 0;
}

int anim_covered_sweep_accepts(int paired,
                               const unsigned char *pre, int n_pre,
                               const unsigned char *cur, int n_cur) {
    if (!paired || n_pre <= 0) return 0;
    return anim_table_covers(pre, n_pre, cur, n_cur);
}

int anim_shown_table(int n_live, int n_sweep, int n_pending, int *out_sweeping) {
    int which = ANIM_SHOWN_NONE, sweeping = 0;
    if (n_live > 0)         { which = ANIM_SHOWN_LIVE; }
    else if (n_sweep > 0)   { which = ANIM_SHOWN_SWEEP;   sweeping = 1; }
    else if (n_pending > 0) { which = ANIM_SHOWN_PENDING; sweeping = 1; }
    if (out_sweeping) *out_sweeping = sweeping;
    return which;
}

int anim_finish_rows(const unsigned char *elimination, int n_elim,
                     int game_over, int n_players, int my_seat,
                     AnimFinishRow *out, int cap) {
    if (!out || n_elim < 0 || n_players < 0) return ANIM_EBADARG;
    if (n_elim > 0 && !elimination) return ANIM_EBADARG;
    const int rows = n_elim + (game_over >= 0 ? 1 : 0);
    if (rows > cap) return ANIM_ECAP;
    int w = 0;
    for (int i = 0; i < n_elim; i++) {
        out[w].place = i + 1;
        out[w].seat = elimination[i];
        out[w].is_you = (my_seat >= 0 && (int)elimination[i] == my_seat) ? 1 : 0;
        w++;
    }
    // The fool is the one seat still holding cards, and takes the last place -
    // which is the SEAT COUNT, not the row count: a row knows it is the fool by
    // place == total.
    if (game_over >= 0) {
        out[w].place = n_players;
        out[w].seat = game_over;
        out[w].is_you = (my_seat >= 0 && game_over == my_seat) ? 1 : 0;
        w++;
    }
    return w;
}

int anim_shown_ledger_allows(int claim, int sequencing) {
    return (claim == ANIM_CLAIM_BYSTANDER && sequencing) ? 0 : 1;
}
