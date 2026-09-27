/* Tallybones - the exact single-player solver. tb_bot.h is the contract. */
#include "tb_bot.h"
#include "../../../shared/c/deal_rng.h"
#include <pthread.h>
#include <string.h>

static const uint32_t POW6[TB_DICE + 1] = { 1, 6, 36, 216, 1296, 7776 };

static int key_of(const int c[TB_FACES])
{
    int key = 0;
    for (int f = TB_FACES - 1; f >= 0; f--) key = key * 6 + c[f];
    return key;
}

/* ---- the combinatorics ---------------------------------------------------- */

/* Every multiset of 0..5 dice, in order of size, so that a keep's
 * sub-multisets (one die fewer) always have a lower index than it. */
static void build_keeps(TbBot *b)
{
    memset(b->key_keep, 0xFF, sizeof b->key_keep);
    int k = 0, h = 0;
    for (int size = 0; size <= TB_DICE; size++) {
        int c[TB_FACES];
        for (c[0] = 0; c[0] <= size; c[0]++)
        for (c[1] = 0; c[0] + c[1] <= size; c[1]++)
        for (c[2] = 0; c[0] + c[1] + c[2] <= size; c[2]++)
        for (c[3] = 0; c[0] + c[1] + c[2] + c[3] <= size; c[3]++)
        for (c[4] = 0; c[0] + c[1] + c[2] + c[3] + c[4] <= size; c[4]++) {
            c[5] = size - c[0] - c[1] - c[2] - c[3] - c[4];
            b->key_keep[key_of(c)] = (int16_t)k;
            b->keep_n[k] = (uint8_t)size;
            for (int f = 0; f < TB_FACES; f++) b->keep_c[k][f] = (uint8_t)c[f];
            b->keep_hand[k] = -1;
            for (int f = 0; f < TB_FACES; f++) {
                if (!c[f]) { b->keep_sub[k][f] = -1; continue; }
                c[f]--;
                b->keep_sub[k][f] = b->key_keep[key_of(c)];
                c[f]++;
            }
            if (size == TB_DICE) {
                int i = 0;
                for (int f = 0; f < TB_FACES; f++)
                    for (int j = 0; j < c[f]; j++) b->hand[h][i++] = (uint8_t)(f + 1);
                b->hand_keep[h] = (int16_t)k;
                b->keep_hand[k] = (int16_t)h;
                for (int cat = 0; cat < TB_CATS; cat++)
                    b->score[h][cat] = (int8_t)tb_score_of(b->hand[h], cat);
                h++;
            }
            k++;
        }
    }
}

/* For each keep with r dice to reroll: all 6^r raw outcomes, each one in
 * turn, collapsed by the hand they make. Integer counts over 6^r. */
static void build_outcomes(TbBot *b)
{
    int e = 0;
    for (int k = 0; k < TB_BOT_KEEPS; k++) {
        int r = TB_DICE - b->keep_n[k];
        uint32_t count[TB_BOT_HANDS] = { 0 };
        for (uint32_t raw = 0; raw < POW6[r]; raw++) {
            int c[TB_FACES];
            for (int f = 0; f < TB_FACES; f++) c[f] = b->keep_c[k][f];
            for (int i = 0, x = (int)raw; i < r; i++, x /= 6) c[x % 6]++;
            count[b->keep_hand[b->key_keep[key_of(c)]]]++;
        }
        b->out_at[k] = (uint16_t)e;
        for (int h = 0; h < TB_BOT_HANDS; h++)
            if (count[h]) {
                b->out_hand[e] = (uint8_t)h;
                b->out_count[e] = (uint16_t)count[h];
                e++;
            }
    }
    b->out_at[TB_BOT_KEEPS] = (uint16_t)e;
}

/* ---- the induction ------------------------------------------------------------ */

double tb_bot_best_cat(const TbBot *b, int rem, int upper, int hand, int *cat)
{
    double best = 0;
    int arg = -1;
    for (int c = 0; c < TB_CATS; c++) {
        if (!(rem >> c & 1)) continue;
        int s = b->score[hand][c], u = upper, bonus = 0;
        if (c < TB_UPPER_CATS) {
            u = upper + s;
            if (u >= TB_BONUS_AT) {
                if (upper < TB_BONUS_AT) bonus = TB_BONUS;
                u = TB_BONUS_AT;
            }
        }
        double v = s + bonus + b->ev[rem & ~(1 << c)][u];
        if (arg < 0 || v > best) { best = v; arg = c; }
    }
    if (cat) *cat = arg;
    return best;
}

void tb_bot_turn(const TbBot *b, int rem, int upper, TbBotTurn *t)
{
    if (upper > TB_BONUS_AT) upper = TB_BONUS_AT;
    if (upper < 0) upper = 0;
    t->rem = rem;
    t->upper = upper;
    for (int h = 0; h < TB_BOT_HANDS; h++) t->v[0][h] = tb_bot_best_cat(b, rem, upper, h, 0);
    memset(t->ek[0], 0, sizeof t->ek[0]);
    for (int r = 1; r < TB_ROLLS; r++) {
        const double *next = t->v[r - 1];
        double *ek = t->ek[r], best[TB_BOT_KEEPS];
        for (int k = 0; k < TB_BOT_KEEPS; k++) {
            if (b->keep_n[k] == TB_DICE) {
                ek[k] = t->v[0][b->keep_hand[k]];        /* keep all five = score now */
            } else {
                double acc = 0;
                for (int e = b->out_at[k]; e < b->out_at[k + 1]; e++)
                    acc += b->out_count[e] * next[b->out_hand[e]];
                ek[k] = acc / POW6[TB_DICE - b->keep_n[k]];
            }
            /* the best keep inside k: k itself, or the best inside k minus a die */
            best[k] = ek[k];
            for (int f = 0; f < TB_FACES; f++) {
                int s = b->keep_sub[k][f];
                if (s >= 0 && best[s] > best[k]) best[k] = best[s];
            }
        }
        for (int h = 0; h < TB_BOT_HANDS; h++) t->v[r][h] = best[b->hand_keep[h]];
    }
}

/* The first roll: every raw outcome of five dice, then the best play. */
static double turn_value(const TbBot *b, int rem, int upper, TbBotTurn *t)
{
    tb_bot_turn(b, rem, upper, t);
    double acc = 0;
    for (int e = b->out_at[0]; e < b->out_at[1]; e++)
        acc += b->out_count[e] * t->v[TB_ROLLS - 1][b->out_hand[e]];
    return acc / POW6[TB_DICE];
}

typedef struct {
    TbBot *b;
    const uint16_t *masks;
    int n, tid, threads;
} Level;

static void *solve_level(void *arg)
{
    Level *l = arg;
    TbBotTurn t;
    for (int i = l->tid; i < l->n; i += l->threads) {
        int rem = l->masks[i];
        for (int u = 0; u < TB_BOT_UPPER; u++) l->b->ev[rem][u] = turn_value(l->b, rem, u, &t);
    }
    return 0;
}

int tb_bot_build(TbBot *b, int threads)
{
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    build_keeps(b);
    build_outcomes(b);
    for (int u = 0; u < TB_BOT_UPPER; u++) b->ev[0][u] = 0;   /* a full card: nothing to come */
    /* in order of increasing number of remaining categories: every state
     * reads only states with one category fewer, all solved a level ago */
    static uint16_t masks[TB_BOT_MASKS];
    for (int level = 1; level <= TB_CATS; level++) {
        int n = 0;
        for (int m = 1; m < TB_BOT_MASKS; m++)
            if (__builtin_popcount((unsigned)m) == level) masks[n++] = (uint16_t)m;
        Level ls[64];
        pthread_t th[64];
        for (int i = 0; i < threads; i++) ls[i] = (Level){ b, masks, n, i, threads };
        for (int i = 1; i < threads; i++)
            if (pthread_create(&th[i], 0, solve_level, &ls[i])) return 0;
        solve_level(&ls[0]);
        for (int i = 1; i < threads; i++) pthread_join(th[i], 0);
    }
    return 1;
}

double tb_bot_value(const TbBot *b, int rem, int upper)
{
    if (upper > TB_BONUS_AT) upper = TB_BONUS_AT;
    if (upper < 0) upper = 0;
    return b->ev[rem & TB_FULL_CARD][upper];
}

/* ---- the policy ---------------------------------------------------------------- */

int tb_bot_hand(const TbBot *b, const uint8_t dice[TB_DICE])
{
    int c[TB_FACES] = { 0 };
    for (int i = 0; i < TB_DICE; i++) {
        if (dice[i] < 1 || dice[i] > TB_FACES) return -1;
        c[dice[i] - 1]++;
    }
    return b->keep_hand[b->key_keep[key_of(c)]];
}

int tb_bot_keep(const TbBot *b, const uint8_t dice[TB_DICE], int mask)
{
    int c[TB_FACES] = { 0 };
    for (int i = 0; i < TB_DICE; i++)
        if (mask >> i & 1) c[dice[i] - 1]++;
    return b->key_keep[key_of(c)];
}

TbBotChoice tb_bot_choose(const TbBot *b, const TbBotTurn *t, const uint8_t dice[TB_DICE], int rolls_left)
{
    TbBotChoice ch = { TB_M_SCORE, -1, 0 };
    int h = tb_bot_hand(b, dice);
    if (h < 0) return (TbBotChoice){ TB_M_NONE, -1, 0 };
    if (rolls_left >= TB_ROLLS) rolls_left = TB_ROLLS - 1;
    ch.ev = t->v[0][h];                                   /* stopping first on a tie */
    for (int mask = 0; rolls_left > 0 && mask < TB_ALL_KEPT; mask++) {
        double v = t->ek[rolls_left][tb_bot_keep(b, dice, mask)];
        if (v > ch.ev) { ch.ev = v; ch.kind = TB_M_KEEP; ch.arg = mask; }
    }
    if (ch.kind == TB_M_SCORE) tb_bot_best_cat(b, t->rem, t->upper, h, &ch.arg);
    return ch;
}

/* ---- the simulation -------------------------------------------------------------- */

int tb_bot_play(const TbBot *b, const uint8_t seed[32], uint64_t g, TbBotTurn *t)
{
    uint8_t s[32];
    memcpy(s, seed, 32);
    for (int i = 0; i < 8; i++) s[24 + i] = (uint8_t)(g >> (8 * i));
    DealRng r;
    deal_rng_seed(&r, s);
    int rem = TB_FULL_CARD, upper = 0, card[TB_CATS] = { 0 };
    for (int turn = 0; turn < TB_CATS; turn++) {
        tb_bot_turn(b, rem, upper, t);
        uint8_t dice[TB_DICE];
        for (int i = 0; i < TB_DICE; i++) dice[i] = (uint8_t)(1 + deal_rng_bounded(&r, TB_FACES));
        for (int left = TB_ROLLS - 1;; left--) {
            TbBotChoice ch = tb_bot_choose(b, t, dice, left);
            if (ch.kind == TB_M_KEEP) {
                for (int i = 0; i < TB_DICE; i++)
                    if (!(ch.arg >> i & 1)) dice[i] = (uint8_t)(1 + deal_rng_bounded(&r, TB_FACES));
                continue;
            }
            if (ch.kind != TB_M_SCORE || ch.arg < 0 || !(rem >> ch.arg & 1)) return -1;
            card[ch.arg] = tb_score_of(dice, ch.arg);
            rem &= ~(1 << ch.arg);
            upper += ch.arg < TB_UPPER_CATS ? card[ch.arg] : 0;
            break;
        }
    }
    int total = 0, up = 0;
    for (int c = 0; c < TB_CATS; c++) total += card[c];
    for (int c = 0; c < TB_UPPER_CATS; c++) up += card[c];
    return total + (up >= TB_BONUS_AT ? TB_BONUS : 0);
}

typedef struct {
    const TbBot *b;
    const uint8_t *seed;
    uint64_t n;
    int tid, threads, bad;
    TbBotSim sim;
} Sims;

static void *sim_worker(void *arg)
{
    Sims *w = arg;
    TbBotTurn t;
    for (uint64_t g = (uint64_t)w->tid; g < w->n; g += (uint64_t)w->threads) {
        int s = tb_bot_play(w->b, w->seed, g, &t);
        if (s < 0 || s >= TB_BOT_MAX_SCORE) { w->bad = 1; continue; }
        w->sim.n++;
        w->sim.sum += (uint64_t)s;
        w->sim.sumsq += (uint64_t)s * (uint64_t)s;
        w->sim.hist[s]++;
    }
    return 0;
}

int tb_bot_simulate(const TbBot *b, const uint8_t seed[32], uint64_t n, int threads, TbBotSim *out)
{
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    static Sims w[64];
    pthread_t th[64];
    for (int i = 0; i < threads; i++) {
        memset(&w[i], 0, sizeof w[i]);
        w[i].b = b; w[i].seed = seed; w[i].n = n; w[i].tid = i; w[i].threads = threads;
    }
    for (int i = 1; i < threads; i++)
        if (pthread_create(&th[i], 0, sim_worker, &w[i])) return 0;
    sim_worker(&w[0]);
    for (int i = 1; i < threads; i++) pthread_join(th[i], 0);
    memset(out, 0, sizeof *out);
    int bad = 0;
    for (int i = 0; i < threads; i++) {
        bad |= w[i].bad;
        out->n += w[i].sim.n;
        out->sum += w[i].sim.sum;
        out->sumsq += w[i].sim.sumsq;
        for (int s = 0; s < TB_BOT_MAX_SCORE; s++) out->hist[s] += w[i].sim.hist[s];
    }
    return !bad;
}
