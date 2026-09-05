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

int anim_resolve_unconfirmed_attack_covers(const AnimPending *pending, int n_pending,
                                           const Card *server_table, int n_server_table,
                                           const AnimEvent *events, int n_events,
                                           const AnimFinalState *fin,
                                           AnimResolve *out) {
    if (!out || !fin) return ANIM_EBADARG;
    if (n_pending < 0 || n_pending > ANIM_MAX_CARDS) return ANIM_EBADARG;
    if (n_pending > 0 && !pending) return ANIM_EBADARG;
    out->n_revert = out->n_merge = out->n_clear = 0;

    // Server already shows (some of) my cards -> accepted; the per-event dedup
    // upstream handles them, this branch does not apply.
    unsigned char on_table[KEYSET_N];
    keyset_clear(on_table);
    for (int i = 0; i < n_server_table; i++) keyset_add_card(on_table, server_table[i]);
    int accepted = 0;
    for (int i = 0; i < n_pending; i++) {
        if (keyset_has_card(on_table, pending[i].card)) { accepted = 1; break; }
    }
    if (n_pending == 0 || accepted) return ANIM_EOK;   // {revert:[], merge:[], clear:[]}

    // SPECIAL CASE: a pickup/cards_to_trash cleared the table. A card the clear
    // event NAMES was on the table (accepted) and is being carried off -> CLEAR
    // (drop tracking, no revert). A card NOT named never reached the table
    // (genuinely too slow) -> REVERT.
    int has_table_clear = 0;
    unsigned char swept[KEYSET_N];
    keyset_clear(swept);
    for (int e = 0; e < n_events; e++) {
        if (events[e].type == ANIM_EVT_PICKUP || events[e].type == ANIM_EVT_CARDS_TO_TRASH) {
            has_table_clear = 1;
            for (int k = 0; k < events[e].n_cards; k++) {
                if (events[e].cards) keyset_add_card(swept, events[e].cards[k]);
            }
        }
    }
    if (has_table_clear) {
        for (int i = 0; i < n_pending; i++) {
            if (keyset_has_card(swept, pending[i].card)) out->clear[out->n_clear++] = i;
            else                                         out->revert[out->n_revert++] = i;
        }
        return ANIM_EOK;
    }

    // Capacity check (an ATTACK rule — game.c handle_attack DEFENDER_CAPACITY):
    // can the defender still take every attack? Covers are the defender's own
    // play and are excluded. A legal in-flight attack keeps totalAttacks <=
    // defenderHand, so this never false-reverts one.
    int n_pending_attacks = 0;
    for (int i = 0; i < n_pending; i++) if (!pending[i].is_cover) n_pending_attacks++;
    int defender_hand = 0;
    if (fin->defender >= 0 && fin->defender < fin->n_players) {
        defender_hand = fin->hand_length[fin->defender];
    }
    int total_attacks = fin->final_uncovered_attacks + n_pending_attacks;
    if (n_pending_attacks > 0 && total_attacks > defender_hand) {
        // Attack invalidated by an earlier attack -> revert the attacks, keep covers.
        for (int i = 0; i < n_pending; i++) {
            if (pending[i].is_cover) out->merge[out->n_merge++] = i;
            else                     out->revert[out->n_revert++] = i;
        }
        return ANIM_EOK;
    }

    // Defender can hold them -> keep and merge everything.
    for (int i = 0; i < n_pending; i++) out->merge[out->n_merge++] = i;
    return ANIM_EOK;
}
