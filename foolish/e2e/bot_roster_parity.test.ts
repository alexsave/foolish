// The bot roster is written down in two places, and the kernel's build is a
// third witness. This test makes them agree.
//
// The canonical table is the C kernel's (c/src/bot_roster.c): key -> brain
// + tuning knobs + logs flag + flags, shared by the server, the phone and every
// future client (docs/C_CORE_CONSOLIDATION.md F1/A1). The server seats a bot by
// its key through that table on the C Table (table_load refuses a brain the
// build does not know). The other places that name bots:
//
//   * server/impls/supabase/seed.sql - the live bot rows, which must be exactly
//     the roster's `seeded` set, each linked into the shipped bots.wasm. A seeded
//     key the kernel does not dispatch is a bot that plays nothing like its name
//     and pollutes the Elo leaderboard.
//   * .github/workflows/memory.yml - the edge memory gate's bot keysets.
//
// The roster and seed.sql are read as TEXT on purpose: those assertions must
// hold without a C or wasm build, so they also catch an edit that lands before
// bots.wasm.gz is regenerated. The kernel's own dump (kernelBotRoster) is then
// held to the text, so the shipped module carries the table the source says.
import { test } from 'node:test';
import { kernelBotRoster } from '../sdk/ts/wasm/bots.ts';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const read = (p: string) => readFileSync(join(ROOT, p), 'utf8');

type RosterEntry = {
    key: string;
    strat: string;
    knobs: Record<string, string>;
    usesLogs: boolean;
    seeded: boolean;
    offline: boolean;
    tier: number;
};

// Parse the C table's rows:
//   { "cordite", STRAT_CORDITE, CORDITE_KNOBS, 1, 1, 1, 9 },
// Macro-valued knobs (CORDITE_KNOBS) and the adjacent-string-literal form used
// for octogen are resolved against the #defines above the table.
function parseCRoster(): RosterEntry[] {
    const src = read('c/src/bot_roster.c');

    const defines = new Map<string, string>();
    for (const m of src.matchAll(/^#define\s+(\w+)\s+"([^"]*)"\s*$/gm)) defines.set(m[1], m[2]);

    const body = src.slice(src.indexOf('static const BotRosterEntry ROSTER[]'));
    // Strip // comments (the table's column header is one) so they can't be
    // mistaken for row content; rows may wrap across lines.
    const table = body.slice(0, body.indexOf('};')).replace(/\/\/[^\n]*/g, '');

    const rows: RosterEntry[] = [];
    const ROW = /\{\s*"([^"]+)"\s*,\s*(STRAT_\w+)\s*,\s*((?:"[^"]*"|\w+|\s)+?)\s*,\s*(\d)\s*,\s*(\d)\s*,\s*(\d)\s*,\s*(\d+)\s*\}/g;
    for (const m of table.matchAll(ROW)) {
        rows.push({
            key: m[1],
            strat: m[2],
            knobs: parseKnobSpec(resolveStr(m[3], defines)),
            usesLogs: m[4] === '1',
            seeded: m[5] === '1',
            offline: m[6] === '1',
            tier: Number(m[7]),
        });
    }
    return rows;
}

// A knob field is "" | MACRO | "LIT" | "LIT" MACRO (adjacent literals concat).
function resolveStr(expr: string, defines: Map<string, string>): string {
    let out = '';
    for (const tok of expr.match(/"[^"]*"|\w+/g) ?? []) {
        if (tok.startsWith('"')) out += tok.slice(1, -1);
        else if (defines.has(tok)) out += defines.get(tok)!;
        else throw new Error(`bot_roster.c: unresolved knob token ${tok}`);
    }
    return out;
}

// "CD_BUDGET=prod,CD_RACE=1" -> { CD_BUDGET: 'prod', CD_RACE: '1' }
function parseKnobSpec(spec: string): Record<string, string> {
    const out: Record<string, string> = {};
    for (const part of spec.split(',')) {
        if (!part) continue;
        const eq = part.indexOf('=');
        assert.ok(eq > 0, `malformed knob "${part}"`);
        out[part.slice(0, eq)] = part.slice(eq + 1);
    }
    return out;
}

// The bots seed.sql actually inserts.
function parseSeededKeys(): Set<string> {
    // Strip `--` comments FIRST: the prose between the rows contains semicolons
    // and apostrophes, either of which would otherwise end the statement early
    // and silently under-report the seeded set.
    const src = read('server/impls/supabase/seed.sql').replace(/--[^\n]*/g, '');
    const at = src.indexOf('INSERT INTO bots');
    assert.ok(at > 0, 'seed.sql: no INSERT INTO bots');
    const end = src.indexOf(';', at);
    assert.ok(end > at, 'seed.sql: unterminated INSERT INTO bots');
    const stmt = src.slice(at, end);
    const keys = new Set<string>();
    for (const m of stmt.matchAll(/\(\s*'[^']*'\s*,\s*'([^']+)'\s*\)/g)) keys.add(m[1]);
    return keys;
}

test('C roster: table is well-formed and is the strength ladder', () => {
    const roster = parseCRoster();
    assert.ok(roster.length >= 10, `parsed only ${roster.length} roster rows — parser drifted?`);

    const keys = roster.map(r => r.key);
    assert.equal(new Set(keys).size, keys.length, 'roster keys must be unique');

    const tiers = roster.map(r => r.tier);
    assert.deepEqual(tiers, [...tiers].sort((a, b) => a - b), 'roster must be in tier order');
    assert.equal(new Set(tiers).size, tiers.length, 'tiers must be unique');

    // Every STRAT_* the table names must exist in the kernel's strategy.h.
    const strategyH = read('c/src/strategy.h');
    for (const r of roster) {
        assert.ok(new RegExp(`#define\\s+${r.strat}\\s`).test(strategyH),
            `bot_roster.c names ${r.strat}, which strategy.h does not define`);
    }

    // The rungs the player-facing ladder is made of (docs/IOS_BOT_NAMING.md §1).
    assert.deepEqual(roster.filter(r => r.offline).map(r => r.key), [
        'random', 'simple_heuristic', 'handwritten', 'espresso', 'robusta',
        'firecracker', 'gunpowder', 'blackpowder', 'cordite', 'octogen',
    ], 'the offline picker rungs must match the bot-naming ladder, in order');
});

test('C roster: handwritten/espresso are the PRODUCTION mirrors, not arena variants', () => {
    // The arena/rollout variants (STRAT_HANDWRITTEN, STRAT_ESPRESSO) drifted from
    // the production bots and stay frozen for cordite's rollout policy. A
    // player-facing rung pointing at one of them is the §3 bug: offline
    // "Handwritten" was not the site's Handwritten.
    const roster = parseCRoster();
    const by = (k: string) => roster.find(r => r.key === k)!;
    assert.equal(by('handwritten').strat, 'STRAT_HANDWRITTEN_PROD');
    assert.equal(by('espresso').strat, 'STRAT_ESPRESSO_PROD');
});

test('C roster: the _max tiers are retired', () => {
    // octogen_max aliased octogen; cordite_max's flat CD_BUDGET=max budget was
    // WEAKER than the prod schedule at 6-8 players. One cordite, prod budget.
    const keys = new Set(parseCRoster().map(r => r.key));
    for (const gone of ['cordite_max', 'octogen_max', 'semtex_max']) {
        assert.ok(!keys.has(gone), `${gone} must not return to the roster`);
    }
    const dumped = new Set(kernelBotRoster().map(e => e.key));
    for (const gone of ['cordite_max', 'octogen_max', 'semtex_max']) {
        assert.ok(!dumped.has(gone), `${gone} must not be in the kernel's roster`);
    }
    assert.ok(!parseSeededKeys().has('cordite_max'), 'cordite_max must not be seeded');
    assert.ok(!parseSeededKeys().has('octogen_max'), 'octogen_max must not be seeded');
});

test('the shipped kernel carries the roster the source says, and links every seeded bot', () => {
    const roster = parseCRoster();
    const dump = new Map(kernelBotRoster().map(e => [e.key, e]));
    assert.deepEqual([...dump.keys()], roster.map(r => r.key), 'the kernel dumps the rows bot_roster.c lists, in order');
    for (const entry of roster) {
        const k = dump.get(entry.key)!;
        assert.equal(k.usesLogs, entry.usesLogs,
            `${entry.key}: uses_logs differs - the server would skip or feed the belief log wrongly`);
        assert.equal(k.seeded, entry.seeded, `${entry.key}: seeded differs from bot_roster.c`);
        assert.equal(k.offline, entry.offline, `${entry.key}: offline differs from bot_roster.c`);
        assert.equal(k.tier, entry.tier, `${entry.key}: tier differs from bot_roster.c`);
        // A seeded bot is dealt on the site: the shipped module must run its
        // brain, or a seat carrying it has no bot behind its name.
        if (entry.seeded) assert.ok(k.linked, `${entry.key} is seeded but not linked into bots.wasm`);
    }
});

// A bot NAME in CI is a claim about what is being measured. memory.yml's
// edge-serve job spent months driving `semtex`/`fulminate` — keys culled from
// the roster and never linked into bots.wasm — which getBotStrategy resolved to
// `random`: the cheapest bot in the tree, measured against the edge memory
// budget and reported green under the expensive bots' names (issue #111). The
// job's own guard could not catch it, because `random` makes real moves and the
// guard only asked for moves.
//
// So hold the keysets to the roster HERE, where it costs no CI minutes and no
// edge runtime. Text, like the rest of this file: it must fail on the commit
// that culls a bot, not on the next run that happens to boot a stack.
function parseWorkflowKeysets(): string[][] {
    const yml = read('.github/workflows/memory.yml');
    const m = /^\s*for keys in ([^;]+); do\s*$/m.exec(yml);
    if (!m) {
        throw new Error('memory.yml no longer has a `for keys in ...; do` loop — '
            + 'if the edge-serve gate moved, move this guard with it');
    }
    return m[1].trim().split(/\s+/).map(set => set.split(','));
}

test('memory.yml drives bots that this tree actually ships', () => {
    const roster = parseCRoster();
    const linked = new Set(kernelBotRoster().filter(e => e.linked).map(e => e.key));
    const keysets = parseWorkflowKeysets();

    assert.ok(keysets.length > 0, 'the edge-serve gate drives no bots at all');

    for (const keys of keysets) {
        assert.ok(keys.length >= 2, `keyset ${keys.join(',')} seats fewer than 2 players`);
        for (const key of keys) {
            const entry = roster.find(r => r.key === key);
            assert.ok(entry, `memory.yml drives '${key}', which is not in the C roster `
                + `(c/src/bot_roster.c). It resolves to 'random', and the job measures that.`);
            // In the roster is not enough: a brain the shipped bots.wasm does not
            // link cannot be seated, so the gate would measure nothing under its name.
            assert.ok(linked.has(key), `memory.yml drives '${key}', which the C roster knows `
                + `but the shipped bots.wasm does not link.`);
        }
    }

    // The gate exists to reproduce production memory kills, which only the
    // Monte-Carlo bots can produce. A keyset of nothing but cheap bots would
    // satisfy every check above and still measure nothing.
    const MONTE_CARLO = new Set(['firecracker', 'blackpowder', 'cordite', 'octogen']);
    for (const keys of keysets) {
        assert.ok(keys.some(k => MONTE_CARLO.has(k)),
            `keyset ${keys.join(',')} has no Monte-Carlo bot in it: nothing in that game can `
            + `reach the edge memory budget the job exists to guard`);
    }
});

test('seed.sql seeds exactly the roster\'s `seeded` set', () => {
    const roster = parseCRoster();
    const expected = roster.filter(r => r.seeded).map(r => r.key).sort();
    const actual = [...parseSeededKeys()].sort();
    assert.deepEqual(actual, expected,
        'the seeded bot rows must equal the roster\'s seeded set — a seeded key the ' +
        'kernel does not dispatch silently plays as `random`');
});
