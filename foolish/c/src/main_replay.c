// Replay a single game with arbitrary strategies, printing every move and
// the relevant state. Used to investigate specific seeds (e.g. cases where
// random crushes espresso - what is random actually doing?).
//
// Or read a share link back: the decoded log stream, move by move, with the
// seat names and move gaps its extras carry.
//
// Usage:
//   cnitro_replay --seed=49107 --pairs=rand-esp
//   cnitro_replay --code="<replay link or code>" [--from=<first event>]

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/cli_util.h"
#include "../src/bot_play.h"
#include "../src/replay.h"
#include "../src/replay_steps.h"
#include "../src/replay_extras.h"
#include "../cardtext/card_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <strings.h>

// The face of a card is card_text.h's, the table datagen hands the web: a card
// value is a rank INDEX (1 is the two, ACE_VALUE the ace), not a face number.
static const char *suit_name(int s) {
    return s >= 0 && s < NUM_SUITS ? CARD_SUIT_FACE[s].glyph : "?";
}
static const char *value_name(int v) {
    return v >= 1 && v <= ACE_VALUE ? CARD_RANK_LABEL[v] : "?";
}
static void print_card(Card c, int trump) {
    printf("%s%s", value_name(c.value), suit_name(c.suit));
    if (c.suit == trump) printf("*");
}

static int strat_id(const char *s) {
    if (!strcmp(s,"esp")||!strcmp(s,"espresso")) return STRAT_ESPRESSO;
    if (!strcmp(s,"rand")||!strcmp(s,"random")) return STRAT_RANDOM;
    if (!strcmp(s,"hw")||!strcmp(s,"handwritten")) return STRAT_HANDWRITTEN;
    fprintf(stderr,"unknown strat '%s'\n",s); exit(2);
}

static int run_strategy(int strat, const Game *g, int p, const LegalMoves *moves) {
    if (strat == STRAT_RANDOM) return random_strategy_choose(g, p, moves, NULL);
    if (strat == STRAT_ESPRESSO) return espresso_strategy_choose(g, p, moves, NULL);
    if (strat == STRAT_HANDWRITTEN) return handwritten_strategy_choose(g, p, moves, NULL);
    return -1;
}

static void print_hand(const Player *p, int trump) {
    for (int i = 0; i < p->hand_count; i++) {
        if (i) printf(" ");
        print_card(p->hand[i], trump);
    }
}

static void describe_move(const LegalMove *m, int trump) {
    switch (m->type) {
        case MOVE_ATTACK:
            printf("ATTACK [");
            for (int i = 0; i < m->n_cards; i++) { if (i) printf(","); print_card(m->cards[i], trump); }
            printf("]");
            break;
        case MOVE_COVER:
            printf("COVER [");
            for (int i = 0; i < m->n_cards; i++) {
                if (i) printf(",");
                print_card(m->cards[i], trump); printf("→"); print_card(m->attack_cards[i], trump);
            }
            printf("]");
            break;
        case MOVE_PASS:
            printf("PASS [");
            for (int i = 0; i < m->n_cards; i++) { if (i) printf(","); print_card(m->cards[i], trump); }
            printf("]");
            break;
        case MOVE_PICKUP: printf("PICKUP"); break;
        case MOVE_GOOD:   printf("GOOD"); break;
        default: printf("?"); break;
    }
}

// ---------- reading a share link back ---------------------------------------

static const char *const LOG_NAME[] = {
    [LOG_GAME_START] = "game_start", [LOG_ATTACK] = "attack", [LOG_COVER] = "cover",
    [LOG_PASS] = "pass", [LOG_PICKUP] = "pickup", [LOG_GOOD] = "good",
    [LOG_DISCARD] = "discard", [LOG_DEFENDER_CHANGE] = "defender_change",
    [LOG_PLAYER_OUT] = "player_out", [LOG_DRAW] = "draw",
};

static int is_move(int type) {
    return type == LOG_ATTACK || type == LOG_COVER || type == LOG_PASS || type == LOG_PICKUP;
}

static void print_decoded_card(Card c, int trump) {
    if (c.suit < 0 || c.suit >= NUM_SUITS || c.value < 1 || c.value > ACE_VALUE) printf("??");
    else print_card(c, trump);
}

// A seat as the stream names it, padded to `width` CHARACTERS (code points, so a
// Cyrillic name lines up with a Latin one); 0 for no padding.
static void print_seat(int seat, const ReplayExtras *x, int width) {
    char buf[96];
    if (seat < 0) snprintf(buf, sizeof buf, "   --   ");
    else if (x && (x->flags & REPLAY_EXTRAS_FLAG_NAMES) && seat < x->n_names)
        snprintf(buf, sizeof buf, "%d:%.*s", seat, (int)x->names[seat].len, x->names[seat].text);
    else snprintf(buf, sizeof buf, "%d:seat%d", seat, seat);
    int chars = 0;
    for (const char *p = buf; *p; p++) chars += ((unsigned char)*p & 0xC0) != 0x80;
    printf("%s", buf);
    for (; chars < width; chars++) putchar(' ');
}

// The extras segment of a pasted link: the base32 after the dash that follows
// the moves code (replay_link_parse drops it), or 0 when there is none.
static int extras_segment(const char *link, const char *code, int code_len, char *out, int cap) {
    for (const char *p = link; *p; p++) {
        if (strncasecmp(p, code, (size_t)code_len) != 0 || p[code_len] != '-') continue;
        int n = 0;
        for (const char *q = p + code_len + 1; *q && n < cap - 1; q++) {
            const char ch = *q;
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '2' && ch <= '7'))) break;
            out[n++] = ch;
        }
        out[n] = 0;
        return n;
    }
    return 0;
}

static int decode_link(const char *link, int first) {
    static char code[2 * REPLAY_MAX_INT_BYTES], seg[16384];
    static unsigned char bytes[REPLAY_MAX_INT_BYTES], dec[1 << 20], blob[8192], answer[1 << 16];
    static ReplayExtras x;
    const int n = replay_link_parse(link, code, (int)sizeof code);
    if (n <= 0) { fprintf(stderr, "not a replay code: \"%s\"\n", link); return 2; }
    const int nb = b32_decode(code, bytes, (int)sizeof bytes);
    ReplaySummary s;
    int r = nb > 0 ? replay_summary_v6(bytes, nb, &s) : -REPLAY_ECAP;
    const int dn = r < 0 ? r : replay_decode(bytes, nb, dec, (int)sizeof dec);
    if (r < 0 || dn < 0) {
        fprintf(stderr, "replay decode: error %d (detail %d)\n", r < 0 ? r : dn, replay_last_error_detail());
        return 1;
    }

    const ReplayExtras *names = NULL;
    const int ns = extras_segment(link, code, n, seg, (int)sizeof seg);
    if (ns > 0) {
        const int bl = b32_decode(seg, blob, (int)sizeof blob);
        const int na = bl > 0 ? replay_extras_decode(blob, bl, s.num_players, s.moves, answer, (int)sizeof answer) : -1;
        if (na > 0 && replay_extras_unpack(answer, na, &x) == na) names = &x;
        else fprintf(stderr, "extras: unreadable (%d), printing without them\n", na);
    }

    int total = 0;
    ReplayDecodedLog l;
    for (int at = REPLAY_DEC_HDR; replay_decoded_log(dec, dn, &at, &l) > 0;) total++;

    printf("\n%d players \u00b7 trump ", s.num_players);
    print_decoded_card(s.trump, -1);
    printf(" (suit %d) \u00b7 first attacker seat %d \u00b7 format %d\n", s.power_suit, s.first_attacker, s.version);
    printf("names: ");
    if (names && (names->flags & REPLAY_EXTRAS_FLAG_NAMES))
        for (int i = 0; i < names->n_names; i++)
            printf("%s%d:%.*s", i ? "  " : "", i, (int)names->names[i].len, names->names[i].text);
    else printf("(none)");
    printf("\nelimination order (seats, first out first):");
    for (int i = 0; i < s.num_eliminated; i++) printf(" %d", s.elimination[i]);
    printf("\nFOOL (loser) = seat %d = ", s.fool);
    print_seat(s.fool, names, 0);
    printf("\ntotal events: %d\n\n", total);

    const int timed = names && (names->flags & REPLAY_EXTRAS_FLAG_TIMES);
    int i = 0, mv = 0, at = REPLAY_DEC_HDR, rc;
    while ((rc = replay_decoded_log(dec, dn, &at, &l)) > 0) {
        if (i++ >= first) {
            const char *type = l.log_type >= 0 && l.log_type <= LOG_DRAW ? LOG_NAME[l.log_type] : "?";
            printf("#%3d %-16s ", i, type);
            print_seat(l.seat, names, 14);
            printf(" ");
            for (int k = 0; k < l.n_pairs; k++) {
                if (k) printf(" ");
                print_decoded_card(l.primary[k], -1);
                if (l.target[k].suit >= 0) { printf("\u2192on\u2192"); print_decoded_card(l.target[k], -1); }
            }
            if (timed && is_move(l.log_type) && mv < names->n_gaps) printf(" (+%.1fs)", names->gaps[mv]);
            printf("\n");
        }
        if (is_move(l.log_type)) mv++;
    }
    if (rc < 0) { fprintf(stderr, "replay log stream: error %d\n", rc); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    const char *link = get_arg(argc, argv, "code", NULL);
    if (link) return decode_link(link, parse_int(get_arg(argc, argv, "from", NULL), 0));
    int seed = parse_int(get_arg(argc, argv, "seed", "1"), 1);
    const char *pairs = get_arg(argc, argv, "pairs", "rand-esp");
    int n_p = parse_int(get_arg(argc, argv, "players", "2"), 2);

    int strats[MAX_PLAYERS]; int n_strats = 0;
    char buf[256]; strncpy(buf, pairs, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, "-", &save); t && n_strats < MAX_PLAYERS;
         t = strtok_r(NULL, "-", &save)) {
        strats[n_strats++] = strat_id(t);
    }
    if (n_strats == 1) {
        for (int i = 1; i < n_p; i++) strats[i] = strats[0];
        n_strats = n_p;
    }
    if (n_strats != n_p) { fprintf(stderr,"need %d strats, got %d\n",n_p,n_strats); return 2; }

    game_set_seed((uint32_t)seed);
    random_strategy_set_seed((uint32_t)seed);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = (int8_t)n_p;
    for (int i = 0; i < n_p; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        g.players[i].strategy_key = (int8_t)strats[i];
    }
    start_game(&g);

    int trump = g.power_suit;
    printf("=== seed=%d  trump=%s ===\n", seed, suit_name(trump));
    printf("Initial deal:\n");
    for (int i = 0; i < n_p; i++) {
        printf("  p%d (%s): ", i,
               strats[i] == STRAT_RANDOM ? "rand" :
               strats[i] == STRAT_ESPRESSO ? "esp" :
               strats[i] == STRAT_HANDWRITTEN ? "hw" : "?");
        print_hand(&g.players[i], trump);
        printf("\n");
    }
    printf("Flipped: ");
    if (g.has_flipped) print_card(g.flipped, trump); else printf("-");
    printf("  first_attacker=p%d  defender=p%d\n\n", g.first_attacker, g.defender);

    int move_no = 0;
    BotPlay bp;
    bot_play_init(&bp, &g, 4000, BOT_PLAY_SHUFFLE, NULL);
    for (int p; (p = bot_play_next(&bp)) >= 0; ) {
        int idx = run_strategy(strats[p], &g, p, &bp.moves);
        if (idx < 0 || idx >= bp.moves.n) continue;
        const LegalMove *m = &bp.moves.moves[idx];

        move_no++;
        printf("[%d] p%d (%s) %s ", move_no, p,
               strats[p] == STRAT_RANDOM ? "rand" :
               strats[p] == STRAT_ESPRESSO ? "esp" :
               strats[p] == STRAT_HANDWRITTEN ? "hw" : "?",
               p == g.defender ? "DEF" : "ATK");
        describe_move(m, trump);
        printf("   (had %d legal)\n", bp.moves.n);

        bot_play_apply(&bp, idx);
    }

    int loser = game_done(&g);
    printf("\n=== Game over: ");
    if (loser >= 0) printf("p%d is the durak (held %d cards)", loser, g.players[loser].hand_count);
    else printf("no winner");
    printf(" — total moves: %d ===\n", move_no);
    if (loser >= 0) {
        printf("Final hands:\n");
        for (int i = 0; i < n_p; i++) {
            printf("  p%d: %d cards", i, g.players[i].hand_count);
            if (g.players[i].hand_count > 0) {
                printf("  ");
                print_hand(&g.players[i], trump);
            }
            printf("\n");
        }
    }
    return 0;
}
