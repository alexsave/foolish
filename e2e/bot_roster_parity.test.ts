// The bot roster is written down in three places. This test makes them agree.
//
// The canonical table is the C kernel's (cnitro/src/bot_roster.c): key -> brain
// + tuning knobs + logs flag + flags, shared by the server, the phone and every
// future client (docs/C_CORE_CONSOLIDATION.md F1/A1). The other two are
// consumers that still restate parts of it:
//
//   * supabase/functions/_shared/bot_strategy.ts — the TS registry, which still
//     carries the knobs as a wasm env table. Env OVERRIDES the roster
//     (cnitro/src/bot_knobs.h), so while both exist the server's behavior is
//     defined by this file — and it must therefore say EXACTLY what the roster
//     says, or the phone and the site run different bots. That is not a
//     hypothetical: before the roster, iOS ran cordite at the arena budget with
//     early-race off, and pointed `handwritten`/`espresso` at the arena variants
//     rather than the production mirrors (§3).
//   * supabase/seed.sql — the live bot rows, which must be exactly the roster's
//     `seeded` set. A seeded key the kernel does not dispatch silently plays as
//     `random` (wasm_choose_move's default arm), i.e. a bot that plays nothing
//     like its name and pollutes the Elo leaderboard.
//
// These are read as TEXT on purpose: the assertion must hold without a C or
// wasm build, so it also catches an edit that lands before bots.wasm.gz is
// regenerated. When the TS env table is deleted (the cutover step), the registry
// half of this test goes with it and the roster simply wins.
import { test } from 'node:test';
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
    const src = read('cnitro/src/bot_roster.c');

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

// Parse the TS registry rows:
//   ['cordite', new WasmBotStrategy('cordite', STRAT.cordite, { env: {...}, logs: true })],
function parseTsRegistry() {
    const src = read('supabase/functions/_shared/bot_strategy.ts');
    const body = src.slice(src.indexOf('BOT_STRATEGIES: Map<string, BotStrategy>'));
    const table = body.slice(0, body.indexOf('\n]);'));

    // Constants referenced inside the env objects (e.g. OCTOGEN_TRUMP_KEEP).
    const consts = new Map<string, string>();
    for (const m of src.matchAll(/^const\s+(\w+)\s*=\s*'([^']*)'\s*;/gm)) consts.set(m[1], m[2]);

    const out = new Map<string, { strat: string; env: Record<string, string>; logs: boolean }>();
    for (const m of table.matchAll(/\[\s*'([^']+)'\s*,\s*new WasmBotStrategy\(\s*'([^']+)'\s*,\s*STRAT\.(\w+)\s*(?:,\s*(\{[\s\S]*?\})\s*)?\)\s*\]/g)) {
        const [, key, name, strat, optsRaw] = m;
        assert.equal(name, key, `registry key '${key}' disagrees with its display name '${name}'`);

        const opts = optsRaw ?? '';
        const env: Record<string, string> = {};
        const envBlock = /env:\s*\{([^}]*)\}/.exec(opts);
        if (envBlock) {
            for (const e of envBlock[1].matchAll(/(\w+)\s*:\s*(?:'([^']*)'|(\w+))/g)) {
                const [, k, lit, ident] = e;
                const v = lit !== undefined ? lit : consts.get(ident);
                assert.ok(v !== undefined, `registry env ${k} references unknown const ${ident}`);
                env[k] = v;
            }
        }
        out.set(key, { strat, env, logs: /logs:\s*true/.test(opts) });
    }
    return out;
}

// The bots seed.sql actually inserts.
function parseSeededKeys(): Set<string> {
    // Strip `--` comments FIRST: the prose between the rows contains semicolons
    // and apostrophes, either of which would otherwise end the statement early
    // and silently under-report the seeded set.
    const src = read('supabase/seed.sql').replace(/--[^\n]*/g, '');
    const at = src.indexOf('INSERT INTO bots');
    assert.ok(at > 0, 'seed.sql: no INSERT INTO bots');
    const end = src.indexOf(';', at);
    assert.ok(end > at, 'seed.sql: unterminated INSERT INTO bots');
    const stmt = src.slice(at, end);
    const keys = new Set<string>();
    for (const m of stmt.matchAll(/\(\s*'[^']*'\s*,\s*'([^']+)'\s*\)/g)) keys.add(m[1]);
    return keys;
}

// STRAT_* (C) <-> STRAT.* (TS, wasm/bots.ts) name mapping. Both sides are
// checked against strategy.h so a renamed id fails loudly rather than silently
// comparing two different brains.
const C_TO_TS_STRAT: Record<string, string> = {
    STRAT_RANDOM: 'random',
    STRAT_SIMPLE_HEURISTIC: 'simple_heuristic',
    STRAT_HANDWRITTEN_PROD: 'handwritten',
    STRAT_ESPRESSO_PROD: 'espresso',
    STRAT_FIRECRACKER: 'firecracker',
    STRAT_BLACKPOWDER: 'blackpowder',
    STRAT_CORDITE: 'cordite',
    STRAT_OCTOGEN: 'octogen',
    // Offline-only rungs; the site has never registered these.
    STRAT_ROBUSTA: null as unknown as string,
    STRAT_GUNPOWDER: null as unknown as string,
};

test('C roster: table is well-formed and is the strength ladder', () => {
    const roster = parseCRoster();
    assert.ok(roster.length >= 10, `parsed only ${roster.length} roster rows — parser drifted?`);

    const keys = roster.map(r => r.key);
    assert.equal(new Set(keys).size, keys.length, 'roster keys must be unique');

    const tiers = roster.map(r => r.tier);
    assert.deepEqual(tiers, [...tiers].sort((a, b) => a - b), 'roster must be in tier order');
    assert.equal(new Set(tiers).size, tiers.length, 'tiers must be unique');

    // Every STRAT_* the table names must exist in the kernel's strategy.h.
    const strategyH = read('cnitro/src/strategy.h');
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
    const registry = parseTsRegistry();
    for (const gone of ['cordite_max', 'octogen_max', 'semtex_max']) {
        assert.ok(!registry.has(gone), `${gone} must not be registered in TS`);
    }
    assert.ok(!parseSeededKeys().has('cordite_max'), 'cordite_max must not be seeded');
    assert.ok(!parseSeededKeys().has('octogen_max'), 'octogen_max must not be seeded');
});

test('roster == TS registry, knob for knob', () => {
    const roster = parseCRoster();
    const registry = parseTsRegistry();

    for (const entry of roster) {
        const ts = registry.get(entry.key);
        const tsStrat = C_TO_TS_STRAT[entry.strat];
        if (tsStrat === undefined) throw new Error(`unmapped C strat ${entry.strat}`);

        // Offline-only rungs (robusta/gunpowder) have no TS registration, and
        // must not acquire one by accident.
        if (tsStrat === null) {
            assert.ok(!ts, `${entry.key} is offline-only but appears in the TS registry`);
            assert.ok(!entry.seeded, `${entry.key} is offline-only but marked seeded`);
            continue;
        }
        if (!ts) {
            // espresso is in the roster (an offline rung) but deliberately not
            // registered/seeded on the site.
            assert.ok(!entry.seeded, `${entry.key} is seeded but missing from the TS registry`);
            continue;
        }

        assert.equal(ts.strat, tsStrat, `${entry.key}: brain differs (C ${entry.strat} vs TS STRAT.${ts.strat})`);
        assert.equal(ts.logs, entry.usesLogs, `${entry.key}: uses_logs differs — one host would skip the belief log`);
        assert.deepEqual(ts.env, entry.knobs,
            `${entry.key}: knobs differ. The env table overrides the roster, so this IS a live ` +
            `strength/latency divergence between the site and the phone.`);
    }

    // No TS registration may name a key the roster does not know: the kernel
    // would have no entry to run it from once the env table is deleted.
    for (const key of registry.keys()) {
        const known = roster.some(r => r.key === key);
        // These predate the roster and are not dispatched by bots.wasm at all
        // (seed.sql's note); they fall back to random today and are slated for
        // deletion with the A7 cleanup.
        const legacyUndispatched = ['ultimate_champion', 'champion', 'hacker', 'fulminate', 'semtex'];
        if (!known && !legacyUndispatched.includes(key)) {
            assert.fail(`TS registers '${key}', which is not in the C roster`);
        }
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
