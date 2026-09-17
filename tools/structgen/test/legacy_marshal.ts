// The HAND-WRITTEN path, copied verbatim from sdk/ts/wasm/engine.ts at a844b2a1
// (marshalGame, parseState, wireStateCard, cardFromWire and their helpers) so
// the parity test and bench compare against exactly what ships today. Plain TS,
// no path aliases: it runs under tsx, bundled, and on node's own type stripping.
export type Card = { suit: number; value: number };
export type AnyGame = any;   // eslint-disable-line @typescript-eslint/no-explicit-any
export interface Kernel {
    memory: WebAssembly.Memory;
    k_game(): number; k_io(): number; k_layout_hash(): number;
    k_import(): void; k_import_keys(): void; k_export(): number; k_set_deterministic_deck(on: number): void;
    k_adopt(): void; k_human_mask(): number; k_pickup(seat: number): number; k_transition(): void; k_refill(): void;
}

export const i8 = (v: number) => (v > 127 ? v - 256 : v);
export const HIDDEN_CARD: Card = { suit: -1, value: -1 };
export const CARD_POOL: Card[] = [];
for (let s = 0; s < 4; s++) for (let v = 1; v <= 13; v++) CARD_POOL[s * 13 + v - 1] = { suit: s, value: v };
export const WIRE_HIDDEN = 0xfe;
export const WIRE_NONE = 0xff;
export function wireStateCard(c: Card): number {
    let s = i8(c.suit & 0xff), v = i8(c.value & 0xff);
    if (s === -1 && v === -1) return WIRE_HIDDEN;
    if (s < 0) s = 0; else if (s > 3) s = 3;
    if (v < 1) v = 1; else if (v > 13) v = 13;
    return s * 13 + (v - 1);
}
export function cardFromWire(b: number): Card {
    if (b === WIRE_HIDDEN) return HIDDEN_CARD;
    return CARD_POOL[b <= 51 ? b : 51];
}
export const G_STATUS_TO_INT: Record<string, number> = { waiting: 0, playing: 1, game_over: 2 };
export const P_STATUS_TO_INT: Record<string, number> = { idle: 0, ready: 1, in: 2, out: 3 };

let memView = new Uint8Array(0);
function mem(ex: Kernel): Uint8Array {
    if (memView.buffer !== ex.memory.buffer) memView = new Uint8Array(ex.memory.buffer);
    return memView;
}

export function legacyMarshal(ex: Kernel, game: AnyGame): void {
    const buf = mem(ex);
    let q = ex.k_io();
    buf[q++] = G_STATUS_TO_INT[game.status] ?? 0;
    buf[q++] = game.players.length;
    buf[q++] = game.power_suit & 0xff;
    buf[q++] = game.first_attacker & 0xff;
    buf[q++] = game.defender & 0xff;
    buf[q++] = game.discard_pile_length & 0xff;
    buf[q++] = (game.discard_pile_length >> 8) & 0xff;
    buf[q++] = game.flipped ? 1 : 0;
    buf[q++] = game.flipped ? wireStateCard(game.flipped) : 0;
    let mask = 0;
    for (const pid of game.good_players ?? []) {
        const s = game.players.findIndex((p: AnyGame) => p.player_id === pid);
        if (s >= 0) mask |= 1 << s;
    }
    buf[q++] = mask & 0xff;
    buf[q++] = (mask >> 8) & 0xff;
    buf[q++] = (mask >> 16) & 0xff;
    buf[q++] = (mask >> 24) & 0xff;
    buf[q++] = game.good_timestamp !== null && game.good_timestamp !== undefined ? 1 : 0;
    buf[q++] = game.deck.length & 0xff;
    buf[q++] = (game.deck.length >> 8) & 0xff;
    for (const c of game.deck) buf[q++] = wireStateCard(c);
    buf[q++] = game.table_battles.length;
    for (const b of game.table_battles) {
        buf[q++] = wireStateCard(b.attack);
        buf[q++] = b.defense ? wireStateCard(b.defense) : WIRE_NONE;
    }
    for (const p of game.players) {
        buf[q++] = P_STATUS_TO_INT[p.status] ?? 0;
        buf[q++] = p.awaiting_attack ? 1 : 0;
        buf[q++] = p.hand.length;
        for (const c of p.hand) buf[q++] = wireStateCard(c);
    }
    buf[q++] = game.elimination_order.length;
    for (const pid of game.elimination_order) {
        const s = game.players.findIndex((p: AnyGame) => p.player_id === pid);
        buf[q++] = s & 0xff;
    }
    ex.k_import();
    {
        const io = ex.k_io();
        for (let i = 0; i < game.players.length; i++) buf[io + i] = game.players[i].is_ai ? 0 : 0xff;
        ex.k_import_keys();
    }
    if (game.deterministic_deck) ex.k_set_deterministic_deck(1);
}

export interface KernelState {
    status: number; numPlayers: number; powerSuit: number; firstAttacker: number; defender: number;
    discard: number; flipped: Card | null; goodMask: number; hasGoodTs: boolean; deck: Card[];
    battles: { attack: Card; defense: Card | null }[];
    players: { status: number; awaiting: boolean; hand: Card[] }[];
    elimination: number[];
}

// wasm_export_state + parseState
export function legacyParse(ex: Kernel): KernelState {
    ex.k_export();
    const buf = mem(ex);
    let q = ex.k_io();
    const status = buf[q++];
    const numPlayers = buf[q++];
    const powerSuit = i8(buf[q++]);
    const firstAttacker = i8(buf[q++]);
    const defender = i8(buf[q++]);
    const discard = buf[q] | (buf[q + 1] << 8); q += 2;
    const hasFlipped = buf[q++] !== 0;
    const flippedWire = buf[q++];
    const goodMask = buf[q] | (buf[q + 1] << 8) | (buf[q + 2] << 16) | (buf[q + 3] << 24); q += 4;
    const hasGoodTs = buf[q++] !== 0;
    const deckN = buf[q] | (buf[q + 1] << 8); q += 2;
    const deck: Card[] = new Array(deckN);
    for (let i = 0; i < deckN; i++) deck[i] = cardFromWire(buf[q++]);
    const nBattles = buf[q++];
    const battles: KernelState['battles'] = [];
    for (let i = 0; i < nBattles; i++) {
        const attack = cardFromWire(buf[q++]);
        const dw = buf[q++];
        battles.push({ attack, defense: dw === WIRE_NONE ? null : cardFromWire(dw) });
    }
    const players: KernelState['players'] = [];
    for (let i = 0; i < numPlayers; i++) {
        const pStatus = buf[q++];
        const awaiting = buf[q++] !== 0;
        const handN = buf[q++];
        const hand: Card[] = new Array(handN);
        for (let j = 0; j < handN; j++) hand[j] = cardFromWire(buf[q++]);
        players.push({ status: pStatus, awaiting, hand });
    }
    const elimN = buf[q++];
    const elimination: number[] = [];
    for (let i = 0; i < elimN; i++) elimination.push(i8(buf[q++]));
    return {
        status, numPlayers, powerSuit, firstAttacker, defender, discard,
        flipped: hasFlipped ? cardFromWire(flippedWire) : null,
        goodMask, hasGoodTs, deck, battles, players, elimination,
    };
}
