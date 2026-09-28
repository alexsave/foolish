// endgame_retro/alphabeta_probe.c (oracle/offline-only; not a shipped target).
// Replicate octogen's EXACT per-root-move endgame solve on one position: build
// the round boundary, generate the attacker's legal moves, and for each, apply
// it and solve the child from depth0=1 with a full window — exactly as
// octogen_strategy.c's verdict probe does. Report per-move value/abort/nodes;
// on a cyclic endgame every move aborts (why the oracle falls back to MC).
// #includes cordite_sim.c to reach the static move-gen + apply helpers.
#include "cordite_sim.c"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void make_state(SimState *s, uint64_t HA, uint64_t HD, int power) {
    memset(s, 0, sizeof(*s));
    s->num_players = 2; s->power_suit = (uint8_t)power;
    s->defender = 1; s->first_attacker = 0;
    s->status = GAME_STATUS_PLAYING;
    s->hand[0] = HA; s->hand[1] = HD;
    s->status_p[0] = PLAYER_STATUS_IN; s->status_p[1] = PLAYER_STATUS_IN;
    s->in_mask = 0x3u;
}
static uint64_t mask_of(const int *ids, int n){uint64_t m=0;for(int i=0;i<n;i++)m|=1ull<<ids[i];return m;}
static const char *SUIT="SHCD";
static const char *MT[]={"ATK","COV","PASS","PICKUP","GOOD","NONE"};
static void movelbl(char *o, const SolMove *m){
    int w=snprintf(o,64,"%s",MT[m->type]);
    for(int i=0;i<m->n;i++){int id=m->cards[i],su=id/13,v=id%13+1;
        w+=snprintf(o+w,64-w,"%s%d%c",i?",":":",v,SUIT[su]);}
}

int main(int argc, char **argv) {
    int A[] = {47,36,8,19,23,32,33,11,24};   // octogen (attacker), real ids
    int D[] = {34,22,21};                     // defender, real ids
    int power = 1;                            // trump = Hearts
    long per_move_budget = argc > 1 ? atol(argv[1]) : 200000000L;

    printf("MAX_DEPTH=(default 48)  per_move_budget=%ld\n", per_move_budget);
    ensure_masks();
    SimState root;
    make_state(&root, mask_of(A, sizeof A/sizeof*A), mask_of(D, sizeof D/sizeof*D), power);

    // Root value from attacker's perspective (me=0), like the standalone probe.
    cd_sim_solve_reset();
    int ab0 = 0; long rb = 5000000000L;
    int rv = cd_sim_solve_d(&root, 0, -2000, 2000, &rb, 0, &ab0);
    printf("ROOT solve (depth0=0): value=%d aborted=%d nodes=%ld\n\n",
           rv, ab0, 5000000000L - rb);

    // Generate attacker's (player 0) legal moves at the root round boundary.
    SolMove moves[CD_SIM_SOLVE_MAX_MOVES];
    int actor = 0;   // attacker to move at a fresh round boundary
    int nm = sim_gen_moves(&root, actor, moves, CD_SIM_SOLVE_MAX_MOVES);
    printf("attacker legal moves: %d\n", nm);

    int resolved = 0, aborted = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < nm; i++) {
        SimState child = root;
        sim_apply_sol(&child, actor, &moves[i]);
        cd_sim_solve_reset();
        int ab = 0; long b = per_move_budget;
        int v = cd_sim_solve_d(&child, /*me=*/0, -2000, 2000, &b, /*depth0=*/1, &ab);
        long nodes = per_move_budget - b;
        char lbl[64]; movelbl(lbl, &moves[i]);
        const char *verd = ab ? "UNKNOWN(abort)" : (v>0?"WIN":v<0?"LOSS":"DRAW");
        printf("  move %2d %-14s -> v=%5d %-14s nodes=%ld%s\n",
               i, lbl, v, verd, nodes, ab?"  <== ABORT":"");
        if (ab) aborted++; else resolved++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
    printf("\nresolved=%d aborted=%d  (%.2fs)\n", resolved, aborted, secs);
    return 0;
}
