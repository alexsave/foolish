// The server runs on the C Table, not on a TypeScript Game
// (docs/C_GAME_SHAPE_MIGRATION.md Phases 4b and 8).
//
// Phase 8 deleted the TS game model: the game shape and its string tables
// (server/api/core/types.ts), the parity twins of the kernel's rules
// (server/api/common: game_lifecycle, pure_bot_actions, bot_strategy,
// common_utils, finish_order, actions/), the rules and guards embeds and the
// Game marshal (sdk/ts/wasm/engine.ts, rules_wasm.ts, guards_wasm.ts), the TS
// wire encoders (sdk/ts/wire view, roster, evwire, logwire) and the test helpers
// that handed a test the TS Game or PersonalGame (e2e/helpers/seeded_game.ts,
// view_game.ts). These checks keep them gone:
//
//   - the deleted modules stay deleted;
//   - the kernel bridges (sdk/ts/wasm/bots.ts) and the web-reader helper for
//     tests (e2e/helpers/client_read.ts) export none of the Game-taking or
//     PersonalGame-making names they used to;
//   - over the module graph every edge function loads (its entry points and
//     everything they reach through a static import, a re-export or a dynamic
//     import(), with the import map's aliases resolved as Deno resolves them),
//     no module resolves a seat from a player id in TypeScript.
//
// e2e/no_ts_game_shape.test.ts holds the rest: no TS declares the game's shape,
// and no TS lays out a kernel buffer by hand.

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
// Deleted in Phase 8. A path here that exists again is a TS game model coming back.
const DELETED = [
    'server/api/core/types.ts',
    'server/api/core/bot_interfaces.ts',
    'server/api/common/game_lifecycle.ts',
    'server/api/common/pure_bot_actions.ts',
    'server/api/common/bot_strategy.ts',
    'server/api/common/finish_order.ts',
    'server/api/common/common_utils.ts',
    'server/api/common/actions',
    'server/api/common/replay/encode.ts',
    'server/api/common/replay/decode_kernel.ts',
    'sdk/ts/wasm/engine.ts',
    'sdk/ts/wasm/rules_wasm.ts',
    'sdk/ts/wasm/guards_wasm.ts',
    'sdk/ts/wire/view.ts',
    'sdk/ts/wire/roster.ts',
    'sdk/ts/wire/evwire.ts',
    'sdk/ts/wire/logwire.ts',
    'e2e/helpers/seeded_game.ts',
    'e2e/helpers/view_game.ts',
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

test('the TS game model Phase 8 deleted stays deleted', () => {
    const back = DELETED.filter((f) => existsSync(join(REPO, f)));
    assert.deepEqual(back, [], `\n${back.join('\n')}\n`);
});

// Exports that took or made a TS Game or PersonalGame.
const GAME_TAKING = [
    'marshalGame', '__marshalGame', '__mem', '__setResident', '__adoptEngine', '__LOG_TYPE_TO_INT', '__wireLogCard',
    'execute', 'runKernel', 'stateToGame', 'applyStateToGame', 'applyKernelStateToGame', 'appendLogs',
    'materializeKernelGame', 'serializeGameState', 'deserializeGameState', 'runPackedAction', 'runPackedGameAction',
    'runPackedStart', 'runPackedRearrange', 'exportPackedDriveProducts', 'kernelHumanMask',
    'kernelValidateAttack', 'kernelValidateCover', 'kernelValidatePass', 'kernelValidatePickup', 'kernelValidateGood',
    'kernelAttack', 'kernelCover', 'kernelPass', 'kernelPickup', 'kernelGood', 'kernelStartGame',
    'kernelRoundTransition', 'kernelResetToLobby', 'kernelGameDone', 'kernelShouldAct', 'kernelNextPlayer',
    'kernelLegalMoves', 'wasmChooseMove', 'wasmChooseMoveDirect', 'importLogs', 'importLogsPacked', 'importStrategyKeys',
    'kernelReplayEncodeV6FromGame', 'wasmBotDrive', 'wasmBotEligibleMask', 'wasmViewFromGame',
    'decodeEnvelope', 'readPush', 'snapshotToGame', 'gameToView', 'playSeededV6',
];

test('the kernel bridge and the test reader export nothing that takes or makes a TS Game', () => {
    const offenders: string[] = [];
    for (const file of ['sdk/ts/wasm/bots.ts', 'e2e/helpers/client_read.ts']) {
        const src = readFileSync(join(REPO, file), 'utf8');
        for (const name of GAME_TAKING) {
            const declared = new RegExp(`\\bexport\\s+(?:async\\s+)?(?:function|const|let|class)\\s+${name}\\b`).test(src);
            const listed = [...src.matchAll(/\bexport\s*\{([^}]*)\}/g)]
                .some((m) => m[1].split(',').some((n) => n.trim().split(/\s+as\s+/).pop() === name));
            if (declared || listed) offenders.push(`${file} exports ${name}`);
        }
    }
    assert.deepEqual(offenders, [], `\n${offenders.join('\n')}\n`);
});
