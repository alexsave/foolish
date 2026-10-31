// bots.c - THE BOT REGISTRY: every brain and every named bot, one row each.
//
// ---- adding a bot ---------------------------------------------------------
//
// 1. Write src/<name>_strategy.c with `int <name>_strategy_choose(const Game *,
//    int, const LegalMoves *, void *)`, and its prototype in strategy.h. The
//    Makefile compiles every src/*_strategy.c (STRATEGY_SRC); nothing else
//    lists it.
// 2. Give it the next id in strategy.h (STRAT_<NAME> = STRAT_COUNT, then bump
//    STRAT_COUNT; ids are never compacted or reused, see strategy.h).
// 3. Add its row below, at [STRAT_<NAME>], and its function at the same index
//    of one dispatch table under it (NAMED for a bot with a key, RESEARCH for
//    a brain without one). That is the whole registration:
//    strategy_choose, parse_strategy, the roster (bot_roster.h), the table the
//    web and the phone read (BotRosterTable), the server's bot accounts
//    (foolish/server/src/store/bot_seed.c) and the phone's ladder (generated
//    into sdk/swift/gen/phone by tools/structgen/gen.sh through datagen) all
//    read this table, and no switch or second list exists anywhere.
//    - A research brain needs `name` and `alias`, and `native_only = 1` with
//      its dispatch entry wrapped in NATIVE_ONLY (the browser's modules link
//      only what a named bot there dispatches).
//    - A NAMED bot (one a host can seat) also needs `key`, its knobs and
//      flags, and `tier`: the next one up, since the roster is tiers 1..n.
//    - A SEEDED bot (dealt on the website) also needs `seeded = 1`, a `city`
//      and its distance `km`, BOT_IDS_MAX freshly minted v4 `ids` (never
//      derived), and a `bot.<key>` string in foolish/c/i18n (keys.h and all
//      twenty-five strings_<code>.c) for the phone's picker.
//    - A seeded row's comment, on the line above its `city`, carries that
//      CITY's song. The song belongs to the city, not the brain: when a city
//      moves to another bot, its song moves with it. A comment on purpose, so
//      it ships no bytes; name the piece, never quote its lyrics.
//
// A host never restates any of this: the web reads BotRosterTable
// (wasm_bot_roster_dump), the phone reads the same table through EngineC and
// its city ladder from the generated module, and the server derives its bot
// rows from `ids` and `city`.
//
// ---- the table ------------------------------------------------------------
//
// Indexed by the STRAT_* id itself, so a row is BOTS[id], its brain NAMED[id]
// or RESEARCH[id],
// and the arrays' length IS the id space (STRAT_COUNT); no switch exists. A
// retired id is an empty slot in both: no name parses to it and nothing
// dispatches it.
//
// THE ROSTER ORDER IS `tier`, not the id: ids are history (handwritten_prod
// is 16, newer than octogen's ancestors), tiers are strength. bot_roster.c
// walks rows with a `key` by tier, so a roster index is tier - 1.
//
// The rows are literals because datagen reads them: shared/tools/datagen
// --columns takes the fields a host needs (gen.sh says which) and never
// evaluates the rest, which is how the `ids` array can sit in a row.
#include "bots.h"

#include <stddef.h>

// The browser's modules (WASM_BOT_CFLAGS, -DFOOLISH_SEEDED_BOTS_ONLY: web.wasm,
// oracle.wasm, oracle-mt.wasm and the e2e table_host) link only the brains a
// named bot there can dispatch, which are the rostered ones minus the
// offline-only espresso and gunpowder and the server-only fusion. Every other
// brain is `native_only` in its row and NATIVE_ONLY (NULL) in the dispatch
// table, so their objects can stay out of the link (the Makefile's
// WASM_BOT_SRC) and bot_roster_linked says 0 rather than a seat silently
// playing another bot. Linking espresso_prod and gunpowder there
// measured +5.1 KB gzip on every cold start for rungs nothing seeds; fusion's
// rollout noise is compiled out of web.wasm and the table host altogether
// (cordite_sim.h CD_ROLLOUT_NOISE), so its function does not exist there.
//
// The one exception is the Infinite Oracle's two modules (FOOLISH_ORACLE_BUILD):
// no seat plays there either, but the Oracle analyses every position with
// fusion (docs/INFINITE_ORACLE_DESIGN.md "The brain"), so they compile its noise
// in and link its function (ORACLE_LINKED), and bot_linked says so. Fusion's
// row stays native_only: that is what web.wasm and the table host read.
//
// No browser module seeds a store either, so the server accounts' ids are
// left out of them too (SEED_IDS): 56 UUIDs every visitor would download and
// nothing there reads.
#ifdef FOOLISH_SEEDED_BOTS_ONLY
#define NATIVE_ONLY(fn) NULL
#define SEED_IDS(...) { NULL }
#else
#define NATIVE_ONLY(fn) fn
#define SEED_IDS(...) { __VA_ARGS__ }
#endif
// Native, and in the Oracle's modules: cordite_sim.h CD_ROLLOUT_NOISE's condition.
#if !defined(FOOLISH_SEEDED_BOTS_ONLY) || defined(FOOLISH_ORACLE_BUILD)
#define ORACLE_LINKED(fn) fn
#else
#define ORACLE_LINKED(fn) NULL
#endif

// The octogen trump-conservation tie-break, in milli-units of mean-finish
// taxed per trump led while the deck is alive (octogen_strategy.c
// OG_TRUMP_KEEP). '40' = on; '0' reverts to the pre-fix behavior. This value
// moved here from bot_strategy.ts, where it was an edge-deployable constant:
// changing it is a kernel rebuild, deliberately, because it is a property of
// the bot named "octogen", not of the server that happens to run it.
#define OCTOGEN_KNOBS "OG_TRUMP_KEEP=40"

// CD_RACE stops a deliberation once the leading candidate is statistically
// separated (validated strength-neutral at C=75: pc4x800 identical, pc2/pc6
// within noise; landslide decisions finish in ~50 worlds instead of ~900).
// CD_BUDGET=prod is the deployed player-count-aware world budget.
#define CORDITE_KNOBS "CD_BUDGET=prod,CD_RACE=1,CD_RACE_C=75"

// Notes on the named bots, each of which was a live bug once:
//
//  - SEEDED IS NARROWER THAN OFFLINE, and simple_heuristic is why. A seeded bot
//    is dealt onto the website, where a seat shows the rung's city and nothing
//    else, so a rung with no city cannot be seeded: it would put "Simple
//    Heuristic 2" on a board between Miami and Madrid. It kept its brain, its
//    tier and the offline picker, and lost only the site.
//
//  - `handwritten` is STRAT_HANDWRITTEN_PROD and `espresso` STRAT_ESPRESSO_PROD,
//    the exact mirrors of the production TS bots. The un-suffixed
//    STRAT_HANDWRITTEN / STRAT_ESPRESSO are the arena/rollout variants, which
//    drifted from the production bots and stay frozen because cordite's
//    rollout policy is tuned against them. ios_api.c once pointed the
//    player-facing rungs at the arena variants, so offline Handwritten was not
//    the site's Handwritten. That is why a row has both a `name` (the brain's)
//    and a `key` (the bot's).
//
//  - there is no `cordite_max` / `octogen_max`. octogen_max was an alias of
//    octogen (identical knobs). cordite_max was CD_BUDGET=max, a FLAT
//    120/240/168 world budget that only beats the player-count-aware `prod`
//    schedule at 2-4 players and is roughly HALF of it at 6-8
//    (cordite_strategy.c cd_worlds), so "Max" was weaker than plain Cordite in
//    the larger games. A key maps to one brain and a brain to at most one key
//    (bot_roster_find_by_strat), so a knob-only tier cannot come back.
static const BotDef BOTS[STRAT_COUNT] = {
    [STRAT_RANDOM] = {
        .name = "random", .alias = "rand",
        .key = "random", .knobs = "", .uses_logs = 0, .seeded = 1, .offline = 1, .tier = 1,
        // Miami's song: Kygo, "Stole the Show".
        .city = "Miami", .km = 9600,
        .ids = SEED_IDS("176d94a4-71f2-4542-9510-5603e52058d7", "15b54f52-2240-4955-8055-e27b7d767e3a",
                        "fb86b312-0a92-456a-9d86-dbcf139c5c0c", "cb71817c-55c4-4588-9e50-48f5f14c1fe0",
                        "250cca6c-6e42-49c1-9ddb-f0a892d1bdc8", "127fe4f9-c6ae-4574-a138-6d672398c0b3",
                        "958fe3ae-f1d3-4009-ba13-5497c241c373"),
    },
    // The arena/rollout espresso and handwritten (see the note above): brains,
    // not bots.
    [STRAT_ESPRESSO] = { .name = "espresso", .alias = "esp", .native_only = 1 },
    [STRAT_HANDWRITTEN] = { .name = "handwritten", .alias = "hw", .native_only = 1 },
    [STRAT_ROBUSTA] = {
        .name = "robusta", .alias = "rob",
        .key = "robusta", .knobs = "", .uses_logs = 1, .seeded = 1, .offline = 1, .tier = 5,
        // Seoul's song: LE SSERAFIM, "Eve, Psyche & The Bluebeard's wife".
        .city = "Seoul", .km = 6600,
        .ids = SEED_IDS("7ef1fc0b-3615-4dc8-b3ef-aa8f5d54d974", "8bc76702-5436-4765-9276-06b3cf48c28a",
                        "2fb4b0e0-042f-4e8c-a686-0fe57683a53f", "31e33723-01db-4ed7-91a5-25c7aacadb34",
                        "4b562969-4424-4806-ae2b-211635fd01a0", "d94da892-95c2-4c4a-aa40-b1835293c564",
                        "c423710d-afe3-4ec8-bc7f-6dbd4fb891b8"),
    },
    [STRAT_FIRECRACKER] = {
        .name = "firecracker", .alias = "fc",
        .key = "firecracker", .knobs = "", .uses_logs = 1, .seeded = 1, .offline = 1, .tier = 6,
        // Madrid's song: Celia Cruz, "La Vida Es un Carnaval".
        .city = "Madrid", .km = 3440,
        .ids = SEED_IDS("8dab2d5c-c44d-4b59-837b-8e412c2ae942", "323fe1d3-7911-473d-a510-acae44946c0e",
                        "7e4ded9f-0f9e-4a19-8246-c42202365971", "34d13cb1-a638-43c8-b7b9-142736ef3dfc",
                        "10e6bc02-3e6f-4ee5-8388-fcedfaf6c08f", "4c61fbcc-ddd8-4188-9b46-74ade99681d3",
                        "7e178642-b839-4a4f-91db-bf6aae3513cb"),
    },
    // Offline only: no city, so not on the site.
    [STRAT_GUNPOWDER] = {
        .name = "gunpowder", .alias = "gp", .native_only = 1,
        .key = "gunpowder", .knobs = "", .uses_logs = 1, .seeded = 0, .offline = 1, .tier = 7,
    },
    [STRAT_BLACKPOWDER] = {
        .name = "blackpowder", .alias = "bp",
        .key = "blackpowder", .knobs = "", .uses_logs = 1, .seeded = 1, .offline = 1, .tier = 8,
        // Vienna's song: Mozart, Requiem, "Lacrimosa".
        .city = "Vienna", .km = 1660,
        .ids = SEED_IDS("492b48b0-f474-4661-8545-b269ca614325", "a541a928-8152-441c-8fb9-ff224bbf7dc7",
                        "4201eb28-61ad-40d5-885b-4950111489a0", "29107dcb-fb7b-4088-bf44-b41f7fa9b84f",
                        "ea14d04e-ebcc-4f73-81de-3ccd86530d4f", "c634657e-cb48-4ef6-800a-f76f2250eb43",
                        "6e3e3e5d-5466-4d84-8767-6e4d9d5394aa"),
    },
    [STRAT_CORDITE] = {
        .name = "cordite", .alias = "cd",
        .key = "cordite", .knobs = CORDITE_KNOBS, .uses_logs = 1, .seeded = 1, .offline = 1, .tier = 9,
        // Warsaw's song: Chopin, Fantaisie-Impromptu (Op. 66).
        .city = "Warsaw", .km = 1151,
        .ids = SEED_IDS("56959dab-9d74-438b-b0e7-2f08d4524c81", "30532f60-cd98-48bb-8044-e2f8393d5702",
                        "88cc6a4a-400f-40ed-917f-20ec9eb5e16f", "4d1c6b02-a5f7-41e6-b57a-f5aada3db922",
                        "b06f300a-2f9c-4d99-bb50-77580466e4ab", "518a9f06-e7bf-4688-a75e-ce603711ed83",
                        "bc195cef-fac6-45fd-a518-c7d25b42e35e"),
    },
    [STRAT_ASTROLITE] = { .name = "astrolite", .alias = "as", .native_only = 1 },
    // cordite with the pre-change (1x) budget; research.
    [STRAT_CORDITE_OLD] = { .name = "cordite_old", .alias = "cd0", .native_only = 1 },
    [STRAT_SIMPLE_HEURISTIC] = {
        .name = "simple_heuristic", .alias = "sh",
        .key = "simple_heuristic", .knobs = "", .uses_logs = 0, .seeded = 0, .offline = 1, .tier = 2,
    },
    [STRAT_CHAMPION] = { .name = "champion", .alias = "ch", .native_only = 1 },
    [STRAT_ULTIMATE_CHAMPION] = {
        .name = "ultimate_champion", .alias = "uc", .native_only = 1,
    },
    [STRAT_HACKER] = { .name = "hacker", .alias = "hk", .native_only = 1 },
    [STRAT_FULMINATE] = { .name = "fulminate", .alias = "fm", .native_only = 1 },
    // Offline only: no city, so not on the site.
    [STRAT_ESPRESSO_PROD] = {
        .name = "espresso_prod", .alias = "ep", .native_only = 1,
        .key = "espresso", .knobs = "", .uses_logs = 1, .seeded = 0, .offline = 1, .tier = 4,
    },
    [STRAT_HANDWRITTEN_PROD] = {
        .name = "handwritten_prod", .alias = "hp",
        .key = "handwritten", .knobs = "", .uses_logs = 0, .seeded = 1, .offline = 1, .tier = 3,
        // New York's song: Robin S, "Show Me Love".
        .city = "New York", .km = 7500,
        .ids = SEED_IDS("663f2cbf-e8f7-407c-a991-192a1a929a17", "a3791b0c-9fe3-4881-846e-d9a36b5ffec5",
                        "7615f43f-f937-43df-95bc-5ba7537e5efc", "44d3db6c-7ea2-4573-ab6d-59d512a43e00",
                        "71afbbc7-8210-43ee-87f2-a553e2850659", "aed56295-c1af-42a7-957d-e93bc1c0e8db",
                        "5b4bd18e-4c3b-4ffe-b3d7-0190fa756052"),
    },
    [STRAT_DISTILLED] = { .name = "distilled", .alias = "dl", .native_only = 1 },
    // 18 (semtex) and 19 (semtex_oracle) are retired.
    [STRAT_OCTOGEN] = {
        .name = "octogen", .alias = "og",
        .key = "octogen", .knobs = OCTOGEN_KNOBS, .uses_logs = 1, .seeded = 1, .offline = 1, .tier = 10,
        // St. Petersburg's song: Prokofiev, "Dance of the Knights".
        .city = "St. Petersburg", .km = 635,
        .ids = SEED_IDS("4f6f2ebb-1d3e-4761-a5a9-c91313eba087", "f60b53ec-74f6-45fe-9e0f-315e5495bc18",
                        "45f7a264-aedc-4ee4-8473-4681d6e0bcb8", "acc775ae-6d35-41d2-88bc-7dc1599cff82",
                        "df16e0de-4569-4771-804e-98f8d13affd1", "fc185071-a522-424e-a1bd-0c4d34b32d3e",
                        "abe1fcfa-992c-4bf5-b163-32c0b0a21a7b"),
    },
    [STRAT_OCTOGEN_ORACLE] = {
        .name = "octogen_oracle", .alias = "ogo", .native_only = 1,
    },
    // 22 (torpex) and 23 (novichok) are retired.
    //
    // The top rung (c/FUSION.md): octogen's brain and knobs, whose pick a
    // second race with 30% rollout noise may overrule when it is two standard
    // errors better, at every table size; fusion_strategy_choose sets it itself.
    // Seeded, so the server deals it on the website as Moscow; not offline, so
    // the phone's picker does not offer it. native_only: the server links the
    // kernel natively and is the only place a seated fusion plays (web.wasm
    // runs no seat's brain and compiles its noise out; the Oracle's modules
    // link it to analyse with, not to seat, see ORACLE_LINKED above).
    [STRAT_FUSION] = {
        .name = "fusion", .alias = "fu", .native_only = 1,
        .key = "fusion", .knobs = OCTOGEN_KNOBS, .uses_logs = 1, .seeded = 1, .offline = 0, .tier = 11,
        // Moscow's song: the National Anthem of the Soviet Union.
        .city = "Moscow", .km = 0,
        .ids = SEED_IDS("2c58c4a2-a254-4a75-a465-db25fce8de79", "a721ac2c-afc4-41d7-87ff-285cef86f1fd",
                        "592588e9-ba8f-44b9-be56-2803377705bf", "98385eea-2b92-44f7-a436-11c20337eec3",
                        "03f1f377-00a7-4936-b7ea-ed4f2c93b206", "6db6e1cc-fa6c-4e9f-a2e9-9bba574e63d5",
                        "efe6f751-912f-48be-a488-5aee56b85bdb"),
    },
};

// THE DISPATCH, beside the rows and apart from them on purpose, in two tables
// indexed like the rows. A function pointer is a link-time reference: the
// browser's web.wasm reading one row pulled 5.7 KB of the brains' data into
// every visitor's download, and the phone's bot library (linked per object,
// dead-stripped) took every research brain once the roster's dispatch reached
// them, +62 KB of code and +335 KB of statics (both measured on this change).
// So the rows hold data only; NAMED holds the brains a named bot runs, which is
// all bot_roster_choose reaches (bot_choose); RESEARCH holds the rest, which
// only the arena's strategy_choose reaches. Each brain is in exactly one of
// them, NAMED exactly when its row has a key (tests.c).
//
// NATIVE_ONLY here and `native_only` in the row say the same thing, and tests.c
// compiles this file as the browser's modules do to hold the two together.
// Every research brain is native-only, so RESEARCH is empty there.
static const StrategyFn NAMED[STRAT_COUNT] = {
    [STRAT_RANDOM]           = random_strategy_choose,
    [STRAT_SIMPLE_HEURISTIC] = simple_heuristic_strategy_choose,
    [STRAT_HANDWRITTEN_PROD] = handwritten_prod_strategy_choose,
    [STRAT_ESPRESSO_PROD]    = NATIVE_ONLY(espresso_prod_strategy_choose),
    [STRAT_ROBUSTA]          = robusta_strategy_choose,
    [STRAT_FIRECRACKER]      = firecracker_strategy_choose,
    [STRAT_GUNPOWDER]        = NATIVE_ONLY(gunpowder_strategy_choose),
    [STRAT_BLACKPOWDER]      = blackpowder_strategy_choose,
    [STRAT_CORDITE]          = cordite_strategy_choose,
    [STRAT_OCTOGEN]          = octogen_strategy_choose,
    [STRAT_FUSION]           = ORACLE_LINKED(fusion_strategy_choose),
};
static const StrategyFn RESEARCH[STRAT_COUNT] = {
    [STRAT_ESPRESSO]          = NATIVE_ONLY(espresso_strategy_choose),
    [STRAT_HANDWRITTEN]       = NATIVE_ONLY(handwritten_strategy_choose),
    [STRAT_ASTROLITE]         = NATIVE_ONLY(astrolite_strategy_choose),
    [STRAT_CORDITE_OLD]       = NATIVE_ONLY(cordite_old_strategy_choose),
    [STRAT_CHAMPION]          = NATIVE_ONLY(champion_strategy_choose),
    [STRAT_ULTIMATE_CHAMPION] = NATIVE_ONLY(ultimate_champion_strategy_choose),
    [STRAT_HACKER]            = NATIVE_ONLY(hacker_strategy_choose),
    [STRAT_FULMINATE]         = NATIVE_ONLY(fulminate_strategy_choose),
    [STRAT_DISTILLED]         = NATIVE_ONLY(distilled_strategy_choose),
    [STRAT_OCTOGEN_ORACLE]    = NATIVE_ONLY(octogen_oracle_strategy_choose),
};

const BotDef *bot_def(int strat) {
    if (strat < 0 || strat >= STRAT_COUNT || !BOTS[strat].name) return NULL;
    return &BOTS[strat];
}

int strategy_retired(int strat) { return strat >= 0 && strat < STRAT_COUNT && !BOTS[strat].name; }

StrategyFn bot_choose(int strat) { return bot_def(strat) ? NAMED[strat] : NULL; }

int bot_linked(int strat) {
    const BotDef *d = bot_def(strat);
#if defined(FOOLISH_SEEDED_BOTS_ONLY) && defined(FOOLISH_ORACLE_BUILD)
    return d && (!d->native_only || NAMED[strat] != NULL);   // and fusion (ORACLE_LINKED)
#elif defined(FOOLISH_SEEDED_BOTS_ONLY)
    return d && !d->native_only;
#else
    return d != NULL;
#endif
}

int strategy_choose(int strat, const Game *g, int bot_idx, const LegalMoves *moves) {
    const StrategyFn fn = !bot_def(strat) ? NULL : NAMED[strat] ? NAMED[strat] : RESEARCH[strat];
    return fn ? fn(g, bot_idx, moves, NULL) : -1;
}

// No strcmp: the freestanding wasm modules link this file and have none.
static int same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int parse_strategy(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < STRAT_COUNT; i++)
        if (BOTS[i].name && (same(BOTS[i].name, s) || same(BOTS[i].alias, s))) return i;
    return -1;
}
