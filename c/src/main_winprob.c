// cnitro_winprob: the win-probability strip's command line (winprob.h,
// docs/POST_GAME_ANALYSER.md § The win-probability strip).
//
//   cnitro_winprob --code=<replay code or foolish.cards link>
//                  [--engine=robusta] [--worlds=300] [--belief-worlds=24]
//                  [--threads=N] [--seed=1]
//                  [--bin=<file>] [--tsv=<file>] [--quiet]
//
// The measurement is winprob_packed (src/winprob.c) and its result is the
// packed bytes winprob.h describes. --bin writes those bytes; everything this
// prints - the summary and the --tsv dump - is read back out of them through
// winprob_read_header / winprob_read_step, so what it shows is what any other
// consumer of the bytes would see, and the text can never drift from the file.

#include "../src/bot_roster.h"
#include "../src/cli_util.h"
#include "../src/replay.h"
#include "../src/replay_extras.h"
#include "../src/winprob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SUIT = "SHCD";
static const char *VAL[13] = { "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A" };

static void card_str(char *b, size_t n, unsigned id, int trump) {
    if (id >= 52) { snprintf(b, n, "??"); return; }
    snprintf(b, n, "%s%c%s", VAL[id % 13], SUIT[id / 13], (int)(id / 13) == trump ? "*" : "");
}

static const char *kind_str(unsigned kind) {
    switch (kind) {
        case REPLAY_ATOM_ATTACK:    return "attack";
        case REPLAY_ATOM_COVER:     return "cover";
        case REPLAY_ATOM_PASS:      return "pass";
        case REPLAY_ATOM_PICKUP:    return "pickup";
        case REPLAY_ATOM_GOOD:      return "good";
        case REPLAY_ATOM_ROUND_END: return "round end";
        default:                    return "deal";
    }
}

// The move a step follows, as the label a reader would print.
static void move_str(char *out, size_t n, const WinprobStep *S, int trump) {
    snprintf(out, n, "%s", kind_str(S->kind));
    char cb[8], tb[8];
    for (int k = 0; k < S->n_cards && k < WINPROB_MOVE_CARDS; k++) {
        if (S->cards[k] == 0xFF) continue;
        card_str(cb, sizeof cb, S->cards[k], trump);
        size_t l = strlen(out);
        if (S->kind == REPLAY_ATOM_COVER && S->target != 0xFF) {
            card_str(tb, sizeof tb, S->target, trump);
            snprintf(out + l, n - l, " %s>%s", cb, tb);
        } else {
            snprintf(out + l, n - l, " %s", cb);
        }
    }
    if (S->n_cards > WINPROB_MOVE_CARDS) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, " +%d", S->n_cards - WINPROB_MOVE_CARDS);
    }
}

static void progress(void *ctx, int done, int total) {
    (void)ctx;
    fprintf(stderr, "  step %d/%d\r", done, total);
    fflush(stderr);
}

// The TSV dump, read back out of the packed bytes.
//
//   name  <seat> <utf-8>                 a seat's name, empty when the link carried none
//   out   <place> <seat>                 the order the seats went out
//   step  <i> <move> <seat> <deck> <label> <hand per seat, -1 = out>
//   wp    <i> truth|belief <seat> <p_fool x10000> <mean_fp x1000> <n>
static void dump_tsv(FILE *f, const unsigned char *buf, int len, const WinprobHeader *h) {
    int np = h->n_players;
    int trump = h->trump_suit;
    fprintf(f, "# cnitro_winprob wire v%d, replay code v%d\n", h->version, h->code_version);
    fprintf(f, "players\t%d\n", np);
    fprintf(f, "trump\t%c\n", SUIT[trump]);
    fprintf(f, "fool\t%d\n", h->fool == 0xFF ? -1 : h->fool);
    fprintf(f, "engine\t%s\n", bot_roster_at(h->roster_idx) ? bot_roster_at(h->roster_idx)->key : "?");
    fprintf(f, "worlds\t%d\t%d\n", h->worlds, h->belief_worlds);
    fprintf(f, "playouts\t%u\n", h->playouts);
    fprintf(f, "elapsed_ms\t%u\n", h->elapsed_ms);
    for (int s = 0; s < np; s++) fprintf(f, "name\t%d\t%s\n", s, h->name[s]);
    for (int i = 0; i < np; i++)
        if (h->elim[i] != 0xFF) fprintf(f, "out\t%d\t%d\n", i + 1, h->elim[i]);
    char label[96];
    for (int i = 0; i < h->n_steps; i++) {
        WinprobStep S;
        if (winprob_read_step(buf, len, h, i, &S) < 0) { fprintf(stderr, "step %d unreadable\n", i); return; }
        move_str(label, sizeof label, &S, trump);
        fprintf(f, "step\t%d\t%d\t%d\t%d\t%s", i,
                S.move == 0xFFFF ? -1 : S.move, S.seat == 0xFF ? -1 : S.seat, S.deck, label);
        for (int s = 0; s < np; s++) fprintf(f, "\t%d", S.hand[s] == 0xFF ? -1 : S.hand[s]);
        fprintf(f, "\n");
        for (int s = 0; s < np; s++) {
            if (S.truth_fool[s] != WINPROB_NONE)
                fprintf(f, "wp\t%d\ttruth\t%d\t%u\t%u\t%u\n", i, s,
                        S.truth_fool[s], S.truth_mean[s], S.n_truth);
            if (S.belief_fool[s] != WINPROB_NONE)
                fprintf(f, "wp\t%d\tbelief\t%d\t%u\t%u\t%u\n", i, s,
                        S.belief_fool[s], S.belief_mean[s], S.n_belief);
        }
    }
}

// One playout has exactly one fool, so a step's truth probabilities sum to 1,
// up to each seat's own rounding to four places (at most N/2 units of 1e-4, and
// none at all when the world count divides 10000). A sum that drifts further
// means the seats were scored from different playout sets - the one way this
// fold can be wrong without looking wrong.
static int truth_drift(const unsigned char *buf, int len, const WinprobHeader *h) {
    int worst = 0;
    for (int i = 0; i < h->n_steps; i++) {
        WinprobStep S;
        if (winprob_read_step(buf, len, h, i, &S) < 0) return worst;
        if (!S.n_truth) continue;
        int total = 0;
        for (int s = 0; s < h->n_players; s++)
            if (S.truth_fool[s] != WINPROB_NONE) total += S.truth_fool[s];
        if (abs(total - 10000) > abs(worst)) worst = total - 10000;
    }
    return worst;
}

static void usage(void) {
    fprintf(stderr,
        "usage: cnitro_winprob --code=<replay code or foolish.cards link>\n"
        "         [--engine=robusta] [--worlds=300] [--belief-worlds=24]\n"
        "         [--threads=N] [--seed=1] [--bin=<file>] [--tsv=<file>] [--quiet]\n");
}

int main(int argc, char **argv) {
    const char *url = get_arg(argc, argv, "code", 0);
    if (!url) { usage(); return 2; }

    WinprobParams p;
    winprob_params_default(&p);
    const char *eng = get_arg(argc, argv, "engine", "robusta");
    p.roster_idx = bot_roster_find(eng);
    if (!bot_roster_at(p.roster_idx)) { fprintf(stderr, "unknown engine '%s'\n", eng); return 2; }
    p.worlds        = parse_int(get_arg(argc, argv, "worlds", 0), p.worlds);
    p.belief_worlds = parse_int(get_arg(argc, argv, "belief-worlds", 0), p.belief_worlds);
    p.threads       = parse_int(get_arg(argc, argv, "threads", 0), 1);
    p.seed          = (uint32_t)parse_int(get_arg(argc, argv, "seed", 0), (int)p.seed);
    const char *bin = get_arg(argc, argv, "bin", 0);
    const char *tsv = get_arg(argc, argv, "tsv", 0);
    int quiet = has_flag(argc, argv, "quiet");

    // The moves half and the extras half of the link, split here: the codec's
    // own decoder stops at the dash, and the seat names live past it.
    static char code_s[REPLAY_MAX_INT_BYTES * 2];
    if (replay_link_parse(url, code_s, (int)sizeof code_s) <= 0) {
        fprintf(stderr, "not a replay link or code\n");
        return 2;
    }
    const char *dash = strchr(url, '-');
    p.names_b32 = (dash && dash[1]) ? dash + 1 : 0;

    static unsigned char code[REPLAY_MAX_INT_BYTES];
    int code_len = replay_b32_decode(code_s, code, (int)sizeof code);
    if (code_len <= 0) { fprintf(stderr, "bad code\n"); return 2; }

    static unsigned char out[1 << 21];
    int n = winprob_packed(code, code_len, &p, quiet ? 0 : progress, 0, out, (int)sizeof out);
    if (n < 0) {
        fprintf(stderr, "winprob failed: %d (replay error %d)\n", -n, winprob_last_replay_error());
        return 1;
    }
    if (bin) {
        FILE *f = fopen(bin, "wb");
        if (!f || fwrite(out, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot write %s\n", bin); return 1; }
        fclose(f);
    }

    WinprobHeader h;
    if (winprob_read_header(out, n, &h) < 0) { fprintf(stderr, "unreadable header\n"); return 1; }

    if (tsv) {
        FILE *f = fopen(tsv, "w");
        if (!f) { fprintf(stderr, "cannot write %s\n", tsv); return 1; }
        dump_tsv(f, out, n, &h);
        fclose(f);
    }
    if (quiet) { printf("%d bytes\n", n); return 0; }

    printf("\n%d seats, trump %c, replay code v%d, %d steps, %s at every seat\n",
           h.n_players, SUIT[h.trump_suit], h.code_version, h.n_steps,
           bot_roster_at(h.roster_idx) ? bot_roster_at(h.roster_idx)->key : "?");
    printf("%u playouts, %u ms, %d bytes (%d header + %d x %d)\n",
           h.playouts, h.elapsed_ms, n, h.header_bytes, h.n_steps, h.step_bytes);
    printf("truth check: worst step sums to %.4f (1 +- %d/10000 is right)\n",
           1.0 + truth_drift(out, n, &h) / 10000.0, h.n_players / 2);
    if (h.flags & WINPROB_F_BELIEF_FAIL)
        printf("WARNING: a seat's belief broke conservation somewhere; those seats carry no point\n");

    printf("\nfinish order:\n");
    for (int i = 0; i < h.n_players; i++)
        if (h.elim[i] != 0xFF)
            printf("  %d. seat %d %s\n", i + 1, h.elim[i], h.name[h.elim[i]]);
    if (h.fool != 0xFF)
        printf("  %d. seat %d %s  (the fool)\n", h.n_players, h.fool, h.name[h.fool]);

    // Each seat's truth line as a sparkline: the shape at a glance, and the
    // file for everything else.
    // A blank means "not measured"; a certain fool is a dot, not a hole.
    static const char *RAMP = ".:-=+*#%@";
    printf("\nchance of not being the fool, per seat, over the game:\n");
    for (int s = 0; s < h.n_players; s++) {
        printf("  %-18s ", h.name[s][0] ? h.name[s] : "-");
        for (int i = 0; i < h.n_steps; i++) {
            WinprobStep S;
            if (winprob_read_step(out, n, &h, i, &S) < 0) break;
            if (S.truth_fool[s] == WINPROB_NONE) { putchar(' '); continue; }
            int win = 10000 - S.truth_fool[s];
            putchar(RAMP[win * 8 / 10000]);
        }
        printf("\n");
    }
    return 0;
}
