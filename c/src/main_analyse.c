// cnitro_analyse: the post-game analyser's command line (docs/POST_GAME_ANALYSER.md).
//
//   cnitro_analyse --code=<base32 v6 code> [--seat=N] [--engine=robusta]
//                  [--worlds=24] [--futures=4] [--exhaustive=512]
//                  [--candidates=0] [--solve=200000] [--seed=1]
//                  [--deep-engine=octogen] [--deep-nodes=3] [--deep-worlds=64]
//                  [--threads=0] [--raw=<file>] [--quiet]
//
// The analysis itself is analyse_packed (src/analyse.c) and its result is the
// packed bytes analyse.h describes; this file is a presentation layer over the
// READER, so what it prints is what any other consumer of the bytes would see.
// --raw writes the bytes themselves.

#include "../src/analyse.h"
#include "../src/bot_roster.h"
#include "../src/cli_util.h"
#include "../src/replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SUIT = "SHCD";
// card.h values 1..13: value 13 is the ace and the 36-card deck is 5..13, so a
// value reads one pip higher (5 = six, 9 = ten, 10 = jack).
static const char *VAL[14] = { "?", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A" };

static void card_str(char *b, size_t n, int id, int trump) {
    if (id >= 52) { snprintf(b, n, "??"); return; }
    snprintf(b, n, "%s%c%s", VAL[id % 13 + 1], SUIT[id / 13], (id / 13 == trump) ? "*" : "");
}

static void move_str(char *out, size_t n, const AnalyseCand *c, int trump) {
    char cb[8], tb[8];
    out[0] = 0;
    switch (c->type) {
        case MOVE_PICKUP: snprintf(out, n, "pickup"); return;
        case MOVE_GOOD:   snprintf(out, n, "good"); return;
        case MOVE_ATTACK: snprintf(out, n, "attack"); break;
        case MOVE_COVER:  snprintf(out, n, "cover"); break;
        case MOVE_PASS:   snprintf(out, n, "pass"); break;
        default:          snprintf(out, n, "move%d", c->type); break;
    }
    for (int k = 0; k < c->n_cards; k++) {
        card_str(cb, sizeof cb, c->cards[k], trump);
        size_t l = strlen(out);
        if (c->type == MOVE_COVER) {
            card_str(tb, sizeof tb, c->targets[k], trump);
            snprintf(out + l, n - l, " %s>%s", cb, tb);
        } else {
            snprintf(out + l, n - l, " %s", cb);
        }
    }
}

static const char *verdict_str(int v) {
    switch (v) {
        case ANALYSE_V_FORCED:   return "forced";
        case ANALYSE_V_BEST:     return "best";
        case ANALYSE_V_DECLINED: return "declined";
        case ANALYSE_V_CHANCE:   return "MISTAKE";
        case ANALYSE_V_DECISIVE: return "DECISIVE";
        case ANALYSE_V_LOST:     return "lost";
        default:                 return "?";
    }
}

static const char *roster_key(int idx) {
    const BotRosterEntry *e = bot_roster_at(idx);
    return e ? e->key : "?";
}

static int engine_arg(const char *s, int def) {
    if (!s || !*s) return def;
    int idx = bot_roster_find(s);
    if (idx < 0) { fprintf(stderr, "unknown engine '%s'\n", s); exit(2); }
    return idx;
}

static void usage(void) {
    fprintf(stderr,
        "usage: cnitro_analyse --code=<base32 v6 code> [--seat=N] [--engine=robusta]\n"
        "         [--worlds=24] [--futures=4] [--exhaustive=512] [--candidates=0]\n"
        "         [--solve=200000] [--seed=1] [--deep-engine=octogen] [--deep-nodes=3]\n"
        "         [--deep-worlds=64] [--threads=0] [--raw=<file>] [--quiet]\n");
}

int main(int argc, char **argv) {
    const char *code_s = get_arg(argc, argv, "code", 0);
    if (!code_s) { usage(); return 2; }

    AnalyseParams p;
    analyse_params_default(&p);
    p.seat = parse_int(get_arg(argc, argv, "seat", 0), -1);
    p.roster_idx = engine_arg(get_arg(argc, argv, "engine", 0), p.roster_idx);
    p.worlds = parse_int(get_arg(argc, argv, "worlds", 0), p.worlds);
    p.futures = parse_int(get_arg(argc, argv, "futures", 0), p.futures);
    p.exhaustive_cap = parse_int(get_arg(argc, argv, "exhaustive", 0), p.exhaustive_cap);
    p.max_candidates = parse_int(get_arg(argc, argv, "candidates", 0), p.max_candidates);
    p.solve_budget = parse_int(get_arg(argc, argv, "solve", 0), (int)p.solve_budget);
    p.seed = (uint32_t)parse_int(get_arg(argc, argv, "seed", 0), (int)p.seed);
    p.deep_roster_idx = engine_arg(get_arg(argc, argv, "deep-engine", 0), -1);
    p.deep_nodes = parse_int(get_arg(argc, argv, "deep-nodes", 0), p.deep_nodes);
    p.deep_worlds = parse_int(get_arg(argc, argv, "deep-worlds", 0), p.deep_worlds);
    p.threads = parse_int(get_arg(argc, argv, "threads", 0), 0);
    const char *raw = get_arg(argc, argv, "raw", 0);
    int quiet = get_arg(argc, argv, "quiet", 0) != 0;

    static unsigned char code[REPLAY_MAX_INT_BYTES];
    int code_len = replay_b32_decode(code_s, code, (int)sizeof code);
    if (code_len <= 0) { fprintf(stderr, "bad code\n"); return 2; }

    static unsigned char out[1 << 20];
    int n = analyse_packed(code, code_len, &p, out, (int)sizeof out);
    if (n < 0) {
        fprintf(stderr, "analyse failed: %d (replay error %d)\n", n, analyse_last_replay_error());
        return 1;
    }
    if (raw) {
        FILE *f = fopen(raw, "wb");
        if (!f || fwrite(out, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot write %s\n", raw); return 1; }
        fclose(f);
    }
    if (quiet) { printf("%d bytes\n", n); return 0; }

    AnalyseHeader h;
    int q = analyse_read_header(out, n, &h);
    if (q < 0) { fprintf(stderr, "unreadable header (%d)\n", q); return 1; }
    int trump = h.trump_suit;

    printf("post-game analysis: %d players, trump %c, fool %s, engine %s",
           h.n_players, SUIT[trump], h.fool == 0xFF ? "-" : (char[]){ (char)('0' + h.fool), 0 },
           roster_key(h.roster_idx));
    if (h.deep_roster_idx != 0xFF) printf(", deep %s", roster_key(h.deep_roster_idx));
    printf("\ncost: %u playouts, %u exact solves, %u ms\n", h.n_playouts, h.n_solves, h.elapsed_ms);
    if (h.flags & ANALYSE_HF_BELIEF_FAIL) printf("WARNING: the belief model failed conservation at some node; those nodes carry no verdict\n");

    printf("\ndeal:\n");
    for (int s = 0; s < h.n_players; s++) {
        printf("  seat %d: %d trump%s in the opening hand (P=%.1f%%, P(at most)=%.1f%%), %d trumps entered the hand all game\n",
               s, h.deal[s].opening_trumps, h.deal[s].opening_trumps == 1 ? "" : "s",
               h.deal[s].p_exact / 100.0, h.deal[s].p_at_most / 100.0, h.deal[s].trumps_seen);
    }

    printf("\ndecisions:\n");
    static AnalyseNode node;
    char mv[256], bv[256];
    for (int i = 0; i < h.n_nodes; i++) {
        int r = analyse_read_node(out + q, n - q, &node);
        if (r < 0) { fprintf(stderr, "unreadable node %d (%d)\n", i, r); return 1; }
        q += r;
        move_str(mv, sizeof mv, &node.cands[node.played], trump);
        printf("  #%-3d step %-3d seat %d  %-28s", i, node.step, node.seat, mv);
        if (node.verdict == ANALYSE_V_FORCED) {
            printf("  forced%s\n", (node.flags & ANALYSE_NF_BELIEF_FAIL) ? " (belief FAILED)" : "");
            continue;
        }
        printf("  win %5.1f%%  %-8s", node.win_prob / 100.0, verdict_str(node.verdict));
        int best = (node.flags & ANALYSE_NF_DEEP) ? node.deep_best : node.best;
        int loss = (node.flags & ANALYSE_NF_DEEP) ? node.deep_loss : node.loss;
        int se   = (node.flags & ANALYSE_NF_DEEP) ? node.deep_loss_se : node.loss_se;
        if (best != node.played) {
            move_str(bv, sizeof bv, &node.cands[best], trump);
            printf("  better: %-24s  +%.3f", bv, loss / 1000.0);
            if (se) printf(" +-%.3f", 1.96 * se / 1000.0);
        }
        printf("  [%d worlds%s%s%s%s%s%s]\n", node.n_worlds,
               (node.flags & ANALYSE_NF_EXHAUSTIVE) ? ", every hand" : "",
               (node.flags & ANALYSE_NF_FUTURES) ? ", deck order sampled" : "",
               (node.flags & ANALYSE_NF_PROOF) ? ", PROOF" : "",
               (node.flags & ANALYSE_NF_CAPPED) ? ", candidates capped" : "",
               (node.flags & ANALYSE_NF_DEEP) ? ((node.flags & ANALYSE_NF_DEEP_AGREES) ? ", deep agrees" : ", deep DISAGREES") : "",
               (node.flags & ANALYSE_NF_DEEP) ? "" : "");
        if (node.verdict == ANALYSE_V_CHANCE || node.verdict == ANALYSE_V_DECISIVE
            || node.verdict == ANALYSE_V_DECLINED || (node.flags & ANALYSE_NF_PROOF)) {
            for (int c = 0; c < node.n_cands; c++) {
                const AnalyseCand *cd = &node.cands[c];
                move_str(mv, sizeof mv, cd, trump);
                int use_deep = (node.flags & ANALYSE_NF_DEEP) != 0;
                int nn = use_deep ? cd->deep_n : cd->n;
                int nf = use_deep ? cd->deep_n_fool : cd->n_fool;
                int mean = use_deep ? cd->deep_mean_fp : cd->mean_fp;
                printf("        %c %-28s fool %3d/%-3d  mean %.3f",
                       c == node.played ? '>' : (c == best ? '*' : ' '), mv, nf, nn, mean / 1000.0);
                if (cd->proof == ANALYSE_P_WIN) printf("  proven WIN");
                else if (cd->proof == ANALYSE_P_LOSS) printf("  proven LOSS");
                else if (cd->proof == ANALYSE_P_MIXED) printf("  proven: wins %d of %d worlds", cd->proven_wins, cd->proven_wins + cd->proven_losses);
                printf("\n");
            }
        }
    }
    if (h.decisive_node != 0xFFFF) printf("\ndecisive moment: decision #%d\n", h.decisive_node);
    return 0;
}
