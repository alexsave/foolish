// No bot reaches a web page under the name of an explosive.
//
// The roster is named after explosives everywhere it is stored - the C strategy
// keys, bots.nickname, the names inside a replay blob - and the website renders
// each one as its rung's city on the road to Moscow (docs/IOS_BOT_NAMING.md).
// The rendering is one function, src/common/botName.ts, and the cities are
// `bot.<strategy key>` in c/i18n, read by the phone app through the same keys.
//
// THAT LEAVES A JOIN NOTHING IN C CHECKS. The roster is one table
// (c/src/bot_roster.c) and the cities are another (c/i18n), and a rung seeded
// onto the site with no city in the second renders as the raw stored base -
// "Simple Heuristic 2" on a board between Miami and Madrid, or, for a family
// whose name is the thing this whole map exists to hide, "Cordite 1". Splitting
// a table means something has to inherit the check the compiler used to make
// (c/i18n/README.md on --require-complete); for these two tables, this is it.
//
// Pure test - no Postgres, no network, no compiler, no wasm. It reads the C as
// text and the generated string modules, so it fails on the commit that breaks
// the join rather than on the next run that happens to build a module.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { botDisplayName } from '../../src/common/botName.ts';
import type { StringId } from '../../src/localization/strings.ts';

const REPO = resolve(import.meta.dirname, '../..');
const read = (p: string) => readFileSync(join(REPO, p), 'utf8');
const GEN = 'sdk/ts/gen/i18n';

/** One language's generated table. */
async function table(code: string): Promise<Record<string, string>> {
    const mod = await import(join(REPO, GEN, `strings.${code}.ts`));
    const key = Object.keys(mod).find((k) => k.startsWith('FoolishStrings'))!;
    return mod[key];
}

async function languages(): Promise<string[]> {
    const { FoolishLanguages } = await import(join(REPO, GEN, 'languages.ts'));
    return FoolishLanguages.map((l: { code: string }) => l.code);
}

/** The roster's seeded keys, read as text from the C table. */
function seededKeys(): string[] {
    const src = read('c/src/bot_roster.c');
    const body = src.slice(src.indexOf('static const BotRosterEntry ROSTER[]'));
    const rows = body.slice(0, body.indexOf('};')).replace(/\/\/[^\n]*/g, '');
    const out: string[] = [];
    // { "cordite", STRAT_CORDITE, CORDITE_KNOBS, 1, 1, 1, 9 } - the fifth column
    // (after key, strat, knobs, uses_logs) is `seeded`.
    const ROW = /\{\s*"([^"]+)"\s*,\s*STRAT_\w+\s*,\s*((?:"[^"]*"|\w+|\s)+?)\s*,\s*\d\s*,\s*(\d)\s*,\s*\d\s*,\s*\d+\s*\}/g;
    for (const m of rows.matchAll(ROW)) if (m[3] === '1') out.push(m[1]);
    assert.ok(out.length >= 5, `parsed only ${out.length} seeded roster rows - parser drifted?`);
    return out;
}

/** The rows seed.sql actually inserts: stored nickname and strategy key. */
function seededRows(): { nickname: string; key: string }[] {
    // Strip `--` comments first: the prose between the rows carries semicolons
    // and apostrophes, either of which ends the statement early and silently.
    const src = read('server/impls/supabase/seed.sql').replace(/--[^\n]*/g, '');
    const at = src.indexOf('INSERT INTO bots');
    const stmt = src.slice(at, src.indexOf(';', at));
    const out: { nickname: string; key: string }[] = [];
    for (const m of stmt.matchAll(/\(\s*'([^']*)'\s*,\s*'([^']+)'\s*\)/g)) {
        out.push({ nickname: `%${m[1]}`, key: m[2] });
    }
    assert.ok(out.length >= 10, `parsed only ${out.length} seeded rows - parser drifted?`);
    return out;
}

// A table is eight seats, so filling one with a single rung takes a human and
// seven copies of that bot. Fewer rows and the lobby runs out mid-fill.
const SEATS_TO_FILL = 7;

// The word list is the point of the exercise, so it is spelled out rather than
// derived from the roster: a rung renamed in C must not quietly stop being
// checked here. Every family the site has ever seeded, plus the research-only
// names one grep away from a page (docs/IOS_BOT_NAMING.md §5 on why "Novichok"
// in particular must never render).
const ORDNANCE = [
    'cordite', 'octogen', 'semtex', 'blackpowder', 'black powder', 'gunpowder',
    'firecracker', 'torpex', 'astrolite', 'novichok', 'fulminate', 'nitro',
    'dynamite', 'tnt',
];

test('every seeded bot has a city, in every language', async () => {
    const keys = seededKeys();
    const missing: string[] = [];
    for (const code of await languages()) {
        const t = await table(code);
        for (const key of keys) if (!(t[`bot.${key}`] ?? '').trim()) missing.push(`${code}.bot.${key}`);
    }
    assert.deepEqual(missing, [], 'these seeded bots have no city to render as, so a seat shows the '
        + 'stored name instead - the explosive the display map exists to hide:\n  '
        + `${missing.join('\n  ')}\n`
        + 'Add the string to all of c/i18n/strings_*.c, or take the rung off the site '
        + '(`seeded` 0 in c/src/bot_roster.c and no rows in seed.sql).');
});

test('every seeded family can fill a table on its own', () => {
    const count = new Map<string, number>();
    for (const r of seededRows()) count.set(r.key, (count.get(r.key) ?? 0) + 1);
    const short = [...count].filter(([, n]) => n < SEATS_TO_FILL).map(([k, n]) => `${k}: ${n}`);
    assert.deepEqual(short, [], 'these seeded bots cannot fill an eight-seat table on their own '
        + `(a human plus ${SEATS_TO_FILL}), so the lobby runs out of them mid-fill:\n  `
        + `${short.join('\n  ')}\n`);
});

test('no seeded nickname renders as an explosive, in any language', async () => {
    const stored = seededRows().map((r) => r.nickname);
    const leaks: string[] = [];
    for (const code of await languages()) {
        const t = await table(code);
        const tr = ((id: StringId) => t[id] ?? '') as (id: StringId) => string;
        for (const name of stored) {
            const shown = botDisplayName(name, tr);
            const low = shown.toLowerCase();
            for (const word of ORDNANCE) if (low.includes(word)) leaks.push(`${code}: "${name}" renders as "${shown}"`);
        }
    }
    assert.deepEqual(leaks, [], 'these bots render under an explosive name on the website:\n  '
        + `${leaks.join('\n  ')}\n`
        + 'src/common/botName.ts maps a stored nickname to its rung\'s city; a base with no '
        + '`bot.<key>` string in c/i18n falls through to the stored name.');
});

test('the display map keeps the tail of a stored name, and leaves a human alone', async () => {
    // The parts that are NOT the city: several copies of one bot stay telling
    // apart, the retired Max tier still sits in old replay blobs, the hex easter
    // egg is culture-neutral and stays verbatim, and a human name is not a bot's.
    const t = await table('en');
    const tr = ((id: StringId) => t[id] ?? '') as (id: StringId) => string;
    assert.equal(botDisplayName('%Octogen 2', tr), 'Moscow 2');
    assert.equal(botDisplayName('%Cordite Max 1', tr), 'St. Petersburg Max 1');
    assert.equal(botDisplayName('%Semtex 3', tr), 'Moscow 3', 'the retired family renders as its successor\'s city');
    // The hex bot left the site in September; old replay blobs still carry it.
    assert.equal(botDisplayName('%0x00C0FFEE', tr), '0x00C0FFEE');
    assert.equal(botDisplayName('Anna', tr), 'Anna');
    assert.equal(botDisplayName('%Espresso 1', tr), 'Espresso 1', 'an offline-only rung has no city and is not seeded');

    // …and the city really is the reader's, not English's.
    const ru = await table('ru');
    const rtr = ((id: StringId) => ru[id] ?? '') as (id: StringId) => string;
    assert.equal(botDisplayName('%Octogen 2', rtr), 'Москва 2');
    assert.equal(botDisplayName('%Cordite 1', rtr), 'Петербург 1');
});
