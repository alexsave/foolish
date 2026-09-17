// Ad-hoc legal-move enumeration stack canary (R4,
// docs/RULES_GUARDS_WASM_MEMORY_PLAN.md; NOT part of the suite - no .test.ts).
// The enumeration twin of e2e/stack_canary.mts: paints the shadow stack, drives
// worst-case cover and attack-combination enumeration, and reports the
// high-water. It used to run on rules.wasm through the TS Game marshal; every
// host now enumerates in bots.wasm (the server's table, the web's client slot),
// so it measures bots.wasm's 22 KiB stack, on boards the kernel itself sealed
// (e2e/helpers/table_fixture.ts: every board here is one game_validate accepts,
// so the sweep is bounded by what a real row can hold) through the kernel's own
// enumerator (wasm_legal_moves). Run:
//   TSX_TSCONFIG_PATH=e2e/tsconfig.json node --import tsx e2e/rules_stack_canary.mts
// The rules.wasm measurement was 14.3 KiB (cover nb=8), against a 32 KiB stack.
import { fixture, fixtureExports, fixtureTable, PLAYING, OUT } from './helpers/table_fixture.ts';
import * as L from '../sdk/ts/gen/game_layout.bots.ts';

const STACK = Number(process.env.STACK_SIZE ?? 22528);   // bots -z stack-size (Makefile WASM_BOT_LDFLAGS)
const RANKS = '23456789TJQKA';
const SUITS = 'cdhs';
const card = (suit: number, rank: number) => `${RANKS[rank]}${SUITS[suit]}`;

// Six seats: the 52-card deck, so a defender can hold as many cards as a game can.
const SEATS = Array.from({ length: 6 }, (_, i) => ({ id: `p${i}`, name: `P${i}` }));

/** Loads a sealed 6-seat board; seats 2..5 are out, seat 0 attacks, seat 1 defends. */
function load(hands: string[][], battles: string[], trump: string): boolean {
    let b = fixture().seats(SEATS).status(PLAYING).attacker(0).defender(1).trump(trump).table(...battles)
        .eliminated(2, 3, 4, 5);
    for (let s = 2; s < 6; s++) b = b.seatStatus(s, OUT);
    hands.forEach((h, s) => { b = b.hand(s, h.join(' ')); });
    try {
        const fx = b.build();
        return fixtureTable().load(fx.state, fx.roster) === L.TABLE_OK;
    } catch {
        return false;   // a board no game can hold: not part of the sweep
    }
}

const ex = fixtureExports();
fixtureTable();   // instantiate
const paint = () => new Uint8Array(ex.memory.buffer).fill(0xA5, 64, STACK - 64);
const scan = () => {
    const u = new Uint8Array(ex.memory.buffer);
    let low = 64;
    while (low < STACK - 64 && u[low] === 0xA5) low++;
    return STACK - low;   // high-water bytes
};

let worst = 0, worstDesc = '', boards = 0, refused = 0;
paint();

// Cover sweep: `nb` uncovered low attacks (distinct cards), the defender holding
// up to `dh` higher cards across every suit, the trump the last spade.
for (const nb of [1, 2, 3, 4, 5, 6, 7, 8]) {
    const attacks = Array.from({ length: nb }, (_, i) => card(i % 3, Math.floor(i / 3)));
    for (const dh of [6, 12, 18, 24, 30, 36, 40]) {
        const hand: string[] = [];
        for (let r = 12; r >= 0 && hand.length < dh; r--) {
            for (let s = 0; s < 4 && hand.length < dh; s++) {
                const c = card(s, r);
                if (!attacks.includes(c) && c !== 'Ks' && c !== '2s') hand.push(c);
            }
        }
        if (!load([['2s'], hand], attacks, 'Ks')) { refused++; continue; }
        ex.wasm_legal_moves(1);
        boards++;
    }
    const hw = scan();
    if (hw > worst) { worst = hw; worstDesc = `cover nb=${nb}`; }
}

// Attack-combination sweep (first attack and throw-ins): a wide attacker hand of
// every value, with `nc` covered battles on the table naming values it holds.
for (const hs of [6, 12, 18, 24, 30, 36, 40]) {
    for (const nc of [0, 1, 2, 3, 4]) {
        const battles = Array.from({ length: nc }, (_, i) => `${card(3, i)}/${card(3, i + 6)}`);
        const used = new Set(battles.flatMap((b) => b.split('/')).concat(['As', '3c']));
        const hand: string[] = [];
        for (let r = 0; r < 13 && hand.length < hs; r++) {
            for (let s = 0; s < 3 && hand.length < hs; s++) if (!used.has(card(s, r))) hand.push(card(s, r));
        }
        if (!load([hand, ['3c']], battles, 'As')) { refused++; continue; }
        ex.wasm_legal_moves(0);
        boards++;
    }
    const hw = scan();
    if (hw > worst) { worst = hw; worstDesc = `attack hs=${hs}`; }
}

console.log(`boards enumerated: ${boards} (${refused} refused by the kernel as boards no game can hold)`);
console.log(`enumeration stack high-water: ${worst} B (${(worst / 1024).toFixed(1)} KiB) at ${worstDesc}`);
console.log(`stack=${STACK} (${(STACK / 1024).toFixed(1)} KiB); headroom = ${((STACK - worst) / 1024).toFixed(1)} KiB; ratio = ${(STACK / worst).toFixed(2)}x`);
