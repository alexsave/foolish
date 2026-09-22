// The picker walks the road to Moscow, in the order the road runs.
//
// Miami 1 … Miami 7, New York 1 … Seoul … Madrid … Vienna … St. Petersburg …
// Moscow 7: weakest rung to strongest, and within a rung by instance number.
// Before this the lobby asked Postgres for `created_at` descending, which is the
// order seed.sql happens to INSERT, backwards - the picker opened on Moscow 7
// and walked out through the families in reverse.
//
// THREE SOURCES MEET HERE and none of them is restated in TypeScript: the rows
// come from seed.sql, the strength order from `tier` in c/src/bot_roster.c, and
// the names from `bot.<key>` in c/i18n. The comparator the lobby uses
// (src/common/botLadder.ts) is handed the tiers, so this drives the real one
// over the real data rather than a fixture that could agree with a bug.
//
// Pure test - no Postgres, no network, no compiler, no wasm.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { sortBotsByLadder, instanceOf } from '../../src/common/botLadder.ts';
import { botDisplayName } from '../../src/common/botName.ts';
import type { StringId } from '../../src/localization/strings.ts';

const REPO = resolve(import.meta.dirname, '../..');
const read = (p: string) => readFileSync(join(REPO, p), 'utf8');

/** strategy key -> tier, read as text from the C table. */
function tiers(): Map<string, number> {
    const src = read('c/src/bot_roster.c');
    const body = src.slice(src.indexOf('static const BotRosterEntry ROSTER[]'));
    const rows = body.slice(0, body.indexOf('};')).replace(/\/\/[^\n]*/g, '');
    const out = new Map<string, number>();
    const ROW = /\{\s*"([^"]+)"\s*,\s*STRAT_\w+\s*,\s*((?:"[^"]*"|\w+|\s)+?)\s*,\s*\d\s*,\s*\d\s*,\s*\d\s*,\s*(\d+)\s*\}/g;
    for (const m of rows.matchAll(ROW)) out.set(m[1], Number(m[3]));
    assert.ok(out.size >= 10, `parsed only ${out.size} roster rows - parser drifted?`);
    return out;
}

/** The rows seed.sql inserts, as the site stores them. */
function seededRows(): { nickname: string; strategy_key: string }[] {
    const src = read('server/impls/supabase/seed.sql').replace(/--[^\n]*/g, '');
    const at = src.indexOf('INSERT INTO bots');
    const stmt = src.slice(at, src.indexOf(';', at));
    const out: { nickname: string; strategy_key: string }[] = [];
    for (const m of stmt.matchAll(/\(\s*'([^']*)'\s*,\s*'([^']+)'\s*\)/g)) {
        out.push({ nickname: `%${m[1]}`, strategy_key: m[2] });
    }
    assert.ok(out.length >= 20, `parsed only ${out.length} seeded rows - parser drifted?`);
    return out;
}

async function english(): Promise<(id: StringId) => string> {
    const mod = await import(join(REPO, 'sdk/ts/gen/i18n/strings.en.ts'));
    const t = mod[Object.keys(mod).find((k) => k.startsWith('FoolishStrings'))!];
    return (id: StringId) => t[id] ?? '';
}

test('the picker opens on the weakest bot and ends on the strongest', async () => {
    const tier = tiers();
    const t = await english();
    // Shuffled deterministically: the input order must not survive into the
    // output, or the test would pass on a comparator that returns 0.
    const rows = seededRows();
    const shuffled = [...rows].sort((a, b) =>
        a.nickname.length - b.nickname.length || b.nickname.localeCompare(a.nickname));

    const shown = sortBotsByLadder(shuffled, (k) => tier.get(k)).map((r) => botDisplayName(r.nickname, t));

    assert.equal(shown[0], 'Miami 1', `the picker opens on ${shown[0]}, not the weakest rung`);
    assert.equal(shown[shown.length - 1], 'Moscow 7', `the picker ends on ${shown[shown.length - 1]}`);

    // The cities in order of first appearance: the ladder, weakest to strongest.
    const ladder = [...new Set(shown.map((n) => n.replace(/ \d+$/, '')))];
    assert.deepEqual(ladder, ['Miami', 'New York', 'Seoul', 'Madrid', 'Vienna', 'St. Petersburg', 'Moscow'],
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
        { nickname: '%Octogen 1', strategy_key: 'octogen' },
        { nickname: '%Mystery 1', strategy_key: 'not_in_the_roster' },
        { nickname: '%Random 1', strategy_key: 'random' },
    ];
    const order = sortBotsByLadder(rows, (k) => tier.get(k)).map((r) => r.nickname);
    assert.deepEqual(order, ['%Random 1', '%Octogen 1', '%Mystery 1'],
        'an unranked strategy belongs at the end of the picker, where it is visible');
});

test('instance numbers sort numerically, not as text', () => {
    // The seeded families stop at 7, where lexicographic order and numeric order
    // agree - so the seeded-rows test above cannot tell the two apart, and a
    // mutation check proved it: deleting the numeric compare left it passing.
    // Ten of a family is what separates them, and is the day this matters.
    const tier = tiers();
    const rows = [2, 10, 1, 11, 3].map((n) => ({ nickname: `%Cordite ${n}`, strategy_key: 'cordite' }));
    const order = sortBotsByLadder(rows, (k) => tier.get(k)).map((r) => r.nickname);
    assert.deepEqual(order, ['%Cordite 1', '%Cordite 2', '%Cordite 3', '%Cordite 10', '%Cordite 11'],
        'Cordite 10 must come after Cordite 3, which text order gets wrong');
});

test('the instance number is the trailing one, and a name without one sorts last', () => {
    assert.equal(instanceOf('%Cordite 3'), 3);
    assert.equal(instanceOf('%Simple Heuristic 12'), 12);
    assert.equal(instanceOf('%0x00C0FFEE'), Number.MAX_SAFE_INTEGER);
    assert.equal(instanceOf('%Moscow'), Number.MAX_SAFE_INTEGER);
});
