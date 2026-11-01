// See bot_roster.h. The named bots are the rows of bots.c that have a `key`;
// this file is their view by key, by brain and by tier, and the one place a
// named bot is run with its knobs installed.

#include "bot_roster.h"

#include <string.h>

#include "bot_knobs.h"
#include "strategy.h"

// Force the flag caches to re-read their knobs. Each Monte-Carlo brain reads
// its knobs once and latches them (cd_flags_loaded / og_flags_loaded), which
// is why the knobs must be invalidated between two decisions that carry
// different specs.
void cordite_reload_flags(void);
void og_reload_flags(void);

// The roster in tier order. The registry is indexed by brain and its tiers are
// 1..n with no gap (tests.c holds that), so roster index i is the keyed row of
// tier i + 1, found by a pass over the ids: no second list, and no state to
// fill (bot drives run on many threads at once). A pass is 25 rows, against a
// decision that runs thousands of rollouts.
static int named(int s) {
    const BotDef *d = bot_def(s);
    return d && d->key;
}

int bot_roster_count(void) {
    int n = 0;
    for (int s = 0; s < STRAT_COUNT; s++) n += named(s);
    return n;
}

int bot_roster_strat(int idx) {
    if (idx < 0) return -1;
    for (int s = 0; s < STRAT_COUNT; s++)
        if (named(s) && bot_def(s)->tier == idx + 1) return s;
    return -1;
}

const BotDef *bot_roster_at(int idx) { return bot_def(bot_roster_strat(idx)); }

int bot_roster_find_by_strat(int strat) {
    return named(strat) ? bot_def(strat)->tier - 1 : -1;
}

int bot_roster_find(const char *key) {
    if (!key) return -1;
    for (int s = 0; s < STRAT_COUNT; s++) {
        if (!named(s)) continue;
        const char *a = bot_def(s)->key, *b = key;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return bot_roster_find_by_strat(s);
    }
    return -1;
}

// The offline projection is a filter over the roster, in tier order.
int bot_roster_offline_count(void) {
    int n = 0;
    for (int s = 0; s < STRAT_COUNT; s++) n += named(s) && bot_def(s)->offline;
    return n;
}

int bot_roster_offline_at(int n) {
    if (n < 0) return -1;
    const int count = bot_roster_count();
    for (int i = 0; i < count; i++) {
        const BotDef *e = bot_roster_at(i);
        if (e && e->offline && n-- == 0) return i;
    }
    return -1;
}

int bot_roster_table(BotRosterTable *out) {
    out->n = 0;
    const int n = bot_roster_count();
    if (n > BOT_ROSTER_MAX) return -1;
    for (int i = 0; i < n; i++) {
        const BotDef *e = bot_roster_at(i);
        if (!e) return -1;   // a tier gap: a refusal, never a shorter roster
        BotRosterRow *r = &out->rows[i];
        int klen = 0;
        while (e->key[klen]) klen++;
        if (klen > BOT_ROSTER_KEY_MAX) return -1;
        memset(r, 0, sizeof *r);
        r->strat = bot_roster_strat(i);
        r->linked = bot_roster_linked(i) != 0;
        r->uses_logs = e->uses_logs != 0;
        r->seeded = e->seeded != 0;
        r->offline = e->offline != 0;
        r->tier = e->tier;
        r->key_len = (uint8_t)klen;
        memcpy(r->key, e->key, (size_t)klen);
    }
    out->n = n;
    return n;
}

// The roster TABLE is universal: every host reads the same keys, knobs, flags
// and tiers. Which brains a given build can actually RUN is a build property
// (a row's `native_only`), and an unlinked brain returns -1, never a silent
// fallback to `random`. Read off the row, not the dispatch table, so the
// browser's module dumps the roster without linking a brain.
int bot_roster_linked(int idx) { return bot_linked(bot_roster_strat(idx)); }

int bot_roster_choose(int idx, const Game *g, int seat, const LegalMoves *moves) {
    const BotDef *e = bot_roster_at(idx);
    const StrategyFn choose = bot_linked(bot_roster_strat(idx)) ? bot_choose(bot_roster_strat(idx)) : NULL;
    if (!e || !choose || !moves || moves->n <= 0) return -1;

    bot_knobs_set(e->knobs);
    cordite_reload_flags();
    og_reload_flags();

    int chosen = choose(g, seat, moves, NULL);

    // Leave no knobs installed: a *_strategy_choose called directly afterwards
    // (the arena, a rollout policy) must see its own C defaults, not whatever
    // the last named bot happened to want.
    bot_knobs_clear();
    cordite_reload_flags();
    og_reload_flags();

    if (chosen < 0 || chosen >= moves->n) return -1;
    return chosen;
}

// ---- a bot's stored name (bot_roster.h) ---------------------------------------

int bot_nickname_write(int strat, int instance, char *out, int cap) {
    const BotDef *d = bot_def(strat);
    if (!d || !d->city || instance < 1 || !out) return -1;
    char digits[12];
    int nd = 0;
    for (int v = instance; v > 0; v /= 10) digits[nd++] = (char)('0' + v % 10);
    const int city = (int)strlen(d->city), n = 1 + city + 1 + nd;
    if (n > cap) return -1;
    out[0] = BOT_NICK_PREFIX;
    memcpy(out + 1, d->city, (size_t)city);
    out[1 + city] = ' ';
    for (int i = 0; i < nd; i++) out[2 + city + i] = digits[nd - 1 - i];
    return n;
}

static int shown(BotNickname *out, int kind, const char *s, int len) {
    out->kind = (int8_t)kind;
    if (len > BOT_NICK_SHOW_MAX) len = BOT_NICK_SHOW_MAX;   // a stored name is at most this long (roster.h)
    out->show_len = (uint8_t)len;
    memcpy(out->show, s, (size_t)len);
    return kind;
}

int bot_nickname_read(const char *name, int len, BotNickname *out) {
    memset(out, 0, sizeof *out);
    if (!name || len <= 0 || name[0] != BOT_NICK_PREFIX) return shown(out, BOT_NICK_HUMAN, name ? name : "", len > 0 ? len : 0);
    const char *bare = name + 1;
    const int blen = len - 1;
    int sp = blen - 1, instance = 0, place = 1;
    while (sp >= 0 && bare[sp] >= '0' && bare[sp] <= '9' && instance < 100000) { instance += (bare[sp] - '0') * place; place *= 10; sp--; }
    if (sp > 0 && sp < blen - 1 && bare[sp] == ' ' && instance >= 1) {
        for (int i = 0, n = bot_roster_count(); i < n; i++) {
            const BotDef *d = bot_roster_at(i);
            if (!d || !d->city || (int)strlen(d->city) != sp || memcmp(d->city, bare, (size_t)sp) != 0) continue;
            const int klen = (int)strlen(d->key);
            if (klen > BOT_ROSTER_KEY_MAX) break;
            out->kind = BOT_NICK_CITY;
            out->key_len = (uint8_t)klen;
            memcpy(out->key, d->key, (size_t)klen);
            out->instance = (int16_t)instance;
            return BOT_NICK_CITY;
        }
    }
    return shown(out, BOT_NICK_STORED, bare, blen);
}
