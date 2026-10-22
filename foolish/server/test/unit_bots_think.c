// unit_bots_think.c - the dev-only bot think time (config.h CBASE_BOT_THINK_MS, bots.h
// cb_bots_think_ms): nothing at all when it is unset (production's path), refused without
// CBASE_DEV=1, and when set a wait counted from the move a viewer was last shown, within min..max,
// the same at every ask on one version, different across versions.
#include "t.h"

#include "app/bots.h"
#include "config.h"
#include "ctx.h"
#include "util/err.h"

#include <stdlib.h>
#include <string.h>

static void config_rules(void) {
    cb_config c;
    char err[256] = "";
    unsetenv("CBASE_DEV");
    unsetenv("CBASE_BOT_THINK_MS");
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.bot_think_min_ms, 0);
    T_EQ_INT(c.bot_think_max_ms, 0);
    // Without CBASE_DEV=1 the knob is refused, so production can never pace its bots by it.
    setenv("CBASE_BOT_THINK_MS", "1500-5000", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "CBASE_DEV=1") != NULL);
    setenv("CBASE_DEV", "1", 1);
    T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_OK);
    T_EQ_INT(c.bot_think_min_ms, 1500);
    T_EQ_INT(c.bot_think_max_ms, 5000);
    const char *bad[] = { "1500", "5000-1500", "-5", "0-10", "a-b", "10-", "1-600001" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        setenv("CBASE_BOT_THINK_MS", bad[i], 1);
        T_EQ_INT(cb_config_from_env(&c, err, sizeof err), CB_E_INVALID);
    }
    unsetenv("CBASE_BOT_THINK_MS");
    unsetenv("CBASE_DEV");
}

static void think_waits(void) {
    static cb_config c;
    memset(&c, 0, sizeof c);
    c.bot_speedup = 1;
    static cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &c;
    static Table t;
    memset(&t, 0, sizeof t);
    const int64_t shown = 1000000;
    t.clock.shown_ms = shown;
    // Unset: never a wait, whatever the board says.
    T_EQ_INT(cb_bots_think_ms(&ctx, &t, "abc123", 7, 0, shown), 0);
    T_EQ_INT(cb_bots_think_ms(&ctx, &t, "abc123", 7, 0, shown - 1000), 0);
    T_EQ_INT(cb_bots_think_ms(NULL, &t, "abc123", 7, 0, shown), 0);
    c.bot_think_min_ms = 1500;
    c.bot_think_max_ms = 5000;
    int distinct = 0;
    int64_t first = -1;
    for (int v = 1; v <= 64; v++) {
        int64_t w = cb_bots_think_ms(&ctx, &t, "abc123", v, 0, shown);
        T_CHECK(w >= 1500 && w <= 5000);
        // The same version asks again (after a sleep that was cut short): the same think.
        T_EQ_INT(cb_bots_think_ms(&ctx, &t, "abc123", v, 0, shown + 400), w - 400 > 0 ? w - 400 : 0);
        T_EQ_INT(cb_bots_think_ms(&ctx, &t, "abc123", v, 0, shown + w), 0);
        if (first < 0) first = w;
        else if (w != first) distinct++;
    }
    T_CHECK(distinct > 32);   // varied, not one fixed pace
    // Another game at the same version thinks differently (not a function of the version alone).
    int differ = 0;
    for (int v = 1; v <= 16; v++)
        if (cb_bots_think_ms(&ctx, &t, "abc123", v, 0, shown) != cb_bots_think_ms(&ctx, &t, "def456", v, 0, shown)) differ++;
    T_CHECK(differ > 8);
    // A board nobody was shown yet: no wait.
    t.clock.shown_ms = 0;
    T_EQ_INT(cb_bots_think_ms(&ctx, &t, "abc123", 7, 0, shown), 0);
}

int main(void) {
    T_RUN(config_rules);
    T_RUN(think_waits);
    return T_DONE();
}
