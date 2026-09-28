// Generated-accessor performance gate. Prints ns/op (median of RUNS x N calls):
//   * every emitted accessor shape in isolation, plus candidate shapes
//     (alt_accessors.ts) the generator could emit instead;
//   * a pointer followed by the generated checked X_f_deref_at vs a raw u32 read.
// It runs on the bots kernel's test build (c/build/bots_test.wasm: the shipped
// module's objects and exports plus the test-only ones, among them the resident
// deal this bench starts from) and its generated module
// (sdk/ts/gen/game_layout.bots.ts), over a Game the kernel dealt.
// The hand-written byte-wire marshal it once raced is gone with the TS game shape
// (docs/C_GAME_SHAPE_MIGRATION.md Phase 8); e2e/bench_decode_packed.ts measures
// the envelope read that replaced it.
// Plain TS with explicit .ts imports: runs under tsx, bundled, and on node's own
// type stripping. See test/bench.sh for all three.
import { readFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import * as L from '../../../sdk/ts/gen/game_layout.bots.ts';
import {
    type Mem, memOf, Game_get_status, Game_set_status, Game_get_deck_count, Game_set_deck_count, Game_set_num_players,
    Game_get_good_players_mask, Game_set_good_players_mask, Game_players_at, Player_hand_at, Player_set_status,
    Game_get_elimination_order, Game_set_elimination_order, Card_get_suit, Card_set_suit, Card_get_value, Card_set_value,
    Card_raw_get, Card_raw_set, Card_pack, Roster_seats_at, RosterSeat_get_name_str, RosterSeat_set_name_str,
    GAME_STATUS_WAITING, PLAYER_STATUS_READY,
} from '../../../sdk/ts/gen/game_layout.bots.ts';
import * as A from './alt_accessors.ts';
import * as Anim from '../../../sdk/ts/gen/anim.bots.ts';

interface Kernel {
    memory: WebAssembly.Memory;
    wasm_init(): void; wasm_set_seed(s: number): void; wasm_start_game(): number;
    wasm_game_ptr_internal(): number; wasm_table_roster_ptr(): number;
}
const wasmPath = process.env.BENCH_WASM ?? new URL('../../../c/build/bots_test.wasm', import.meta.url);
const wasmBytes = readFileSync(wasmPath);
const ex = new WebAssembly.Instance(new WebAssembly.Module(String(wasmPath).endsWith('.gz') ? gunzipSync(wasmBytes) : wasmBytes), {}).exports as unknown as Kernel;
const N = Number(process.env.BENCH_N ?? 200000), RUNS = Number(process.env.BENCH_RUNS ?? 7);
const MODE = process.env.BENCH_MODE ?? 'unknown';
const m: Mem = memOf(ex.memory.buffer), mx = A.memXOf(ex.memory.buffer);
const g = ex.wasm_game_ptr_internal(), pl = Game_players_at(g, 1);
const seat = Roster_seats_at(ex.wasm_table_roster_ptr(), 1);
let sink = 0, obj: unknown = null;

// A 4-player game the kernel dealt, so the rows read and write a real board.
ex.wasm_init();
ex.wasm_set_seed(0x5eed);
Game_set_status(m, g, GAME_STATUS_WAITING);
Game_set_num_players(m, g, 4);
for (let i = 0; i < 4; i++) Player_set_status(m, Game_players_at(g, i), PLAYER_STATUS_READY);
ex.wasm_start_game();

function median(fn: () => void): number {
    for (let i = 0; i < 20000; i++) fn();
    const t: number[] = [];
    for (let r = 0; r < RUNS; r++) {
        const t0 = process.hrtime.bigint();
        for (let i = 0; i < N; i++) fn();
        t.push(Number(process.hrtime.bigint() - t0) / N);
    }
    return t.sort((a, b) => a - b)[RUNS >> 1];
}
// BENCH_ONLY=i runs just row i, so each row can get a fresh process (no JIT
// state carried over from the rows before it); BENCH_LIST prints the names.
const rows: [string, number][] = [];
let rowIndex = 0;
const ONLY = process.env.BENCH_ONLY === undefined ? -1 : Number(process.env.BENCH_ONLY);
const row = (name: string, fn: () => void) => {
    const i = rowIndex++;
    if (process.env.BENCH_LIST) { process.stdout.write(`${i}\t${name}\n`); return; }
    if (ONLY < 0 || ONLY === i) rows.push([name, median(fn)]);
};

const handBytes = new Uint8Array(64).map((_, i) => Card_pack(i % 4, (i % 13) + 1));
let k = 0;
// ---- shapes in isolation -----------------------------------------------------------
row('i8 get+set (Game.status)', () => { Game_set_status(m, g, (k++) & 1); sink += Game_get_status(m, g); });
row('i8 get+set via namespace import', () => { L.Game_set_status(m, g, (k++) & 1); sink += L.Game_get_status(m, g); });
row('i16 get+set DataView (deck_count)', () => { Game_set_deck_count(m, g, (k++) & 31); sink += Game_get_deck_count(m, g); });
row('i16 get+set Int16Array view [alt]', () => { A.ta_set_deck_count(mx, g, (k++) & 31); sink += A.ta_get_deck_count(mx, g); });
row('u32 get+set DataView (good mask)', () => { Game_set_good_players_mask(m, g, (k++) & 15); sink += Game_get_good_players_mask(m, g); });
row('u32 get+set Uint32Array view [alt]', () => { A.ta_set_good_mask(mx, g, (k++) & 15); sink += A.ta_get_good_mask(mx, g); });
row('bitfield card write: 2x read-modify-write', () => { const a = Player_hand_at(pl, (k++) & 7); Card_set_suit(m, a, 2); Card_set_value(m, a, 9); });
row('bitfield card write: raw_set(pack)', () => { Card_raw_set(m, Player_hand_at(pl, (k++) & 7), Card_pack(2, 9)); });
row('bitfield card read: 2 getters', () => { const a = Player_hand_at(pl, (k++) & 7); sink += Card_get_suit(m, a) + Card_get_value(m, a); });
row('bitfield card read: raw_get', () => { sink += Card_raw_get(m, Player_hand_at(pl, (k++) & 7)); });
row('nested _at address (players[i].hand[j])', () => { sink += Player_hand_at(Game_players_at(g, (k++) & 7), 5); });
row('array element get+set (elimination_order)', () => { Game_set_elimination_order(m, g, (k++) & 7, 3); sink += Game_get_elimination_order(m, g, 2); });
row('hand of 8 cards: 8x raw_set', () => { for (let j = 0; j < 8; j++) Card_raw_set(m, Player_hand_at(pl, j), handBytes[j]); });
row('hand of 8 cards: one u8.set [alt]', () => { A.hand_set_bytes(mx, pl, handBytes, 8); });
row('string set+get (RosterSeat.name, 10 chars)', () => { RosterSeat_set_name_str(m, seat, 'Player 123'); obj = RosterSeat_get_name_str(m, seat); });
// ---- pointers: the checked follow against a raw u32 read ------------------------------
// An AnimEvent (sdk/ts/gen/anim.bots.ts) at the top of the kernel's memory, which
// nothing else writes, whose `const Card *cards` points at 8 cards just below it.
const ev = ex.memory.buffer.byteLength - 64, evCards = ev - 8;
m.dv.setUint32(ev + 16, evCards, true);
for (let j = 0; j < 8; j++) Card_raw_set(m, evCards + j, handBytes[j]);
row('pointer: raw u32 read + i [baseline]', () => { sink += m.dv.getUint32(ev + 16, true) + ((k++) & 7); });
row('pointer: _ptr + i', () => { sink += Anim.AnimEvent_cards_ptr(m, ev) + ((k++) & 7); });
row('pointer: _deref_at (NULL + bounds check)', () => { sink += Anim.AnimEvent_cards_deref_at(m, ev, (k++) & 7); });
row('pointer: raw u32 read + i, Card_raw_get [baseline]', () => { sink += Card_raw_get(m, m.dv.getUint32(ev + 16, true) + ((k++) & 7)); });
row('pointer: _deref_at, Card_raw_get', () => { sink += Card_raw_get(m, Anim.AnimEvent_cards_deref_at(m, ev, (k++) & 7)); });
void sink; void obj;

for (const [name, ns] of rows) process.stdout.write(`${MODE}\t${name}\t${ns.toFixed(1)}\n`);
