// Cold-start simulation: one fresh process = one cold Deno edge invocation that
// must LOAD the bots module (inflate the gz -> compile -> instantiate) and make
// ONE cordite decision. argv[2] = raw .wasm path (for isolated compile timing).
// Reports: compile-only ms (scales with module size), and end-to-end cold
// first-decision ms the way the server's bot loop reaches it (createServerTable,
// then a six-seat cordite table dealt by table ops, then one table_bot_drive).
// This is the metric that decides O3-vs-Oz for edge.
//
// Phase 8 (docs/C_GAME_SHAPE_MIGRATION.md) moved it off the TypeScript Game and
// wasmChooseMoveDirect (base64 embed, JS marshal): the table is built and driven
// in C, so the cold figure is the server's own path.
import { readFileSync } from 'node:fs';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';
import { createServerTable, type ServerTable } from '../sdk/ts/table/server_table.ts';
const __log = console.log.bind(console); console.log = () => {}; console.warn = () => {};

const SEATS = 6;
const SEED = Uint8Array.from({ length: 32 }, (_, i) => (i * 13 + 0xde) & 0xff);
const SEED_HEX = Array.from(SEED, (b) => b.toString(16).padStart(2, '0')).join('');

function must(what: string, rc: number): void {
  if (rc !== L.TABLE_OK) throw new Error(`cold_start: ${what} refused (${rc})`);
}

/** Six cordite bots dealt from SEED, entirely by table ops on `table`: create, add the bots, the host leaves, a bot readies. */
function dealCordite(table: ServerTable): void {
  must('create', table.create('host', 'Host'));
  for (let b = 0; b < SEATS; b++) must(`add bot ${b}`, table.addBot('host', `bot-${b}`, `P${b + 1}`, 'cordite', SEED));
  must('leave', table.leave('host', 'host'));
  must('ready', table.ready('bot-0', SEED));
  must('deal seed', table.setDealSeed(SEED_HEX));
}

// (a) isolated cold compile cost (pure size effect)
const bytes = readFileSync(process.argv[2]);
const c0 = process.hrtime.bigint();
new WebAssembly.Module(bytes);
const compileMs = Number(process.hrtime.bigint() - c0) / 1e6;

// (b) end-to-end cold first decision on the server's path
const e0 = process.hrtime.bigint();
const table = createServerTable();   // inflate + compile + instantiate + init
dealCordite(table);
const first = table.botDrive(null, 1);
const coldMs = Number(process.hrtime.bigint() - e0) / 1e6;
if (typeof first === 'number' || first.n !== 1) throw new Error(`cold_start: the first decision was not made (${JSON.stringify(first)})`);

// warm: 10 more decisions on the same (now-loaded, tiering-up) module
let warmNs = 0, wc = 0;
for (let d = 0; d < 10; d++) {
  const s = process.hrtime.bigint();
  const r = table.botDrive(null, 1);
  warmNs += Number(process.hrtime.bigint() - s);
  if (typeof r === 'number' || r.n === 0) break;
  wc++;
  if (r.stop === L.BOT_STOP_ENDED) break;
}
const warmMs = wc ? warmNs / wc / 1e6 : 0;
__log('COLD ' + JSON.stringify({ compileMs: +compileMs.toFixed(2), coldMs: +coldMs.toFixed(2), warmMs: +warmMs.toFixed(2), mem: Math.round(table.memoryBytes() / 1048576) }));
