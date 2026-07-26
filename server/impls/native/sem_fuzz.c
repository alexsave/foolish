// sem_fuzz.c — the SEMANTIC anti-cheat fuzzer.
//
// fuzz_client.c proves the server is memory-safe against hostile input, but it
// throws bytes over a socket and has no ground truth: it cannot tell whether a
// move the server ACCEPTED was actually legal. The one thing an authenticated
// player must never be able to do is CHEAT — play a card they don't hold, move
// out of turn, conjure a card from nowhere. That is decided in exactly one
// place: awire_apply -> handle_* (game.c), the kernel legality engine every
// transport (native /action, iOS bridge, wasm) funnels through. If it is sound,
// no client can cheat regardless of transport; if it is not, TLS and tokens
// don't matter.
//
// So this harness links the kernel DIRECTLY (no server, no socket) and fuzzes
// awire_apply with FULL ground truth — it can see every hand, the deck and the
// trump. It deals real games, plays them to completion with bot_drive, and
// BETWEEN moves fires well-formed-but-illegal frames at awire_apply, asserting
// four invariants that together mean "you cannot cheat":
//
//   1. NO-PHANTOM-CARD (the core anti-cheat): a move naming a card that is not
//      in the acting seat's hand must be REJECTED. If awire_apply ever accepts
//      one, the card reached the table without being held — a cheat.
//   2. CONSERVATION: after any ACCEPTED move, the physical deck is still a
//      partition — no card appears twice across hands+deck+trump+table, and the
//      total (in play + discard) equals the dealt deck size (36 for 2-5p, 52
//      for 6-8p). Catches duplication and creation.
//   3. NO-MUTATION-ON-REJECT: a rejected move must leave the game byte-identical
//      (Game is POD, so memcpy-clone + memcmp is exact). The one documented
//      exception is the PASS_OVERFLOW abort (game.c handle_pass), which mutates
//      then ends the game on purpose.
//   4. LEGAL-MOVE-AGREEMENT: every move calculate_legal_moves() enumerates must
//      be ACCEPTED by awire_apply — the enumerator and the applier can't
//      disagree about what's legal.
//
// Build: `make sem_fuzz` (and `make sem_fuzz_asan` for the sanitized run).
//   usage: sem_fuzz [games] [seed]
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "card.h"
#include "game.h"
#include "legal.h"
#include "awire.h"
#include "bot_drive.h"

// ---- counters ----
static long g_games = 0, g_moves = 0, g_probes = 0, g_accepted = 0, g_rejected = 0;
// findings (any nonzero => the engine let something through it must not have)
static long f_cheat = 0, f_conserve = 0, f_mutation = 0, f_legal_rejected = 0, f_badcard = 0;
static int  g_reports = 0;   // cap the verbose dump

#define REPORT(...) do { if (g_reports < 40) { fprintf(stderr, __VA_ARGS__); g_reports++; } } while (0)

static uint32_t g_rng = 0x12345678u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int rint_(int n) { return n <= 0 ? 0 : (int)(rnd() % (unsigned)n); }

static int deck_size_of(const Game *g) { return g->num_players >= 6 ? 52 : 36; }

// A card id (0..51) is a REAL card for this deck iff its value is in range
// (5..13 for the 36-card deck, 1..13 for the 52-card deck).
static bool id_valid_for(const Game *g, int id) {
    if (id < 0 || id > 51) return false;
    return card_of_id(id).value >= min_value_for(g->num_players);
}

static bool hand_has(const Player *p, Card c) {
    for (int i = 0; i < p->hand_count; i++) if (card_eq(p->hand[i], c)) return true;
    return false;
}

// ---- Invariant 2: card conservation over the WHOLE board ----
// Returns true if consistent; on failure fills `why` and returns false.
static bool conservation_ok(const Game *g, char *why, int whycap) {
    int8_t seen[52];
    memset(seen, 0, sizeof seen);
    int concrete = 0;
    #define TALLY(card) do { \
        Card _c = (card); \
        if (card_is_none(_c)) break; \
        int _id = card_to_id(_c); \
        if (_id < 0 || _id > 51) { snprintf(why, whycap, "invalid card suit=%d val=%d", _c.suit, _c.value); return false; } \
        seen[_id]++; concrete++; \
    } while (0)

    for (int p = 0; p < g->num_players; p++) {
        const Player *pl = &g->players[p];
        if (pl->hand_count < 0 || pl->hand_count > MAX_HAND_SIZE) { snprintf(why, whycap, "hand_count=%d seat=%d", pl->hand_count, p); return false; }
        for (int i = 0; i < pl->hand_count; i++) TALLY(pl->hand[i]);
    }
    if (g->deck_count < 0 || g->deck_count > MAX_DECK) { snprintf(why, whycap, "deck_count=%d", g->deck_count); return false; }
    for (int i = 0; i < g->deck_count; i++) TALLY(g->deck[i]);
    if (g->has_flipped) TALLY(g->flipped);
    if (g->num_battles < 0 || g->num_battles > MAX_BATTLES) { snprintf(why, whycap, "num_battles=%d", g->num_battles); return false; }
    for (int i = 0; i < g->num_battles; i++) { TALLY(g->table_battles[i].attack); TALLY(g->table_battles[i].defense); }
    #undef TALLY

    for (int id = 0; id < 52; id++)
        if (seen[id] > 1) { snprintf(why, whycap, "card id=%d appears %d times (duplicated)", id, seen[id]); return false; }

    int total = concrete + g->discard_pile_length;
    if (total != deck_size_of(g)) {
        snprintf(why, whycap, "card count %d (in-play %d + discard %d) != deck %d",
                 total, concrete, g->discard_pile_length, deck_size_of(g));
        return false;
    }
    return true;
}

// Apply a well-formed action to a CLONE and check invariants 1-3 against it.
// `pre` is the pre-move game (ground truth for hands + no-mutation baseline).
static void probe_apply(const Game *pre, int seat, const AwireAction *a) {
    Game c;
    memcpy(&c, pre, sizeof c);                 // Game is POD: exact clone
    bool ok = awire_apply(&c, seat, a);
    g_probes++;

    if (ok) {
        g_accepted++;
        // Inv 1 (no-phantom-card): for ATTACK/COVER/PASS every `cards[]` entry
        // must have been in this seat's PRE-move hand. (COVER's attacks[] name
        // table cards, not hand cards, so they're not checked here.)
        if (a->kind == AWIRE_ATTACK || a->kind == AWIRE_COVER || a->kind == AWIRE_PASS) {
            const Player *pl = &pre->players[seat];
            for (int i = 0; i < a->n; i++) {
                if (!hand_has(pl, a->cards[i])) {
                    f_cheat++;
                    REPORT("CHEAT: seat %d played kind=%d card suit=%d val=%d NOT in hand — accepted!\n",
                           seat, a->kind, a->cards[i].suit, a->cards[i].value);
                    break;
                }
            }
        }
        // Inv 2 (conservation) on the resulting state.
        char why[96];
        if (!conservation_ok(&c, why, sizeof why)) {
            f_conserve++;
            REPORT("CONSERVE: accepted kind=%d seat=%d broke the deck: %s\n", a->kind, seat, why);
        }
    } else {
        g_rejected++;
        // Inv 3 (no mutation on reject) — the documented PASS_OVERFLOW abort is
        // allowed to have mutated + ended the game.
        if (memcmp(&c, pre, sizeof c) != 0 && engine_last_reject != ENGINE_REJECT_PASS_OVERFLOW) {
            f_mutation++;
            REPORT("MUTATION: rejected kind=%d seat=%d (reason=%d) changed game state\n",
                   a->kind, seat, engine_last_reject);
        }
    }
}

// Build a well-formed random frame (valid kind/n/length; content may be hostile)
// and stress the acting seat with it, plus targeted not-in-hand probes.
static void run_probes(const Game *g, int seat) {
    // A) targeted NO-PHANTOM probes: name a card the seat provably does NOT hold.
    const Player *pl = &g->players[seat];
    for (int t = 0; t < 3; t++) {
        int id = rint_(52);
        // find a valid-for-deck id NOT in this seat's hand
        for (int tries = 0; tries < 64 && (!id_valid_for(g, id) || hand_has(pl, card_of_id(id))); tries++)
            id = rint_(52);
        if (!id_valid_for(g, id) || hand_has(pl, card_of_id(id))) continue;
        Card ghost = card_of_id(id);
        AwireAction a; memset(&a, 0, sizeof a);
        int which = rint_(3);
        if (which == 0)      { a.kind = AWIRE_ATTACK; a.n = 1; a.cards[0] = ghost; }
        else if (which == 1) { a.kind = AWIRE_PASS;   a.n = 1; a.cards[0] = ghost; }
        else {                 a.kind = AWIRE_COVER;  a.n = 1; a.cards[0] = ghost;
                               // cover a real uncovered attack if one exists
                               a.attacks[0] = ghost;
                               for (int b = 0; b < g->num_battles; b++)
                                   if (card_is_none(g->table_battles[b].defense)) { a.attacks[0] = g->table_battles[b].attack; break; }
        }
        probe_apply(g, seat, &a);
    }

    // B) random well-formed frames: valid kind + n<=AWIRE_MAX_CARDS, cards are
    //    random real ids for this deck (so some may, by luck, be legal).
    for (int k = 0; k < 4; k++) {
        AwireAction a; memset(&a, 0, sizeof a);
        a.kind = rint_(5);                                   // ATTACK..GOOD
        int maxn = (a.kind == AWIRE_PICKUP || a.kind == AWIRE_GOOD) ? 0 : (rint_(2) ? 1 + rint_(6) : rint_(AWIRE_MAX_CARDS + 1));
        a.n = maxn > AWIRE_MAX_CARDS ? AWIRE_MAX_CARDS : maxn;
        for (int i = 0; i < a.n; i++) {
            int id = rint_(52); for (int tr = 0; tr < 16 && !id_valid_for(g, id); tr++) id = rint_(52);
            a.cards[i]   = card_of_id(id & 63 ? id : 0);
            int aid = rint_(52); for (int tr = 0; tr < 16 && !id_valid_for(g, aid); tr++) aid = rint_(52);
            a.attacks[i] = card_of_id(aid);
        }
        // occasionally aim at a seat that is NOT the current actor (out-of-turn)
        int s = rint_(4) ? seat : rint_(g->num_players);
        probe_apply(g, s, &a);
    }

    // C) role probes with n=0: PICKUP / GOOD from this seat (must reject unless
    //    the seat truly is in the right role/phase — handled by conservation +
    //    no-mutation, not asserted as always-reject).
    for (int kind = AWIRE_PICKUP; kind <= AWIRE_GOOD; kind++) {
        AwireAction a; memset(&a, 0, sizeof a); a.kind = kind; a.n = 0;
        probe_apply(g, seat, &a);
    }
}

// Inv 4: every enumerated legal move must be accepted by awire_apply.
static void check_legal_agreement(const Game *g, int seat) {
    static _Thread_local LegalMoves lm;   // ~big; keep off the stack
    calculate_legal_moves(g, seat, &lm);
    for (int i = 0; i < lm.n; i++) {
        const LegalMove *m = &lm.moves[i];
        if (m->type == MOVE_WAIT) continue;             // no awire equivalent
        AwireAction a; memset(&a, 0, sizeof a);
        a.kind = m->type;                                // MOVE_* == AWIRE_* for 0..4
        a.n = m->n_cards;
        if (a.n > AWIRE_MAX_CARDS) continue;             // enumerator's wider cap
        for (int c = 0; c < a.n; c++) { a.cards[c] = m->cards[c]; a.attacks[c] = m->attack_cards[c]; }
        Game clone; memcpy(&clone, g, sizeof clone);
        if (!awire_apply(&clone, seat, &a)) {
            f_legal_rejected++;
            REPORT("LEGAL-REJECTED: enumerated move #%d kind=%d n=%d seat=%d rejected by awire_apply (reason=%d)\n",
                   i, a.kind, a.n, seat, engine_last_reject);
        }
    }
}

static void play_one_game(int np) {
    Game g;
    memset(&g, 0, sizeof g);
    g.num_players = (int8_t)np;
    // A wide, reproducible deal seed (full 52!/36! deal space); bytes vary per game.
    uint8_t seed[FOOLISH_SEED_LEN];
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) seed[i] = (uint8_t)rnd();
    game_set_deal_seed_bytes(seed, FOOLISH_SEED_LEN);
    game_seat_and_deal(&g, NULL, np);    // NULL -> keep memset strategy_key 0 = STRAT_RANDOM (all bots)
    if (g.status != GAME_STATUS_PLAYING) return;

    char why[96];
    if (!conservation_ok(&g, why, sizeof why)) { f_conserve++; REPORT("CONSERVE(deal): %s\n", why); return; }

    for (int step = 0; step < 4000; step++) {
        // adversarial + agreement probes on a couple of seats
        int a0 = rint_(np), a1 = rint_(np);
        run_probes(&g, a0);
        check_legal_agreement(&g, a0);
        if (a1 != a0) check_legal_agreement(&g, a1);

        // real state must stay conserved
        if (!conservation_ok(&g, why, sizeof why)) { f_conserve++; REPORT("CONSERVE(mid): %s\n", why); break; }

        // advance one drive cycle (all seats are random bots)
        BotDriveOut out;
        int r = bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, NULL, 0, &out);
        if (r < 0) break;
        g_moves += out.n;
        game_settle_status(&g);
        if (out.stop == BOT_STOP_ENDED || out.stop == BOT_STOP_NO_ELIGIBLE) break;
        if (g.status != GAME_STATUS_PLAYING) break;
    }
    g_games++;
}

int main(int argc, char **argv) {
    long games = argc > 1 ? atol(argv[1]) : 3000;
    if (argc > 2) g_rng = (uint32_t)strtoul(argv[2], NULL, 0) | 1u;

    for (long i = 0; i < games; i++) {
        int np = 2 + rint_(7);   // 2..8
        play_one_game(np);
    }

    long findings = f_cheat + f_conserve + f_mutation + f_legal_rejected + f_badcard;
    printf("sem_fuzz done: games=%ld drive_moves=%ld probes=%ld (accepted=%ld rejected=%ld)\n",
           g_games, g_moves, g_probes, g_accepted, g_rejected);
    printf("  findings: cheat=%ld conservation=%ld mutation_on_reject=%ld legal_move_rejected=%ld bad_card=%ld\n",
           f_cheat, f_conserve, f_mutation, f_legal_rejected, f_badcard);
    if (findings == 0) {
        printf("  PASS: no anti-cheat / conservation / consistency violations\n");
        return 0;
    }
    printf("  FAIL: %ld violation(s) — see stderr\n", findings);
    return 1;
}
