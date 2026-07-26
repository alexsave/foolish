// POC workload generator: plays full games through the REAL kernel
// (game.c + legal.c, production rules) with a seeded random-move driver and
// dumps every k-th decision state in a portable little-endian format that the
// C and Rust benchmark harnesses both read. Also emits a corpus of FMSG
// envelopes via the real msg_encode for the wire benchmark.
//
// The dump is deliberately id-based (card ids 0..51) so neither harness
// depends on the other's in-memory struct layout.
#include "../c/src/game.h"
#include "../c/src/legal.h"
#include "../c/src/msg_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void w8(FILE *f, unsigned v)  { fputc((int)(v & 0xff), f); }
static void w16(FILE *f, unsigned v) { w8(f, v); w8(f, v >> 8); }
static void w32(FILE *f, unsigned v) { w16(f, v); w16(f, v >> 16); }

static int card_id_of(Card c) { return c.suit * 13 + (c.value - 1); }

static void dump_state(FILE *f, const Game *g, int actor) {
    w8(f, (unsigned)g->num_players);
    w8(f, (unsigned)g->power_suit);
    w8(f, (unsigned)g->defender);
    w8(f, (unsigned)g->first_attacker);
    w8(f, (unsigned)g->status);
    w8(f, (unsigned)g->num_battles);
    w8(f, (unsigned)actor);
    w8(f, (unsigned)g->has_flipped);
    w8(f, g->has_flipped ? (unsigned)card_id_of(g->flipped) : 0);
    w8(f, (unsigned)g->num_eliminated);
    w32(f, g->good_players_mask);
    w16(f, (unsigned)g->discard_pile_length);
    w16(f, (unsigned)g->deck_count);
    for (int i = 0; i < g->num_eliminated; i++) w8(f, (unsigned)g->elimination_order[i]);
    for (int i = 0; i < g->deck_count; i++) w8(f, (unsigned)card_id_of(g->deck[i]));
    for (int i = 0; i < g->num_battles; i++) {
        w8(f, (unsigned)card_id_of(g->table_battles[i].attack));
        if (card_is_none(g->table_battles[i].defense)) w8(f, 255);
        else w8(f, (unsigned)card_id_of(g->table_battles[i].defense));
    }
    for (int p = 0; p < g->num_players; p++) {
        const Player *pl = &g->players[p];
        w8(f, (unsigned)pl->status);
        w8(f, (unsigned)pl->hand_count);
        for (int j = 0; j < pl->hand_count; j++) w8(f, (unsigned)card_id_of(pl->hand[j]));
    }
}

static Game g_game;
static LegalMoves g_lm;

static int apply_move(Game *g, int actor, const LegalMove *m) {
    switch (m->type) {
        case MOVE_ATTACK: return handle_attack(g, actor, m->cards, m->n_cards);
        case MOVE_COVER:  return handle_cover(g, actor, m->cards, m->attack_cards, m->n_cards);
        case MOVE_PASS:   return handle_pass(g, actor, m->cards, m->n_cards);
        case MOVE_PICKUP: return handle_pickup(g, actor);
        case MOVE_GOOD:   return handle_good(g, actor);
        default:          return 0;
    }
}

int main(int argc, char **argv) {
    const char *states_path = argc > 1 ? argv[1] : "states.bin";
    const char *env_path    = argc > 2 ? argv[2] : "envelopes.bin";
    const int dump_every = 3;

    FILE *fs = fopen(states_path, "wb");
    if (!fs) { perror("states"); return 1; }
    w32(fs, 0x434F5046u); // "FPOC"
    w32(fs, 1);
    long count_pos = ftell(fs);
    w32(fs, 0); // patched later

    unsigned n_states = 0;
    int pcs[] = { 2, 3, 4, 5, 6, 8 };
    long decisions = 0;
    for (int pi = 0; pi < 6; pi++) {
        int np = pcs[pi];
        for (int game_i = 0; game_i < 40; game_i++) {
            uint32_t seed = (uint32_t)(1000003u * (unsigned)np + 7919u * (unsigned)game_i + 17u);
            game_set_seed(seed);
            memset(&g_game, 0, sizeof(g_game));
            game_seat_and_deal(&g_game, NULL, np);
            for (int ply = 0; ply < 4000; ply++) {
                if (game_done(&g_game) >= 0) break;
                int actor = -1;
                for (int s = 0; s < np; s++) {
                    calculate_legal_moves(&g_game, s, &g_lm);
                    if (g_lm.n > 0) { actor = s; break; }
                }
                if (actor < 0) break;
                if ((decisions++ % dump_every) == 0 && n_states < 20000) {
                    dump_state(fs, &g_game, actor);
                    n_states++;
                }
                int pick = (int)(game_random_u32() % (uint32_t)g_lm.n);
                if (!apply_move(&g_game, actor, &g_lm.moves[pick])) break;
                game_settle_status(&g_game);
            }
        }
    }
    long end = ftell(fs);
    fseek(fs, count_pos, SEEK_SET);
    w32(fs, n_states);
    fclose(fs);
    fprintf(stderr, "states: %u decisions dumped (of %ld played), %ld bytes\n",
            n_states, decisions, end);

    // ---- envelope corpus (real msg_encode output + some corrupted copies) ----
    FILE *fe = fopen(env_path, "wb");
    if (!fe) { perror("envelopes"); return 1; }
    w32(fe, 0x564E4546u); // "FENV"
    long ecount_pos = ftell(fe);
    w32(fe, 0);
    unsigned n_env = 0;
    game_set_seed(0xC0FFEEu);
    static unsigned char body[MSG_MAX_ACTION_BYTES];
    static unsigned char buf[8192];
    for (int i = 0; i < 2000; i++) {
        MsgEnvelope e;
        memset(&e, 0, sizeof(e));
        e.format = MSG_FORMAT_V6;
        e.flags = 0;
        e.n_players = (uint8_t)(2 + game_random_u32() % 7);
        e.variant = 0;
        e.game_id = ((uint64_t)game_random_u32() << 32) | game_random_u32();
        int n_joins = 1 + (int)(game_random_u32() % (uint32_t)e.n_players);
        e.n_joins = n_joins;
        uint32_t used = 0;
        for (int j = 0; j < n_joins; j++) {
            int seat;
            do { seat = (int)(game_random_u32() % (uint32_t)e.n_players); } while (used & (1u << seat));
            used |= 1u << seat;
            e.joins[j].seat = (uint8_t)seat;
            int nl = (int)(game_random_u32() % (MSG_MAX_NAME + 1));
            e.joins[j].name_len = (uint8_t)nl;
            for (int k = 0; k < nl; k++)
                e.joins[j].name[k] = (char)(0x20 + (game_random_u32() % 0x5f));
        }
        for (int k = 0; k < MSG_SEED_LEN; k++) e.seed[k] = (uint8_t)(game_random_u32() & 0xff);
        e.seed[0] |= 1; // never all-zero
        for (int k = 0; k < MSG_PARENT_LEN; k++) e.parent8[k] = (uint8_t)(game_random_u32() & 0xff);
        e.last_actor_seat = (uint8_t)(game_random_u32() % (uint32_t)e.n_players);
        if (i % 5 == 0) {
            e.phase = MSG_PHASE_WAITING;
            e.n_actions = 0; e.turn = 0; e.round = 0; e.actions_len = 0; e.actions = body;
        } else {
            e.phase = (i % 7 == 0) ? MSG_PHASE_FINISHED : MSG_PHASE_LIVE;
            e.n_actions = 1 + (int)(game_random_u32() % 300);
            e.turn = (uint16_t)e.n_actions;
            e.round = (uint8_t)(game_random_u32() % 40);
            e.actions_len = (int)(game_random_u32() % 512);
            for (int k = 0; k < e.actions_len; k++) body[k] = (unsigned char)(game_random_u32() & 0xff);
            e.actions = body;
        }
        int len = msg_encode(&e, buf, (int)sizeof(buf));
        if (len <= 0) { fprintf(stderr, "encode failed rc=%d at %d\n", len, i); return 1; }
        // 1 in 8: corrupt one byte so the decode error paths get exercised too.
        if (i % 8 == 3) buf[game_random_u32() % (uint32_t)len] ^= (unsigned char)(1 + (game_random_u32() % 255));
        w16(fe, (unsigned)len);
        fwrite(buf, 1, (size_t)len, fe);
        n_env++;
    }
    fseek(fe, ecount_pos, SEEK_SET);
    w32(fe, n_env);
    fclose(fe);
    fprintf(stderr, "envelopes: %u dumped\n", n_env);
    return 0;
}
