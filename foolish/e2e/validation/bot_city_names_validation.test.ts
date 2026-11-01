// No bot on the website is named after an explosive - and the check is now on
// what is STORED, because that is all there is.
//
// The roster's strategy keys keep their ordnance names in the kernel
// (cordite_strategy.c, octogen_strategy.c) because that is what the brains are
// called and what c/src/bots.c dispatches on. The player-facing nickname is a
// city on the road to Moscow, the seeded row's `city` in that same registry,
// and the brain's key names the city's translation: the server names a rung's
// accounts "%<city> 1".."%<city> 7" (foolish/server/src/store/bot_seed.c), and
// src/common/botName.ts botDisplayName finds the registry row by that city and
// shows `bot.<key>` in the page's language (Майами 1 in Russian). A name whose
// city no row has (an old replay's ordnance name) shows as its blob stored it.
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
import { EN, translate, type StringId, type StringTable } from '../../src/common/strings.ts';
import { FoolishStringsRu } from '../../sdk/ts/gen/i18n/strings.ru.ts';
import { FoolishLanguages } from '../../sdk/ts/gen/i18n/languages.ts';
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

// The eight rungs, weakest to strongest (docs/IOS_BOT_NAMING.md). Spelled out
// rather than derived, because this is the list the site is held to.
const LADDER = ['Miami', 'New York', 'Seoul', 'Madrid', 'Vienna', 'Warsaw', 'St. Petersburg', 'Moscow'];

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

const tIn = (table: StringTable) => (id: StringId) => translate(table, id);

test('the display name is the city in the reader\'s language, and the instance', () => {
    const en = tIn(EN), ru = tIn(FoolishStringsRu);
    assert.equal(botDisplayName('%Moscow 2', en), 'Moscow 2');
    assert.equal(botDisplayName('%St. Petersburg 7', en), 'St. Petersburg 7');
    assert.equal(botDisplayName('%Miami 1', ru), 'Майами 1');
    assert.equal(botDisplayName('%Moscow 2', ru), 'Москва 2');
    assert.equal(botDisplayName('%St. Petersburg 7', ru), 'Петербург 7');
    // An old replay blob's ordnance name comes through as itself - that game
    // really was played by a bot of that name - and a human's name is theirs.
    for (const t of [en, ru]) {
        assert.equal(botDisplayName('%Octogen 1', t), 'Octogen 1');
        assert.equal(botDisplayName('%Semtex 3', t), 'Semtex 3');
        assert.equal(botDisplayName('%0x00C0FFEE', t), '0x00C0FFEE');
        assert.equal(botDisplayName('Anna', t), 'Anna');
    }
});

test('every seeded city has its name in every language', async () => {
    const missing: string[] = [];
    for (const { code } of FoolishLanguages) {
        const cap = code.charAt(0).toUpperCase() + code.slice(1);
        const table = (await import(`../../sdk/ts/gen/i18n/strings.${code}.ts`))[`FoolishStrings${cap}`] as StringTable;
        for (const r of seededCities()) {
            const id = `bot.${r.key}` as StringId;
            if (!table[id]) missing.push(`${code}: ${id}`);
        }
    }
    assert.deepEqual(missing, [], 'a seeded city with no translation would show as its key');
});
