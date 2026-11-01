// The picker walks the road to Moscow, in the order the road runs.
//
// Miami 1 … Miami 7, New York 1 … Seoul … Madrid … Vienna … Warsaw … St. Petersburg …
// Moscow 7: weakest rung to strongest, and within a rung by instance number.
// Before this the lobby asked Postgres for `created_at` descending, which is the
// order foolish's bot_seed.c happens to INSERT, backwards - the picker opened on Moscow 7
// and walked out through the families in reverse.
//
// ONE SOURCE MEETS THE COMPARATOR HERE and nothing is restated in TypeScript:
// the rows and the strength order both come from the kernel's bot registry
// (c/src/bots.c), read through datagen (sdk/ts/gen/bot_registry.ts). A seeded
// row's server accounts are "%<city> 1".."%<city> 7" (foolish/server/src/store/
// bot_seed.c, where the nickname IS the city - there is no display map), and
// its `tier` is the strength order. The comparator the lobby uses
// (src/common/botLadder.ts) is handed the tiers, so this drives the real one
// over the real data rather than a fixture that could agree with a bug.
//
// Pure test - no Postgres, no network, no compiler, no wasm.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { sortBotsByLadder, instanceOf } from '../../src/common/botLadder.ts';
import { botDisplayName } from '../../src/common/botName.ts';
import { EN, translate, type StringId } from '../../src/common/strings.ts';
import { BotRegistry as Rows } from '../../sdk/ts/gen/bot_registry.ts';

// The registry's rows as the C reads them: a field a row leaves out (a research
// brain has no key, an unseeded bot no city) is its zero value.
const BotRegistry = Rows.map((r) => ({
    name: r.name, key: r.key ?? '', knobs: r.knobs ?? '', uses_logs: r.uses_logs ?? 0, seeded: r.seeded ?? 0,
    offline: r.offline ?? 0, tier: r.tier ?? 0, city: r.city ?? '',
}));

// A seeded bot's accounts per rung: BOT_IDS_MAX in c/src/bots.h, which
// tests.c holds every seeded row to.
const ACCOUNTS = 7;

/** strategy key -> tier, from the registry. */
function tiers(): Map<string, number> {
    const out = new Map<string, number>();
    for (const r of BotRegistry) if (r.key) out.set(r.key, r.tier);
    assert.ok(out.size >= 10, `read only ${out.size} named bots - the generated table drifted?`);
    return out;
}

/** The rows the server seeds, as the site stores them. */
function seededRows(): { nickname: string; strategy_key: string }[] {
    const out: { nickname: string; strategy_key: string }[] = [];
    for (const r of BotRegistry) {
        if (r.seeded !== 1) continue;
        for (let n = 1; n <= ACCOUNTS; n++) out.push({ nickname: `%${r.city} ${n}`, strategy_key: r.key });
    }
    assert.ok(out.length >= 20, `read only ${out.length} seeded rows - the generated table drifted?`);
    return out;
}

test('the picker opens on the weakest bot and ends on the strongest', () => {
    const tier = tiers();
    // Shuffled deterministically: the input order must not survive into the
    // output, or the test would pass on a comparator that returns 0.
    const rows = seededRows();
    const shuffled = [...rows].sort((a, b) =>
        a.nickname.length - b.nickname.length || b.nickname.localeCompare(a.nickname));

    const shown = sortBotsByLadder(shuffled, (k) => tier.get(k)).map((r) => botDisplayName(r.nickname, (id: StringId) => translate(EN, id)));

    assert.equal(shown[0], 'Miami 1', `the picker opens on ${shown[0]}, not the weakest rung`);
    assert.equal(shown[shown.length - 1], 'Moscow 7', `the picker ends on ${shown[shown.length - 1]}`);

    // The cities in order of first appearance: the ladder, weakest to strongest.
    const ladder = [...new Set(shown.map((n) => n.replace(/ \d+$/, '')))];
    assert.deepEqual(ladder, ['Miami', 'New York', 'Seoul', 'Madrid', 'Vienna', 'Warsaw', 'St. Petersburg', 'Moscow'],
        'the cities must appear in strength order - the road to Moscow, weakest first');

    // …and inside each city, 1 before 2 before 3.
    const wrong: string[] = [];
    for (const city of ladder) {
        const nums = shown.filter((n) => n.startsWith(`${city} `)).map((n) => Number(n.slice(city.length + 1)));
        if (nums.join(',') !== [...nums].sort((a, b) => a - b).join(',')) wrong.push(`${city}: ${nums.join(',')}`);
    }
    assert.deepEqual(wrong, [], `these rungs are not in instance order:\n  ${wrong.join('\n  ')}`);
});

test('a bot whose strategy the kernel does not rank sorts last, not into the middle', () => {
    const tier = tiers();
    const rows = [
        { nickname: '%Moscow 1', strategy_key: 'octogen' },
        { nickname: '%Mystery 1', strategy_key: 'not_in_the_roster' },
        { nickname: '%Miami 1', strategy_key: 'random' },
    ];
    const order = sortBotsByLadder(rows, (k) => tier.get(k)).map((r) => r.nickname);
    assert.deepEqual(order, ['%Miami 1', '%Moscow 1', '%Mystery 1'],
        'an unranked strategy belongs at the end of the picker, where it is visible');
});

test('instance numbers sort numerically, not as text', () => {
    // The seeded families stop at 7, where lexicographic order and numeric order
    // agree - so the seeded-rows test above cannot tell the two apart, and a
    // mutation check proved it: deleting the numeric compare left it passing.
    // Ten of a family is what separates them, and is the day this matters.
    const tier = tiers();
    const rows = [2, 10, 1, 11, 3].map((n) => ({ nickname: `%St. Petersburg ${n}`, strategy_key: 'cordite' }));
    const order = sortBotsByLadder(rows, (k) => tier.get(k)).map((r) => r.nickname);
    assert.deepEqual(order, ['%St. Petersburg 1', '%St. Petersburg 2', '%St. Petersburg 3',
        '%St. Petersburg 10', '%St. Petersburg 11'],
        'St. Petersburg 10 must come after St. Petersburg 3, which text order gets wrong');
});

test('the instance number is the trailing one, and a name without one sorts last', () => {
    assert.equal(instanceOf('%Moscow 3'), 3);
    assert.equal(instanceOf('%St. Petersburg 12'), 12);
    assert.equal(instanceOf('%0x00C0FFEE'), Number.MAX_SAFE_INTEGER);
    assert.equal(instanceOf('%Moscow'), Number.MAX_SAFE_INTEGER);
});
