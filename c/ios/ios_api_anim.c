// ios_api_anim.c - THE ANIMATION SURFACE: what a board draws, and in what
// order (see ios/include/ios_api.h).
//
// Split out of ios_api.c by domain, unchanged. Every entry here is a crossing
// into anim_plan.c, and nearly every one of them READS NOTHING BUT ITS
// ARGUMENTS: the stream and the board cross WITH the question, because a board
// animates the stream it was HANDED (a staged bout end is cut in half and the
// settlement withheld) and because a SwiftUI body cannot await the actor the
// resident game lives behind. The two exceptions hold their answer where the
// caller reads it - g_anim_plan and g_anim_beats, one slot each.
//
// The surface plan (fio_msg_surface_plan, which needs an envelope as well) is
// in ios_api_msg.c; the client's board slot is in ios_api_view.c.
//
// WHY ANY OF IT IS IN C (§4.4 / A3). The animation plan — which card flies
// where, in what order — is derived by the kernel and always has been: the
// website decodes the evwire stream and plays it, never deriving anything.
// Offline there is no server to send that stream, and the iOS design's answer
// used to be a Swift diff engine (`BoardDiff.swift`, "given (old GameView, new
// GameView) produce moves"). That would have been a THIRD implementation of the
// same derivation, and legacy the day it was written.
//
// So: the same kernel hooks that feed the web's evwire feed this too
// (evwire_walk, the one derivation), and Swift decodes the packed sequence.

#include "ios_api.h"

#include "game.h"
#include "card.h"
#include "legal.h"
#include "anim_plan.h"

// ---------- the one table layout, read ---------------------------------------
//
// ONE BYTE FOR "NO CARD" AND ONE FOR "A CARD NOBODY CAN NAME", across every
// header that spells them. The readers below tell the two apart and the set
// rules in anim_plan.c let the range check answer for both, so a sentinel that
// drifted in one header would be a card in another.
_Static_assert(FIO_PRETABLE_NONE == ANIM_TABLE_NONE,
               "one 'no card here' byte for a table, not two");
_Static_assert(ANIM_TABLE_NONE == LEGAL_WIRE_NONE,
               "…and it is the same byte the menu wire and PlayBoard use");
_Static_assert(FIO_CONFLICT_NONE == ANIM_TABLE_NONE,
               "…and the conflict wire's");
_Static_assert(FIO_TABLE_UNKNOWN == ANIM_TABLE_UNKNOWN,
               "one 'a card is here that cannot be named' byte, not two");
_Static_assert(FIO_CARD_NONE == CARD_NONE_SUIT && FIO_CARD_NONE == CARD_NONE_VALUE,
               "the host's bare-cover pair is the kernel's CARD_NONE");

// What a table's cells hold: -1 for a byte that is neither a card nor a
// sentinel (a corrupt wire), 0 for a table with a FIO_TABLE_UNKNOWN cell in
// it, 1 for a table every cell of which is a card or bare. The readers that
// LAY a table out take 0 as "no board" (ios_api.h, fio_table_encode: a cell
// nobody can name has no face to lay); the ones that reason by identity take
// it as a cell that vouches for nothing. Stated once so the two cannot come
// to disagree about which bytes are corrupt.
static int table_cells(const uint8_t *p, int n) {
    int unknown = 0;
    for (int i = 0; i < 2 * n; i++) {
        if (p[i] < 52 || p[i] == ANIM_TABLE_NONE) continue;
        if (p[i] == ANIM_TABLE_UNKNOWN) { unknown = 1; continue; }
        return -1;
    }
    return unknown ? 0 : 1;
}

// ---------- animation core (c/src/anim_plan.h) -----------------------------
//
// The layout is documented once, in ios_api.h. Like fio_beats_packed, this
// reads nothing but its arguments: the stream crosses WITH the question because
// a board animates the stream it was HANDED (a staged bout end is cut in half
// and the settlement withheld), and because a SwiftUI body cannot await the
// actor the resident game lives behind.

_Static_assert(FIO_PLAN_SEATS == MAX_PLAYERS,
               "the plan wire's seat block must be the kernel's table size");
_Static_assert(FIO_PLAN_BATTLES == ANIM_PLAN_ROW_MAX,
               "the plan wire's row block must be the kernel's plan row width");
// THE PLAN AND THE BEATS THEMSELVES, not copies of them: the caller reads them
// at fio_anim_plan_ptr / fio_beats_ptr through the generated snapshot readers.
static AnimPlan  g_anim_plan;
static AnimBeats g_anim_beats;

int fio_anim_plan(const uint8_t *in, int len) {
    if (!in || len < 5) return FIO_EBADARG;
    if (in[0] != FIO_PLAN_VERSION) return FIO_EPARSE;
    const int np = in[1];
    const int n = in[2];
    if (np < 2 || np > MAX_PLAYERS) return FIO_EPARSE;
    if (n > ANIM_MAX_STEPS) return FIO_ECAP;

    int p = 6;
    if (p + np > len) return FIO_EPARSE;
    // The final board's flipped trump - read only by the boardless fallback,
    // but it crosses unconditionally so the header stays fixed-width.
    const Card final_flipped = in[5] < 52 ? card_of_id(in[5]) : CARD_NONE;
    int final_hand[MAX_PLAYERS];
    for (int s = 0; s < np; s++) final_hand[s] = in[p++];

    static AnimPlanEvent evs[ANIM_MAX_STEPS];
    static int  hands[ANIM_MAX_STEPS][MAX_PLAYERS];
    static Card pool[ANIM_MAX_CARD_POOL];
    int n_pool = 0;
    for (int i = 0; i < n; i++) {
        // The WHOLE event is bounded before any of it is read - the counts that
        // decide its length (n_cards, n_ids) first, then the extent they imply.
        // A bound checked after the reads it guards is not a bound; an optimiser
        // is free to sink the loads past it.
        if (p + 6 > len) return FIO_EPARSE;
        const int n_cards = in[p + 4], n_ids = in[p + 5];
        if (n_cards > ANIM_MAX_CARDS || n_ids > n_cards) return FIO_ECAP;
        if (p + 11 + np + n_ids > len) return FIO_EPARSE;
        if (n_pool + n_ids > ANIM_MAX_CARD_POOL) return FIO_ECAP;
        const int type = in[p], seat = in[p + 1], from = in[p + 2], to = in[p + 3];
        const int has_counts = in[p + 6], deck = in[p + 7], discard = in[p + 8];
        const Card flipped = in[p + 9] < 52 ? card_of_id(in[p + 9]) : CARD_NONE;
        p += 10;
        for (int s = 0; s < np; s++) hands[i][s] = in[p + s];
        p += np;
        Card *ids = &pool[n_pool];
        for (int k = 0; k < n_ids; k++) {
            if (in[p + k] >= 52) return FIO_EPARSE;
            ids[k] = card_of_id(in[p + k]);
        }
        n_pool += n_ids;
        p += n_ids;
        // …and the row that step committed. FIO_PRETABLE_NONE is "no board",
        // which is what a redacted table crosses as (PreTableWire.table): a row
        // that cannot be described honestly is not described at all.
        //
        // BORROWED FROM `in`, NOT COPIED. AnimPlanEvent borrows its array inputs
        // for the call (the contract EvwEvent keeps), and a row on this wire is
        // ALREADY the dense ids the kernel compares, byte for byte - unlike
        // `cards` beside it, which card_of_id genuinely transforms. Bounded
        // BEFORE the borrow, like every other field here, so the pointer handed
        // on can only span bytes the caller really owns: the guard-page sweep
        // below walks every short prefix flush against PROT_NONE and a bound
        // that is off by one faults there rather than passing.
        //
        // Safe for the same reason the wasm twin's is: `out` is written only
        // after anim_build_plan has returned, and it has copied what it keeps
        // into `plan` by then.
        if (p >= len) return FIO_EPARSE;
        const int n_bat = in[p++];
        int row_n = ANIM_NO_BOARD;
        const uint8_t *row = 0;
        if (n_bat != FIO_PRETABLE_NONE) {
            if (n_bat > FIO_PLAN_BATTLES) return FIO_ECAP;
            if (p + 2 * n_bat > len) return FIO_EPARSE;
            // The cells are checked here and nowhere later: this row is copied
            // into the freeze verbatim (anim_plan.c pre_take_board) and the
            // host lays that out BY IDENTITY, so a byte that is not a card
            // would be drawn as one. A corrupt byte refuses the plan; a card
            // nobody can name makes this step carry no row, which is what the
            // host used to send in its place.
            const int cells = table_cells(&in[p], n_bat);
            if (cells < 0) return FIO_EPARSE;
            if (cells > 0) { row = &in[p]; row_n = n_bat; }
            p += 2 * n_bat;
        }
        evs[i].type = type;
        evs[i].seat = (seat == 0xFF) ? ANIM_SEAT_NONE : seat;
        evs[i].from = from;
        evs[i].to = to;
        evs[i].cards = ids;
        evs[i].n_cards = n_cards;
        // Only REAL identities travel; a viewer-masked step lists none, and the
        // veil must leave its cards alone rather than veil a back.
        evs[i].mask_cards = (n_ids < n_cards);
        evs[i].has_counts = has_counts ? 1 : 0;
        evs[i].deck = deck;
        evs[i].discard = discard;
        evs[i].flipped = flipped;
        evs[i].hand = hands[i];
        evs[i].n_battles = row_n;
        evs[i].battles = row;
    }

    const int rc = anim_build_plan(evs, n, np, in[3], in[4], final_flipped,
                                   final_hand, &g_anim_plan);
    if (rc == ANIM_ECAP) return FIO_ECAP;
    if (rc != ANIM_EOK) return FIO_EBADARG;

    return FIO_EOK;
}

// The plan the last fio_anim_plan built (anim_plan.h AnimPlan), where it lies.
// It used to be flattened into a packed block here and read back field by field
// in AnimPlanWire.swift - 85 bytes of header, a 23-byte stride per step and a
// veil tail, stated twice. The web reads this same struct through generated
// accessors (wasm_anim_plan_ptr); the phone reads it through generated
// snapshots. Valid until the next fio_anim_plan.
const void *fio_anim_plan_ptr(void) { return &g_anim_plan; }


int fio_anim_should_drop_stale(int has_last, int last, int has_incoming, int incoming) {
    return anim_should_drop_stale(has_last, last, has_incoming, incoming);
}

// ---------- the shape of a sequence ----------------------------------------
//
// The layout is documented once, in ios_api.h. Reads nothing but its arguments:
// the stream crosses WITH the question because the board's stream is often not
// the resident game's (a staged bout end is cut in half and the settlement
// withheld), and because a SwiftUI body cannot await the actor it lives behind.

// Every dense id the input names, across the whole stream. A turn is at most
// ANIM_MAX_BEATS events and no event names more cards than a full table sweep.
#define FIO_BEATS_MAX_IDS 1024

int fio_beats(const uint8_t *in, int len) {
    if (!in || len < 2) return FIO_EBADARG;
    if (in[0] != FIO_BEATS_VERSION) return FIO_EPARSE;
    const int n = in[1];
    if (n > ANIM_MAX_BEATS) return FIO_ECAP;

    AnimBeatEvent evs[ANIM_MAX_BEATS];
    Card ids[FIO_BEATS_MAX_IDS];
    int n_ids = 0, p = 2;
    for (int i = 0; i < n; i++) {
        if (p + 5 > len) return FIO_EPARSE;
        const int type = in[p];
        const int seat = in[p + 1];
        const int has_good = in[p + 2];
        const int good = in[p + 3];
        const int k = in[p + 4];
        p += 5;
        if (p + k > len) return FIO_EPARSE;
        if (n_ids + k > FIO_BEATS_MAX_IDS) return FIO_ECAP;
        evs[i].type = type;
        evs[i].seat = (seat == 0xFF) ? ANIM_SEAT_NONE : seat;
        evs[i].cards = &ids[n_ids];
        evs[i].n_cards = k;
        evs[i].mask_cards = 0;   // only real identities are ever listed
        evs[i].good_mask = has_good ? good : ANIM_NO_MASK;
        for (int c = 0; c < k; c++) {
            if (in[p + c] >= 52) return FIO_EPARSE;
            ids[n_ids + c] = card_of_id(in[p + c]);
        }
        n_ids += k;
        p += k;
    }

    const int r = anim_build_beats(evs, n, &g_anim_beats);
    if (r == ANIM_ECAP) return FIO_ECAP;
    if (r < 0) return FIO_EBADARG;
    return FIO_EOK;
}

// The beats the last fio_beats built (anim_plan.h AnimBeats), where they lie.
// Same story as the plan above: a packed block written here and read back in
// BeatWire.swift, one layout stated twice.
const void *fio_beats_ptr(void) { return &g_anim_beats; }


// ---------- the pre-bout table ---------------------------------------------
//
// The layout is documented once, in ios_api.h. Like fio_beats_packed this reads
// nothing but its arguments, and the prior board travels with the stream
// because a single-action pickup turn has no earlier board of its own.

// One table off the wire: `n` battles at `p`, bounded against `end`. Returns
// the bytes consumed, or -1 for a table that runs off the buffer or names a
// card that is not one. `*n_board` comes back as `n`, or ANIM_NO_BOARD for a
// table holding a card nobody can name: the answer here is a table to be laid
// out, so such a board is consumed off the wire and then not a board
// (ios_api.h, fio_table_encode).
static int pretable_read(const uint8_t *p, const uint8_t *end, int n, int *n_board) {
    if (n < 0 || p + 2 * n > end) return -1;
    const int cells = table_cells(p, n);
    if (cells < 0) return -1;
    *n_board = cells > 0 ? n : ANIM_NO_BOARD;
    return 2 * n;
}

int fio_pre_bout_table_packed(const uint8_t *in, int len, char *out, int cap) {
    if (!in || !out || len < 3) return FIO_EBADARG;
    if (in[0] != FIO_PRETABLE_VERSION) return FIO_EPARSE;
    const int n = in[1];
    if (n > ANIM_MAX_STEPS) return FIO_ECAP;
    const uint8_t *end = in + len;

    int p = 2;
    int prior_n = (in[p] == FIO_PRETABLE_NONE) ? ANIM_NO_BOARD : in[p];
    p++;
    const uint8_t *prior = in + p;
    if (prior_n > 0) {
        const int took = pretable_read(in + p, end, prior_n, &prior_n);
        if (took < 0) return FIO_EPARSE;
        p += took;
    }

    static AnimPreEvent evs[ANIM_MAX_STEPS];
    for (int i = 0; i < n; i++) {
        // The WHOLE event is bounded before any of it is read - the counts that
        // decide its length first, then the extent they imply. A bound checked
        // after the reads it guards is not a bound.
        if (in + p + 2 > end) return FIO_EPARSE;
        const int type = in[p];
        int n_bat = (in[p + 1] == FIO_PRETABLE_NONE) ? ANIM_NO_BOARD : in[p + 1];
        p += 2;
        evs[i].type = type;
        evs[i].battles = in + p;
        if (n_bat > 0) {
            const int took = pretable_read(in + p, end, n_bat, &n_bat);
            if (took < 0) return FIO_EPARSE;
            p += took;
        }
        evs[i].n_battles = n_bat;
        if (in + p + 1 > end) return FIO_EPARSE;
        const int n_cards = in[p];
        p++;
        if (in + p + n_cards > end) return FIO_EPARSE;
        for (int k = 0; k < n_cards; k++) if (in[p + k] >= 52) return FIO_EPARSE;
        evs[i].n_cards = n_cards;
        evs[i].cards = in + p;
        p += n_cards;
    }

    static AnimPreTable t;
    const int rc = anim_pre_bout_table(evs, n, prior_n, prior, &t);
    if (rc == ANIM_ECAP) return FIO_ECAP;
    if (rc < 0) return FIO_EBADARG;

    const int need = FIO_PRETABLE_HEAD + 2 * t.n_battles;
    if (cap < need) return FIO_ECAP;
    unsigned char *q = (unsigned char *)out;
    q[0] = FIO_PRETABLE_VERSION;
    q[1] = (unsigned char)t.n_battles;
    q[2] = (unsigned char)(t.paired ? 1 : 0);
    for (int i = 0; i < 2 * t.n_battles; i++) q[FIO_PRETABLE_HEAD + i] = t.battles[i];
    return need;
}

// ---------- the conflict model --------------------------------------------

// THE TRANSPORT (anim_plan.h), said once by the host. This library is linked by
// the iMessage extension, whose messages each carry the whole game in a total
// order, and by the iOS app, whose online play is confirmed by a later server
// broadcast - so it cannot be a constant here, and the kernel refuses to guess.
int fio_set_transport(int transport) { return anim_set_transport(transport); }
int fio_transport(void) { return anim_transport(); }

int fio_conflict_dest(int event_type, int seat, int my_seat) {
    return anim_conflict_dest(event_type, seat, my_seat);
}

// One dense card id off the wire: FIO_CONFLICT_NONE is "no identity here", and
// anything else off the deck is a corrupt wire rather than a case.
static int conflict_id(uint8_t b, int *out) {
    if (b == FIO_CONFLICT_NONE) { *out = ANIM_CARD_NONE; return 1; }
    if (b >= 52) return 0;
    *out = b;
    return 1;
}

int fio_conflict_packed(const uint8_t *in, int len, char *out, int cap) {
    if (!in || !out || len < 2) return FIO_EBADARG;
    if (in[0] != FIO_CONFLICT_VERSION) return FIO_EPARSE;
    const uint8_t *end = in + len;
    int p = 1;

    // Every count is bounded before the extent it implies is read - a bound
    // checked after the reads it guards is not a bound.
    static int moved[ANIM_MAX_CONFLICT_MOTIONS];
    if (in + p + 1 > end) return FIO_EPARSE;
    const int n_moved = in[p++];
    if (n_moved > ANIM_MAX_CONFLICT_MOTIONS) return FIO_ECAP;
    if (in + p + n_moved > end) return FIO_EPARSE;
    for (int i = 0; i < n_moved; i++) {
        if (!conflict_id(in[p + i], &moved[i])) return FIO_EPARSE;
    }
    p += n_moved;

    if (in + p + 1 > end) return FIO_EPARSE;
    const int n_bat = (in[p] == FIO_CONFLICT_NONE) ? 0 : in[p];
    p++;
    const uint8_t *table = in + p;
    if (in + p + 2 * n_bat > end) return FIO_EPARSE;
    // A bare cell (FIO_CONFLICT_NONE) and a card nobody can name
    // (FIO_TABLE_UNKNOWN) are BOTH taken, and both contribute nothing to the
    // standing set: this rule decides by identity, and neither has one. The
    // unknown cell is not refused the way the pre-bout table refuses it because
    // nothing here is laid out - a named card beside it still stands, and
    // refusing the whole board would revert that card for no reason. Anything
    // else off the deck is a corrupt wire.
    if (table_cells(table, n_bat) < 0) return FIO_EPARSE;
    p += 2 * n_bat;

    if (in + p + 1 > end) return FIO_EPARSE;
    const int n_hand = in[p++];
    const uint8_t *hand = in + p;
    if (in + p + n_hand > end) return FIO_EPARSE;
    for (int i = 0; i < n_hand; i++) if (hand[i] >= 52) return FIO_EPARSE;
    p += n_hand;

    if (in + p + 1 > end) return FIO_EPARSE;
    const int n_groups = in[p++];
    if (n_groups > ANIM_MAX_CONFLICT_GROUPS) return FIO_ECAP;
    if (in + p + n_groups > end) return FIO_EPARSE;
    static int groups[ANIM_MAX_CONFLICT_GROUPS];
    int n_motions = 0;
    for (int g = 0; g < n_groups; g++) {
        groups[g] = in[p + g];
        n_motions += groups[g];
    }
    p += n_groups;
    if (n_motions > ANIM_MAX_CONFLICT_MOTIONS) return FIO_ECAP;
    if (in + p + 2 * n_motions > end) return FIO_EPARSE;

    static AnimConflictMotion motions[ANIM_MAX_CONFLICT_MOTIONS];
    for (int i = 0; i < n_motions; i++) {
        if (!conflict_id(in[p + 2 * i], &motions[i].card_id)) return FIO_EPARSE;
        const int dest = in[p + 2 * i + 1];
        if (dest != FIO_CONFLICT_DEST_TABLE && dest != FIO_CONFLICT_DEST_MY_HAND
            && dest != FIO_CONFLICT_DEST_POOL) return FIO_EPARSE;
        motions[i].dest = dest;
    }

    AnimConflictFacts facts;
    if (anim_conflict_facts(moved, n_moved, table, n_bat, hand, n_hand, &facts) != ANIM_EOK) {
        return FIO_EBADARG;
    }
    static AnimConflictPlan plan;
    const int rc = anim_conflict_reversal(motions, n_motions, groups, n_groups, &facts, &plan);
    if (rc == ANIM_ECAP) return FIO_ECAP;
    if (rc == ANIM_ETRANSPORT) return FIO_ETRANSPORT;
    if (rc < 0) return FIO_EBADARG;

    const int need = 2 + plan.n_verdicts + 1 + plan.n_steps + plan.n_order;
    if (cap < need) return FIO_ECAP;
    unsigned char *q = (unsigned char *)out;
    int w = 0;
    q[w++] = FIO_CONFLICT_VERSION;
    q[w++] = (unsigned char)plan.n_verdicts;
    for (int i = 0; i < plan.n_verdicts; i++) q[w++] = plan.verdicts[i];
    q[w++] = (unsigned char)plan.n_steps;
    for (int i = 0; i < plan.n_steps; i++) q[w++] = (unsigned char)plan.step_count[i];
    for (int i = 0; i < plan.n_order; i++) q[w++] = (unsigned char)plan.order[i];
    return w;
}

// ---- the board's sets and small rules -------------------------------------
// Thin crossings: the rule is anim_plan.c's, and every one of these is a set or
// a scalar, so nothing here parses a record.

uint64_t fio_veil_veiled(uint64_t hidden, uint64_t pending_open,
                         int has_hand_before, uint64_t hand_before,
                         int has_my_hand, uint64_t my_hand) {
    return anim_veil_veiled(hidden, pending_open, has_hand_before, hand_before,
                            has_my_hand, my_hand);
}

uint64_t fio_veil_flying(uint64_t hidden, uint64_t pre_hidden) {
    return anim_veil_flying(hidden, pre_hidden);
}

uint64_t fio_veil_hand_slot_deferred(uint64_t veiled, uint64_t flying, uint64_t holdback) {
    return anim_veil_hand_slot_deferred(veiled, flying, holdback);
}

uint64_t fio_veil_fan(uint64_t veiled, uint64_t holdback) {
    return anim_veil_fan(veiled, holdback);
}

void fio_veil_grid(int sweeping, uint64_t veiled,
                   uint64_t swept_flown, uint64_t sweep_unplaced,
                   uint64_t sweep_arriving, uint64_t flying,
                   uint64_t *out_hidden, uint64_t *out_flying) {
    anim_veil_grid(sweeping, veiled, swept_flown, sweep_unplaced, sweep_arriving,
                   flying, out_hidden, out_flying);
}

uint64_t fio_veil_sweep_unplaced(uint64_t placed, uint64_t table) {
    return anim_veil_sweep_unplaced(placed, table);
}

void fio_veil_teardown(uint64_t opened, uint64_t orphaned, int is_newest,
                       uint64_t *out_reveal, uint64_t *out_carry) {
    anim_veil_teardown(opened, orphaned, is_newest, out_reveal, out_carry);
}

void fio_veil_handover(uint64_t standing, uint64_t placing,
                       uint64_t *out_reveal, uint64_t *out_veil) {
    anim_veil_handover(standing, placing, out_reveal, out_veil);
}

int fio_veil_unstarted_replay(int replay_pending, int n_events) {
    return anim_veil_unstarted_replay(replay_pending, n_events);
}

int fio_holdback_is_mine(int armed_at, int teardown_at) {
    return anim_holdback_is_mine(armed_at, teardown_at);
}

uint64_t fio_selection_after_tap(uint64_t selection, int card_id, uint64_t hand) {
    return anim_selection_after_tap(selection, card_id, hand);
}

int fio_is_placement(int event_type) { return anim_is_placement(event_type); }

int fio_is_my_placement(int event_type, int seat, int my_seat) {
    return anim_is_my_placement(event_type, seat, my_seat);
}

int fio_fan_cards(const uint8_t *hand, int n_hand, const uint8_t *held, int n_held,
                  char *out, int cap) {
    if (!out || cap < 0) return FIO_EBADARG;
    const int rc = anim_fan_cards(hand, n_hand, held, n_held, (unsigned char *)out, cap);
    if (rc == ANIM_ECAP) return FIO_ECAP;
    return rc < 0 ? FIO_EBADARG : rc;
}

int fio_laid_count(const uint8_t *hand, int n_hand, const uint8_t *held, int n_held,
                   uint64_t deferred) {
    const int rc = anim_laid_count(hand, n_hand, held, n_held, deferred);
    if (rc == ANIM_ECAP) return FIO_ECAP;
    return rc < 0 ? FIO_EBADARG : rc;
}

int fio_hand_laid_out(const uint8_t *cards, int n_cards, uint64_t deferred,
                      const uint8_t *order, int n_order, char *out, int cap) {
    if (!out || cap < 0) return FIO_EBADARG;
    const int rc = anim_hand_laid_out(cards, n_cards, deferred, order, n_order,
                                      (unsigned char *)out, cap);
    if (rc == ANIM_ECAP) return FIO_ECAP;
    return rc < 0 ? FIO_EBADARG : rc;
}

// ---------- a table, written -------------------------------------------------
//
// One cell of the one table layout, from the pair a host holds. See ios_api.h
// for the rule and for why the unnameable card is a byte of its own rather
// than "no card" or a refusal. The id arithmetic is card.h's, through a Card
// built only once the pair is known to be a deck card - the bitfields would
// narrow anything else silently, and a narrowed suit is some other card's id.
static unsigned char table_cell(int8_t suit, int8_t value, int is_cover) {
    if (suit >= 0 && suit < 4 && value >= 1 && value <= 13) {
        Card c;
        c.suit = suit;
        c.value = value;
        return (unsigned char)card_to_id(c);
    }
    if (is_cover && suit == FIO_CARD_NONE && value == FIO_CARD_NONE) return FIO_CONFLICT_NONE;
    return FIO_TABLE_UNKNOWN;
}

int fio_table_encode(const int8_t *pairs, int n_battles, char *out, int cap) {
    if (!out || cap < 0 || n_battles < 0) return FIO_EBADARG;
    if (n_battles > 0 && !pairs) return FIO_EBADARG;
    if (2 * n_battles > cap) return FIO_ECAP;
    unsigned char *q = (unsigned char *)out;
    for (int i = 0; i < n_battles; i++) {
        q[2 * i]     = table_cell(pairs[4 * i],     pairs[4 * i + 1], 0);
        q[2 * i + 1] = table_cell(pairs[4 * i + 2], pairs[4 * i + 3], 1);
    }
    return 2 * n_battles;
}

uint64_t fio_table_card_ids(const uint8_t *table, int n_battles) {
    return anim_table_card_ids(table, n_battles);
}

int fio_table_covers(const uint8_t *outer, int n_outer, const uint8_t *inner, int n_inner) {
    const int rc = anim_table_covers(outer, n_outer, inner, n_inner);
    return rc < 0 ? FIO_EBADARG : rc;
}

int fio_covered_sweep_accepts(int paired, const uint8_t *pre, int n_pre,
                              const uint8_t *cur, int n_cur) {
    const int rc = anim_covered_sweep_accepts(paired, pre, n_pre, cur, n_cur);
    return rc < 0 ? FIO_EBADARG : rc;
}

int fio_shown_table(int n_live, int n_sweep, int n_pending, int *out_sweeping) {
    return anim_shown_table(n_live, n_sweep, n_pending, out_sweeping);
}

int fio_shown_table_rows(const uint8_t *live, int n_live, const uint8_t *sweep, int n_sweep,
                         int n_pending, int hold_leaving, int *out_sweeping) {
    const int rc = anim_shown_table_rows(live, n_live, sweep, n_sweep, n_pending,
                                         hold_leaving, out_sweeping);
    return rc < ANIM_SHOWN_NONE ? FIO_EBADARG : rc;
}

int fio_pass_slot_shown(int previewing, int dragging, int seen_this_drag,
                        int over_dead_pair, int held_at, int n_battles, int rules) {
    return anim_pass_slot_shown(previewing, dragging, seen_this_drag, over_dead_pair,
                                held_at, n_battles, rules);
}

int fio_finish_rows(const uint8_t *elimination, int n_elim, int game_over,
                    int n_players, int my_seat, char *out, int cap) {
    if (!out) return FIO_EBADARG;
    AnimFinishRow rows[MAX_PLAYERS];
    const int n = anim_finish_rows(elimination, n_elim, game_over, n_players,
                                   my_seat, rows, MAX_PLAYERS);
    if (n == ANIM_ECAP) return FIO_ECAP;
    if (n < 0) return FIO_EBADARG;
    const int need = FIO_FINISH_HEAD + 3 * n;
    if (cap < need) return FIO_ECAP;
    unsigned char *q = (unsigned char *)out;
    int w = 0;
    q[w++] = FIO_FINISH_VERSION;
    q[w++] = (unsigned char)n;
    q[w++] = (unsigned char)n_players;
    for (int i = 0; i < n; i++) {
        q[w++] = (unsigned char)rows[i].place;
        q[w++] = (unsigned char)rows[i].seat;
        q[w++] = (unsigned char)rows[i].is_you;
    }
    return w;
}

int fio_shown_ledger_allows(int claim, int sequencing) {
    return anim_shown_ledger_allows(claim, sequencing);
}

int fio_badge_drops_as_cards_leave(int type) {
    return anim_badge_drops_as_cards_leave(type);
}

static int fio_roles_answer(int changed, const AnimRoles *r, int *out) {
    if (!changed) return 0;
    out[0] = r->defender;
    out[1] = r->first_attacker;
    out[2] = r->good_mask;
    return 1;
}

int fio_roles_goods_opening(int shown_defender, int shown_first_attacker,
                            int shown_good_mask, int first_good_mask, int *out) {
    if (!out) return FIO_EBADARG;
    const AnimRoles shown = { shown_defender, shown_first_attacker, shown_good_mask };
    AnimRoles r;
    return fio_roles_answer(anim_goods_opening(shown, first_good_mask, &r), &r, out);
}

int fio_roles_goods_cleared(int shown_defender, int shown_first_attacker,
                            int shown_good_mask, int step_good_mask, int *out) {
    if (!out) return FIO_EBADARG;
    const AnimRoles shown = { shown_defender, shown_first_attacker, shown_good_mask };
    AnimRoles r;
    return fio_roles_answer(anim_goods_cleared(shown, step_good_mask, &r), &r, out);
}

int fio_roles_pass_hand_off(int shown_defender, int shown_first_attacker,
                            int shown_good_mask, int attack_pass_seats,
                            int final_defender, int *out) {
    if (!out) return FIO_EBADARG;
    const AnimRoles shown = { shown_defender, shown_first_attacker, shown_good_mask };
    AnimRoles r;
    return fio_roles_answer(
        anim_pass_hand_off(shown, (unsigned)attack_pass_seats, final_defender, &r), &r, out);
}
