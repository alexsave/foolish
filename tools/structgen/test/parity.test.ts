// Parity: the hand-written byte-wire marshal (legacy_marshal.ts, verbatim HEAD)
// vs the generated in-place marshal (game_marshal.ts), on one kernel module
// that carries both paths (test/kernel.c, built by test/harness.sh).
//   import: legacy bytes -> state_get   vs  marshal -> k_adopt     => identical Game bytes
//   export: state_put + legacy parse    vs  readState in place     => deepEqual
// States: every position of real random-bot games at 2..8 players, hostile
// mutations of them, states the kernel itself produced, and random raw bytes.
// Run: bash tools/structgen/test/harness.sh &&
//      TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx --test tools/structgen/test/parity.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { game_done } from '../../../server/api/common/common_utils.ts';
import { start_game } from '../../../server/api/common/game_lifecycle.ts';
import { Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS } from '../../../server/api/core/types.ts';
import { shouldBotActCore, processBotAction } from '../../../server/api/common/pure_bot_actions.ts';
import { calculateLegalMoves } from '../../../server/api/common/bot_strategy.ts';
import * as GL from '../build/harness/game_layout.ts';
import { type Kernel, legacyMarshal, legacyParse } from './legacy_marshal.ts';
import { marshal, readState } from './game_marshal.ts';

if (!process.env.E2E_VERBOSE) { console.log = () => {}; console.warn = () => {}; console.info = () => {}; }

const ex = new WebAssembly.Instance(new WebAssembly.Module(readFileSync(new URL('../build/harness/kernel.wasm', import.meta.url))), {}).exports as unknown as Kernel;
const u8 = () => new Uint8Array(ex.memory.buffer);
const G = ex.k_game();
const PREFIX = GL.Game_logs_at(0, 0);   // everything before the log array

// ---- state sources -------------------------------------------------------------
let seed = 0x5eed1234;
const rnd = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 4294967296; };
const ri = (n: number) => Math.floor(rnd() * n);
const mkGame = (np: number): Game => ({
    players: Array.from({ length: np }, (_, i) => ({ player_id: `bot_${i}`, name: `Bot ${i}`, status: PLAYER_STATUS.READY,
        is_ai: i % 3 !== 1, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: 'random' } as PrivatePlayer)),
    deck: [], logs: [], id: 'parity', name: 'parity', status: GAME_STATUS.PLAYING, deck_length: 0, discard_pile_length: 0,
    flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [],
} as unknown as Game);

async function realStates(): Promise<Game[]> {
    const out: Game[] = [];
    for (let np = 2; np <= 8; np++) for (let n = 0; n < 6; n++) {
        const g = mkGame(np);
        start_game(g);
        out.push(structuredClone(g));
        for (let guard = 0; game_done(g) === null && guard < 2000; guard++) {
            const eligible = g.players.filter((p, i) => shouldBotActCore(g, p, i) && calculateLegalMoves(g, p.player_id).length > 0);
            let acted = false;
            for (const p of eligible) if (await processBotAction(g, p)) { acted = true; break; }
            if (!acted) break;
            out.push(structuredClone(g));
        }
    }
    return out;
}
const weirdCard = () => [{ suit: -1, value: -1 }, { suit: 9, value: 40 }, { suit: -7, value: 0 }, { suit: NaN, value: 3 },
    { suit: 2.7, value: 11.2 }, { suit: 300, value: -300 }][ri(6)];
function hostile(g0: Game): Game {
    const g = structuredClone(g0) as any;
    const k = ri(10);
    // Counts stay within the kernel caps: past a cap the OLD wire desyncs (C stops
    // reading an array mid-stream and parses the rest from the wrong bytes), so
    // there is no legacy behaviour worth matching. Over-cap is its own test below.
    if (k === 0) g.deck.push(...Array.from({ length: ri(65 - g.deck.length) }, weirdCard));
    if (k === 1) g.players.forEach((p: any) => p.hand.push(...Array.from({ length: ri(65 - p.hand.length) }, weirdCard)));
    if (k === 2) { g.power_suit = NaN; g.first_attacker = 1000; g.defender = -129; g.discard_pile_length = 70000; }
    if (k === 3) g.table_battles.push(...Array.from({ length: ri(65 - g.table_battles.length) }, () => ({ attack: weirdCard(), defense: rnd() < 0.5 ? null : weirdCard() })));
    if (k === 4) { g.elimination_order = ['nobody', g.players[0].player_id, 'x', 'y', 'z', 'q', 'r', 's'].slice(0, 1 + ri(8)); g.good_players = g.players.map((p: any) => p.player_id); }
    if (k === 5) { g.status = 'bogus'; g.players.forEach((p: any) => { p.status = 'bogus'; p.awaiting_attack = 1; }); }
    if (k === 6) { while (g.players.length < 8) g.players.push({ ...g.players[0], player_id: `extra${g.players.length}`, hand: [weirdCard()] }); }
    if (k === 7) g.flipped = weirdCard();
    if (k === 8) g.good_timestamp = 0;
    if (k === 9) g.deterministic_deck = true;
    return g;
}

function prefix(): Uint8Array { return u8().slice(G, G + PREFIX); }
function zero(): void { u8().fill(0, G, G + PREFIX); }
function checkImport(game: Game): void {
    zero(); legacyMarshal(ex, game); const a = prefix(); const ha = ex.k_human_mask();
    zero(); marshal(ex, game); const b = prefix(); const hb = ex.k_human_mask();
    if (Buffer.compare(a, b) !== 0) {
        const at = a.findIndex((x, i) => x !== b[i]);
        assert.fail(`g_game prefix differs at byte ${at}: legacy ${a[at]} new ${b[at]} (players=${game.players.length})`);
    }
    assert.equal(hb, ha);
}
function checkExport(where: string): void {
    assert.deepEqual(readState(ex, G), legacyParse(ex), `${where}: state`);
}

test('parity: import and export over real games, hostile games and random kernel bytes', async () => {
    const states = await realStates();
    let imports = 0, exports = 0, raw = 0;
    for (const s of states) {
        for (const g of [s, hostile(s), hostile(s)]) {
            checkImport(g); imports++;
            checkExport('imported'); exports++;
            const def = GL.Game_get_defender(GL.memOf(ex.memory.buffer), G);
            if (def >= 0 && def < g.players.length && ex.k_pickup(def)) { ex.k_transition(); ex.k_refill(); checkExport('after kernel pickup + transition + refill'); exports++; }
        }
    }
    // Random bytes in every field, counts kept in range (put_state trusts them).
    const m = GL.memOf(ex.memory.buffer);
    for (let n = 0; n < 3000; n++) {
        const bytes = u8();
        for (let i = 0; i < PREFIX; i++) bytes[G + i] = ri(256);
        GL.Game_set_num_players(m, G, ri(9)); GL.Game_set_deck_count(m, G, ri(65));
        GL.Game_set_num_battles(m, G, ri(65)); GL.Game_set_num_eliminated(m, G, ri(9));
        // _Bool bytes other than 0/1 are undefined behaviour in C (clang tests the
        // low bit, the accessor tests != 0); the kernel never stores one.
        GL.Game_set_has_flipped(m, G, rnd() < 0.5); GL.Game_set_has_good_timestamp(m, G, rnd() < 0.5);
        for (let i = 0; i < 8; i++) {
            GL.Player_set_hand_count(m, GL.Game_players_at(G, i), ri(65));
            GL.Player_set_awaiting_attack(m, GL.Game_players_at(G, i), rnd() < 0.5);
        }
        assert.deepEqual(readState(ex, G), legacyParse(ex), 'random kernel bytes');
        raw++;
    }
    process.stderr.write(`parity: ${states.length} real states; ${imports} imports, ${exports} exports, ${raw} random-byte exports - all identical\n`);
});

test('over-cap games: the in-place marshal clamps and writes nothing outside the Game prefix', () => {
    const g = mkGame(8) as any;
    g.players.push(...Array.from({ length: 5 }, (_, i) => ({ ...g.players[0], player_id: `x${i}` })));
    g.deck = Array.from({ length: 500 }, weirdCard);
    g.table_battles = Array.from({ length: 300 }, () => ({ attack: weirdCard(), defense: null }));
    g.players.forEach((p: any) => { p.hand = Array.from({ length: 200 }, weirdCard); });
    g.elimination_order = Array.from({ length: 40 }, (_, i) => `x${i}`);
    const before = u8().slice(), end = G + PREFIX;
    marshal(ex, g);
    const after = u8();
    for (let i = 0; i < after.length; i++) if ((i < G || i >= end) && after[i] !== before[i]) {
        assert.fail(`byte ${i} outside the Game prefix changed`);
    }
    const m = GL.memOf(ex.memory.buffer);
    assert.deepEqual([GL.Game_get_num_players(m, G), GL.Game_get_deck_count(m, G), GL.Game_get_num_battles(m, G), GL.Game_get_num_eliminated(m, G)],
        [8, GL.Game_deck_LEN, 300 & 0xff, 8]);   // counts wrap to the C width, then clamp - as the byte wire did
});
