// No bot on the website is named after an explosive - and the check is now on
// what is STORED, because that is all there is.
//
// The roster's strategy keys keep their ordnance names in the kernel
// (cordite_strategy.c, octogen_strategy.c) because that is what the brains are
// called and what c/src/bots.c dispatches on. The player-facing nickname is a
// city on the road to Moscow, the seeded row's `city` in that same registry,
// and nothing maps between them: the server names a rung's accounts
// "%<city> 1".."%<city> 7" (foolish/server/src/store/bot_seed.c) and
// src/common/botDisplayName drops the reserved '%' and returns the rest. So a
// page shows what the registry's city says, and a replay shows what its blob
// stored.
//
// THAT MAKES THIS GATE STRONGER THAN THE ONE IT REPLACES. It used to check that
// a render-time map covered every seeded rung in 25 languages; there is no map
// to cover, so it reads the cities the server stores and refuses an ordnance
// word outright. The 25-language `bot.*` strings still exist for the phone's
// offline picker, which names a seat from its strategy key -
// ios/FoolishTests/LocalizationTests holds those, not this file.
//
// Pure test - no Postgres, no network, no compiler, no wasm.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { botDisplayName } from '../../src/common/botName.ts';
import { BotRegistry as Rows } from '../../sdk/ts/gen/bot_registry.ts';

// The registry's rows as the C reads them: a field a row leaves out (a research
// brain has no key, an unseeded bot no city) is its zero value.
const BotRegistry = Rows.map((r) => ({
    name: r.name, key: r.key ?? '', knobs: r.knobs ?? '', uses_logs: r.uses_logs ?? 0, seeded: r.seeded ?? 0,
    offline: r.offline ?? 0, tier: r.tier ?? 0, city: r.city ?? '',
}));

/** The seeded rungs, weakest first: the city each one's accounts are named after. */
function seededCities(): { city: string; key: string }[] {
    const out = BotRegistry.filter((r) => r.seeded === 1).sort((a, b) => a.tier - b.tier)
        .map((r) => ({ city: r.city, key: r.key }));
    assert.ok(out.length >= 3, `read only ${out.length} seeded rows - the generated table drifted?`);
    return out;
}

// The seven rungs, weakest to strongest (docs/IOS_BOT_NAMING.md). Spelled out
// rather than derived, because this is the list the site is held to.
const LADDER = ['Miami', 'New York', 'Seoul', 'Madrid', 'Vienna', 'St. Petersburg', 'Moscow'];

// Every family the site has ever seeded, plus the research-only names one grep
// away from a page (docs/IOS_BOT_NAMING.md section 5 on why "Novichok" in
// particular must never render). Spelled out so that renaming a brain in C
// cannot quietly stop this from checking anything.
const ORDNANCE = [
    'cordite', 'octogen', 'semtex', 'blackpowder', 'black powder', 'gunpowder',
    'firecracker', 'torpex', 'astrolite', 'novichok', 'fulminate', 'nitro',
    'dynamite', 'tnt', 'simple heuristic', 'handwritten', 'espresso', 'robusta',
];

test('no seeded city is an ordnance name', () => {
    const bad: string[] = [];
    for (const { city, key } of seededCities()) {
        const low = city.toLowerCase();
        for (const word of ORDNANCE) if (low.includes(word)) bad.push(`${key}: ${city} (contains "${word}")`);
    }
    assert.deepEqual(bad, [], 'these rows put a kernel brain name on a board, where a city belongs:\n  '
        + `${bad.join('\n  ')}\n`
        + 'The city in c/src/bots.c IS what a page shows - there is no display map to hide it.');
});

test('the seeded cities are the ladder, in strength order', () => {
    // Each of them seats seven deep (a human plus seven fills a table); tests.c
    // holds every seeded row to all seven of its accounts.
    // Mutation that turned it red (restored after): Vienna spelled Wien in bots.c.
    assert.deepEqual(seededCities().map((r) => r.city), LADDER,
        'the seeded cities must be exactly the ladder, weakest first - a rung with no city is a bot the site cannot name, '
        + 'and a city off the ladder is one the naming doc never chose');
});

test('the display name is the stored name, minus the reserved prefix', () => {
    // The whole of the rename, at render: one prefix character. In particular an
    // old replay blob's ordnance name comes through as itself - a replay code is
    // a replay code, and that game really was played by a bot of that name.
    assert.equal(botDisplayName('%Moscow 2'), 'Moscow 2');
    assert.equal(botDisplayName('%St. Petersburg 7'), 'St. Petersburg 7');
    assert.equal(botDisplayName('%Octogen 1'), 'Octogen 1');
    assert.equal(botDisplayName('%Semtex 3'), 'Semtex 3');
    assert.equal(botDisplayName('%0x00C0FFEE'), '0x00C0FFEE');
    assert.equal(botDisplayName('Anna'), 'Anna');
});
