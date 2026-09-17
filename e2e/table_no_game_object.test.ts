// The server runs on the C Table, not on a TypeScript Game
// (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b).
//
// A static check over the module graph every edge function actually loads: the
// entry points server/impls/supabase/functions/<name>/index.ts, and every module
// they reach through a static import, a re-export or a dynamic import(), with
// the import map's aliases (@shared, @api, @sdk) resolved the way Deno resolves
// them. Nothing in that graph may import the TS game shape from
// server/api/core/types.ts (Game, PublicGame, PersonalGame, PrivatePlayer), or
// reach one of the TS twins Phase 4b retired.
//
// The graph, not a directory: server/api/core/types.ts and a few server/api
// modules are still imported by the web client (src/) until Phases 6-8 retire
// them, so "no file under server/ mentions Game" cannot hold yet. What must hold
// now is that no request the server handles can touch one.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';

const REPO = resolve(import.meta.dirname, '..');
const FUNCTIONS = join(REPO, 'server/impls/supabase/functions');
const ALIASES: Record<string, string> = {
    '@shared/': join(FUNCTIONS, '_shared/'),
    '@api/': join(REPO, 'server/api/'),
    '@sdk/': join(REPO, 'sdk/'),
};
const TYPES = join(REPO, 'server/api/core/types.ts');
const GAME_SHAPE = ['Game', 'PublicGame', 'PersonalGame', 'PrivatePlayer'];

// The TS twins of the kernel's game that the server stopped using.
// Only files that still exist: a deleted twin cannot be reached, and its entry
// goes with it (the last test below holds the list to that).
const RETIRED = [
    'server/api/common/game_lifecycle.ts',
    'server/api/common/pure_bot_actions.ts',
    'server/api/common/bot_strategy.ts',
    'server/api/common/finish_order.ts',
    'server/api/common/common_utils.ts',
    'server/api/common/actions/',
    'sdk/ts/wasm/engine.ts',
    'sdk/ts/wire/view.ts',
    'sdk/ts/wire/roster.ts',
    'sdk/ts/wire/evwire.ts',
    'sdk/ts/wire/logwire.ts',
];

const entries = readdirSync(FUNCTIONS)
    .map((d) => join(FUNCTIONS, d, 'index.ts'))
    .filter((p) => existsSync(p));

// Every specifier: static imports and re-exports, and dynamic import('...').
const SPEC = /(?:\bfrom\s*|\bimport\s*\(\s*|\bimport\s+)['"]([^'"]+)['"]/g;

function resolveSpec(from: string, spec: string): string | null {
    let target: string | null = null;
    for (const [alias, dir] of Object.entries(ALIASES)) {
        if (spec.startsWith(alias)) target = join(dir, spec.slice(alias.length));
    }
    if (!target && spec.startsWith('.')) target = resolve(dirname(from), spec);
    if (!target) return null;   // jsr:, npm:, https: - not the repo's code
    for (const candidate of [target, `${target}.ts`, join(target, 'index.ts')]) {
        if (existsSync(candidate) && statSync(candidate).isFile()) return candidate;
    }
    return null;
}

function graph(): Map<string, string> {
    const seen = new Map<string, string>();   // module -> who reached it first
    const stack: [string, string][] = entries.map((e) => [e, 'entry']);
    while (stack.length > 0) {
        const [file, via] = stack.pop()!;
        if (seen.has(file)) continue;
        seen.set(file, via);
        if (!/\.(ts|mts|tsx)$/.test(file)) continue;
        const src = readFileSync(file, 'utf8');
        for (const m of src.matchAll(SPEC)) {
            const target = resolveSpec(file, m[1]);
            if (target && !seen.has(target)) stack.push([target, relative(REPO, file)]);
        }
    }
    return seen;
}

test('the edge entry points exist (the graph is not vacuously empty)', () => {
    const names = entries.map((e) => relative(FUNCTIONS, e)).sort();
    for (const fn of ['action/index.ts', 'meta/index.ts', 'create/index.ts', 'bot-heartbeat/index.ts', 'delete-account/index.ts']) {
        assert.ok(names.includes(fn), `entry point ${fn}`);
    }
    assert.ok(graph().size > entries.length, 'the entry points import something');
});

test('no module the server loads imports the TS game shape (Game, PublicGame, PersonalGame, PrivatePlayer)', () => {
    const offenders: string[] = [];
    for (const [file] of graph()) {
        if (file === TYPES || !/\.(ts|mts|tsx)$/.test(file)) continue;
        const src = readFileSync(file, 'utf8');
        const IMPORT = /\bimport\s+(type\s+)?\{([^}]*)\}\s*from\s*['"]([^'"]+)['"]/g;
        for (const m of src.matchAll(IMPORT)) {
            if (resolveSpec(file, m[3]) !== TYPES) continue;
            const names = m[2].split(',').map((n) => n.trim().replace(/^type\s+/, '').split(/\s+as\s+/)[0]);
            const hit = names.filter((n) => GAME_SHAPE.includes(n));
            if (hit.length > 0) offenders.push(`${relative(REPO, file)} imports ${hit.join(', ')}`);
        }
    }
    assert.deepEqual(offenders, [], `\n${offenders.join('\n')}\n`);
});

test('no module the server loads resolves a seat from an id in TypeScript (the kernel\'s table_seat_of does)', () => {
    // A seat lookup is a scan of a roster's ids for a match: findIndex / indexOf
    // / find over something compared to an id. The kernel answers it
    // (roster_seat_of through ServerTable.seatOf); a TS copy is the rule twice.
    const LOOKUP = /\.(findIndex|indexOf|find)\s*\(\s*\(?\s*(\w+)[^)]*\)?\s*=>\s*\w*\.?\b(id|player_id|user_id)\s*===|\.(findIndex|indexOf)\s*\([^)]*\b(id|player_id|user_id)\b/g;
    const offenders: string[] = [];
    for (const [file] of graph()) {
        if (!/\.(ts|mts|tsx)$/.test(file)) continue;
        const src = readFileSync(file, 'utf8');
        for (const m of src.matchAll(LOOKUP)) {
            const line = src.slice(0, m.index).split('\n').length;
            offenders.push(`${relative(REPO, file)}:${line}: ${m[0]}`);
        }
    }
    assert.deepEqual(offenders, [], `\n${offenders.join('\n')}\n`);
    const wrapper = readFileSync(join(REPO, 'sdk/ts/table/server_table.ts'), 'utf8');
    assert.match(wrapper, /seatOf\([^)]*\)[^{]*\{[^}]*wasm_table_seat_of/, 'ServerTable.seatOf is the kernel call');
});

test('no module the server loads is one of the TS twins Phase 4b retired', () => {
    const g = graph();
    const reached = [...g.keys()]
        .map((f) => relative(REPO, f))
        .filter((f) => RETIRED.some((r) => (r.endsWith('/') ? f.startsWith(r) : f === r)))
        .map((f) => `${f} (reached from ${g.get(join(REPO, f))})`);
    assert.deepEqual(reached, [], `\n${reached.join('\n')}\n`);
});

test('the retired list names only files that still exist', () => {
    // An entry for a deleted file guards nothing and reads as if the twin were
    // still around; when a twin is deleted, its entry is deleted with it.
    const gone = RETIRED.filter((r) => !existsSync(join(REPO, r)));
    assert.deepEqual(gone, [], `deleted, so drop them from RETIRED: ${gone.join(', ')}`);
});
