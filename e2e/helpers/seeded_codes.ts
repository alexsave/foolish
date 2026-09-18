// seeded_codes.ts - finished games whose step indices and renders tests pin.
//
// These four v6 codes were cut from TypeScript Games played by the `handwritten`
// bot on deal seeds seedBytes(np, s) (np:s = 2:7, 3:41, 4:42, 8:43), the way the
// retired e2e/helpers/seeded_game.ts played them, before the TS Game was deleted
// (docs/C_GAME_SHAPE_MIGRATION.md Phase 8). They are frozen rather than re-played
// on the C Table because what is built on them names those exact games:
// oracle_mode_b pins its Monte-Carlo and exact-endgame decisions by step index,
// and the match history DOM golden shows their fools and move counts. A C Table
// game on the same seed plays differently (the kernel's bot cycle draws its own
// stream). A suite that only needs some finished game plays one instead
// (e2e/helpers/bot_table.ts playBotTable), which cannot go stale.
//
// A replay code is only readable by a kernel with the same legal-move menu. If a
// kernel change stops one of these reading as a finished game, seededCode throws
// with that reason, loudly rather than as a vacuous pass: re-cut the game with
// playBotTable, sweep oracle_mode_b for new indices of the same shape, and
// re-record the match history golden.

import { readFileSync } from 'node:fs';
import { replaySummary } from '../../sdk/ts/wasm/bots.ts';

const FIXTURE = new URL('../fixtures/seeded_games/codes.json', import.meta.url);
const codes = JSON.parse(readFileSync(FIXTURE, 'utf8')) as Record<string, { np: number; seed: number; code: string }>;

/** The finished `np`-seat game on seed `s`, as its v6 replay code. */
export function seededCode(np: number, s: number): Uint8Array {
    const row = codes[`${np}:${s}`];
    if (!row) throw new Error(`seeded_codes: no frozen game ${np}:${s}`);
    const code = new Uint8Array(Buffer.from(row.code, 'hex'));
    const summary = replaySummary(code);
    if (!summary || summary.numPlayers !== np || summary.fool < 0) {
        throw new Error(`seeded_codes: the frozen ${np}:${s} game no longer reads as a finished game under this kernel (see the header)`);
    }
    return code;
}
