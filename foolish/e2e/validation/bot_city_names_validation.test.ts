// No bot on the website is named after an explosive - and the check is now on
// what is STORED, because that is all there is.
//
// The roster's strategy keys keep their ordnance names in the kernel
// (cordite_strategy.c, octogen_strategy.c) because that is what the brains are
// called and what c/src/bot_roster.c dispatches on. The player-facing nickname
// is a city on the road to Moscow, in the table, and nothing maps between them:
// src/common/botDisplayName drops the reserved '%' and returns the rest. So a
// page shows what seed.sql says, and a replay shows what its blob stored.
//
// THAT MAKES THIS GATE STRONGER THAN THE ONE IT REPLACES. It used to check that
// a render-time map covered every seeded rung in 25 languages; there is no map
// to cover, so it reads the stored names and refuses an ordnance word outright.
// The 25-language `bot.*` strings still exist for the phone's offline picker,
// which names a seat from its strategy key - ios/FoolishTests/LocalizationTests
// holds those, not this file.
//
// Pure test - no Postgres, no network, no compiler, no wasm.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { botDisplayName } from '../../src/common/botName.ts';

const REPO = resolve(import.meta.dirname, '../..');
const read = (p: string) => readFileSync(join(REPO, p), 'utf8');

/** The rows seed.sql inserts: stored nickname and strategy key. */
function seededRows(): { nickname: string; key: string }[] {
    // Strip `--` comments first: the prose between the rows carries semicolons
    // and apostrophes, either of which ends the statement early and silently.
    const src = read('server/impls/supabase/seed.sql').replace(/--[^\n]*/g, '');
    const at = src.indexOf('INSERT INTO bots');
    const stmt = src.slice(at, src.indexOf(';', at));
    const out: { nickname: string; key: string }[] = [];
    for (const m of stmt.matchAll(/\(\s*'([^']*)'\s*,\s*'([^']+)'\s*\)/g)) {
        out.push({ nickname: m[1], key: m[2] });
    }
    assert.ok(out.length >= 20, `parsed only ${out.length} seeded rows - parser drifted?`);
    return out;
}

// The seven rungs, weakest to strongest (docs/IOS_BOT_NAMING.md). Spelled out
// rather than derived, because this is the list the site is held to.
const LADDER = ['Miami', 'New York', 'Seoul', 'Madrid', 'Vienna', 'St. Petersburg', 'Moscow'];

// A bot is eight seats minus the human, so a rung needs seven rows to fill a
// table with itself.
const SEATS_TO_FILL = 7;

// Every family the site has ever seeded, plus the research-only names one grep
// away from a page (docs/IOS_BOT_NAMING.md section 5 on why "Novichok" in
// particular must never render). Spelled out so that renaming a brain in C
// cannot quietly stop this from checking anything.
const ORDNANCE = [
    'cordite', 'octogen', 'semtex', 'blackpowder', 'black powder', 'gunpowder',
    'firecracker', 'torpex', 'astrolite', 'novichok', 'fulminate', 'nitro',
    'dynamite', 'tnt', 'simple heuristic', 'handwritten', 'espresso', 'robusta',
];

test('no seeded nickname is an ordnance name', () => {
    const bad: string[] = [];
    for (const { nickname } of seededRows()) {
        const low = nickname.toLowerCase();
        for (const word of ORDNANCE) if (low.includes(word)) bad.push(`${nickname} (contains "${word}")`);
    }
    assert.deepEqual(bad, [], 'these rows put a kernel brain name on a board, where a city belongs:\n  '
        + `${bad.join('\n  ')}\n`
        + 'The nickname in seed.sql IS what a page shows - there is no display map to hide it.');
});

test('every seeded nickname is a rung of the ladder plus an instance number', () => {
    const wrong: string[] = [];
    for (const { nickname } of seededRows()) {
        const m = /^(.+?) (\d+)$/.exec(nickname);
        if (!m || !LADDER.includes(m[1])) wrong.push(nickname);
    }
    assert.deepEqual(wrong, [], 'these nicknames are not "<city> <n>" for a city on the ladder '
        + `(${LADDER.join(', ')}):\n  ${wrong.join('\n  ')}\n`);
});

test('every rung of the ladder is seated, seven deep', () => {
    const byCity = new Map<string, number>();
    for (const { nickname } of seededRows()) {
        const city = nickname.replace(/ \d+$/, '');
        byCity.set(city, (byCity.get(city) ?? 0) + 1);
    }
    assert.deepEqual([...byCity.keys()].sort(), [...LADDER].sort(),
        'the seeded cities must be exactly the ladder - a rung with no rows is a city the site advertises and cannot seat');
    const short = [...byCity].filter(([, n]) => n < SEATS_TO_FILL).map(([c, n]) => `${c}: ${n}`);
    assert.deepEqual(short, [], 'these rungs cannot fill an eight-seat table on their own '
        + `(a human plus ${SEATS_TO_FILL}):\n  ${short.join('\n  ')}\n`);
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
