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
//   - the TypeScript server stays deleted (stage 4 of c_base/DESIGN.md "Binary
//     surface": the server is C, and its seat lookup is the kernel's
//     table_seat_of there), and the tests' C Table driver resolves a seat
//     through the kernel too.
//
// e2e/no_ts_game_shape.test.ts holds the rest: no TS declares the game's shape,
// and no TS lays out a kernel buffer by hand.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const REPO = resolve(import.meta.dirname, '..');
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
    // Stage 4: the TypeScript server, with its edge functions and its adapter.
    'server/impls/supabase',
];

test('the tests\' C Table driver resolves a seat through the kernel (table_seat_of)', () => {
    const wrapper = readFileSync(join(REPO, 'e2e/helpers/server_table.ts'), 'utf8');
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
