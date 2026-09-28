// Ad-hoc harness (not part of the suite): bot decision latency through the
// production path - the kernel's bot cycle (table_bot_drive) on a table of bots,
// exactly as the server's loop runs it (e2e/helpers/bot_table.ts) - one action
// per cycle, from pinned deal seeds so runs are comparable across builds.
// Reports per-brain decision timing over full games.
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/perf_harness.mts
import { botCycle, dealBotTable, seedBytes } from './helpers/bot_table.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; }
const report = console.error.bind(console); // survives the console.log gag

const GAMES = Number(process.env.PERF_GAMES ?? '6');
const stats: Record<string, { n: number; ms: number; max: number }> = {};

for (const brains of [['cordite', 'octogen'], ['firecracker', 'blackpowder']]) {
    for (let gi = 0; gi < GAMES; gi++) {
        let row = dealBotTable(brains, seedBytes(brains.length, 0xDEA1 ^ gi), { gameId: `perf${gi}` });
        for (let guard = 0; row.status === L.GAME_STATUS_PLAYING && guard < 4000; guard++) {
            const c = botCycle(row, { maxActions: 1 });
            if (c.drive.n === 0) break;
            const brain = brains[c.drive.seats[0]];
            const s = (stats[brain] ??= { n: 0, ms: 0, max: 0 });
            s.n++; s.ms += c.ms; if (c.ms > s.max) s.max = c.ms;
            row = c.row;
        }
        if (row.status === L.GAME_STATUS_PLAYING) report(`perf${gi} ${brains.join(' vs ')}: did not finish`);
    }
}

for (const [k, s] of Object.entries(stats)) {
    report(`${k.padEnd(12)} decisions=${s.n} avg=${(s.ms / s.n).toFixed(1)}ms max=${s.max.toFixed(0)}ms total=${(s.ms / 1000).toFixed(1)}s`);
}
