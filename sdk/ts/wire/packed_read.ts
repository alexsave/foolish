// Packed kernel blobs -> plain objects, read in TypeScript.
//
// Two formats cross the wasm/JS boundary as bytes and have to arrive as
// objects: view.c's state_put/state_get board (the per-viewer masked blob every
// /ws push and every games.state row carries) and evwire.c's animation sequence.
// The kernel writes both; this reads both.
//
// WHY THIS IS TYPESCRIPT. It was C - src/json_out.c decoded these and handed
// back JSON, so the layout lived in one language. That file is deleted, so the
// offsets below are a SECOND statement of a format the kernel already owns, and
// they can drift from it silently. Nothing here is a design win: it is the cost
// of not shipping a JSON emitter inside the wasm modules. The mitigation is
// e2e/view_codec.test.ts and e2e/packed_wire_stream.test.ts, which round-trip
// kernel-written bytes through these readers - a layout change in C fails them.
//
// SHAPE CONTRACT (unchanged from json_out.h). What comes back is RAW: ints
// where the kernel has ints, seats where the kernel has seats, null where a card
// is masked. It is NOT the host's view model. Identity (player_id, name, is_ai),
// good-order, timestamps and message prose are not here - game.h is explicit
// that seat identity "is deliberately not in the state blob; it lives with the
// caller", so the roster join stays host-side in wire/view.ts's viewToGame.

// ---------- the caps the wasm build is compiled at --------------------------
//
// state_get clamps every count to its array capacity so a corrupt blob cannot
// walk off the end. These mirror c/Makefile's bots.wasm caps; they are
// defence in depth, not parsing - a well-formed blob never reaches them.
const MAX_PLAYERS = 8;
const MAX_DECK = 64;
const MAX_BATTLES = 64;
const MAX_HAND_SIZE = 64;

// c/wasm/wire.h
const WIRE_CARD_HIDDEN = 0xfe;
const WIRE_CARD_NONE = 0xff;

// c/src/game.h
const PLAYER_STATUS_IN = 2;
const PLAYER_STATUS_OUT = 3;

// c/src/evwire.h
const EVWIRE_FORMAT_VERSION = 1;
const EVW_SEAT_NONE = 0xff;

/** One card as the kernel emits it. */
export interface KernelCard { s: number; v: number }

export interface KernelPlayerState {
    seat: number; name: string; status: number; handCount: number;
    awaitingAttack: boolean; strategyKey: number;
    hand: KernelCard[] | null;   // null for every seat that is not the viewer
}

/** A viewer-masked board: view.c's state_put layout, decoded. */
export interface KernelState {
    status: number; numPlayers: number; powerSuit: number;
    deckCount: number; discardCount: number; hasFlipped: boolean;
    firstAttacker: number; defender: number; viewer: number;
    goodMask: number; hasGoodTs: boolean; gameOver: number;
    flipped: KernelCard | null;
    battles: { attack: KernelCard; defense: KernelCard | null }[];
    eliminationOrder: number[];
    players: KernelPlayerState[];
}

export interface KernelEvent {
    type: number; seat: number; msg: number; from: number; to: number;
    cards: (KernelCard | null)[];
    target?: KernelCard;
    battle?: number;
    state: KernelState;
}

export interface KernelSequence {
    viewer: number; actor: number;
    events: KernelEvent[];
    game: KernelState;
}

// The message texts json_out's callers threw, kept verbatim: they are what the
// product's catch sites and e2e tests match on.
const badArg = (what: string) =>
    new Error(`${what}: bad argument (empty payload, or a viewer seat off the board)`);
const parseErr = (what: string) =>
    new Error(`${what}: not a readable payload (truncated, or a format this build does not read)`);

const i8 = (b: number) => (b << 24) >> 24;
const i16 = (lo: number, hi: number) => ((lo | (hi << 8)) << 16) >> 16;
const clamp = (v: number, hi: number) => (v < 0 ? 0 : v > hi ? hi : v);

// wire.h's card_from_wire_state: hostile ids clamp into the representable
// space rather than escaping as an out-of-range id.
function cardFromWire(b: number): KernelCard {
    const v = b > 51 ? 51 : b;
    return { s: (v / 13) | 0, v: (v % 13) + 1 };
}

// view.c's card_from_wire_masked: a redacted card decodes to the {0,1}
// placeholder the browser marshal has always used.
function cardFromWireMasked(b: number): KernelCard {
    return b === WIRE_CARD_HIDDEN ? { s: 0, v: 1 } : cardFromWire(b);
}

// game.c's game_done: the seat left standing once everyone else is out, or -1.
function gameDone(players: { status: number }[]): number {
    let inCount = 0, outCount = 0, lastIn = -1;
    for (let i = 0; i < players.length; i++) {
        if (players[i].status === PLAYER_STATUS_IN) { inCount++; lastIn = i; }
        else if (players[i].status === PLAYER_STATUS_OUT) { outCount++; }
    }
    return (inCount === 1 && outCount === players.length - 1) ? lastIn : -1;
}

// A cursor that refuses to read past the buffer. state_get itself trusts its
// caller for length (the kernel only ever hands it bytes it just wrote); a
// reader fed bytes off the network cannot, so every read is bounded here and a
// short blob is a parse failure rather than a board full of trailing zeros.
class Cur {
    constructor(readonly b: Uint8Array, readonly what: string, public q = 0) {}
    u8(): number { if (this.q >= this.b.length) throw parseErr(this.what); return this.b[this.q++]; }
    u16(): number { const lo = this.u8(), hi = this.u8(); return lo | (hi << 8); }
    take(n: number): Uint8Array {
        if (n < 0 || this.q + n > this.b.length) throw parseErr(this.what);
        const s = this.b.subarray(this.q, this.q + n); this.q += n; return s;
    }
}

// One board out of a cursor positioned at the start of a state_put payload.
function readState(c: Cur, viewer: number): KernelState {
    const status = i8(c.u8());
    const numPlayers = clamp(i8(c.u8()), MAX_PLAYERS);
    const powerSuit = i8(c.u8());
    const firstAttacker = i8(c.u8());
    const defender = i8(c.u8());
    const discardCount = i16(c.u8(), c.u8());
    const hasFlipped = c.u8() !== 0;
    const flippedWire = c.u8();
    const goodMask = (c.u8() | (c.u8() << 8) | (c.u8() << 16) | (c.u8() << 24)) >>> 0;
    const hasGoodTs = c.u8() !== 0;
    const deckCount = clamp(i16(c.u8(), c.u8()), MAX_DECK);
    c.take(deckCount);   // the deck's identities are never rendered, masked or not

    const nBattles = clamp(i8(c.u8()), MAX_BATTLES);
    const battles: KernelState['battles'] = [];
    for (let i = 0; i < nBattles; i++) {
        const attack = cardFromWire(c.u8());
        const db = c.u8();
        battles.push({ attack, defense: db === WIRE_CARD_NONE ? null : cardFromWire(db) });
    }

    const players: KernelPlayerState[] = [];
    for (let p = 0; p < numPlayers; p++) {
        const pstatus = i8(c.u8());
        const awaiting = c.u8() !== 0;
        const handCount = clamp(i8(c.u8()), MAX_HAND_SIZE);
        const handWire = c.take(handCount);
        const isViewer = viewer === p;
        players.push({
            seat: p,
            // Identity is not in the blob (game.h) - the caller owns it. Emitted
            // as the empty/zero the C decoder's zeroed slot produced, so the
            // join in viewToGame sees exactly what it always saw.
            name: '',
            status: pstatus,
            handCount,
            awaitingAttack: isViewer && awaiting,
            strategyKey: 0,
            hand: isViewer ? Array.from(handWire, cardFromWireMasked) : null,
        });
    }

    const nElim = clamp(i8(c.u8()), MAX_PLAYERS);
    const eliminationOrder: number[] = [];
    for (let i = 0; i < nElim; i++) eliminationOrder.push(i8(c.u8()));

    return {
        status, numPlayers, powerSuit, deckCount, discardCount, hasFlipped,
        firstAttacker, defender, viewer, goodMask, hasGoodTs,
        gameOver: gameDone(players),
        flipped: hasFlipped ? cardFromWire(flippedWire) : null,
        battles, eliminationOrder, players,
    };
}

/**
 * Decode a packed masked view blob (view.c's state_put layout) into the board it
 * describes. `viewer` is the seat whose hand is real in the blob, or -1 for the
 * spectator feed. Throws on an unreadable payload - never returns a partial board.
 */
export function kernelViewFromPacked(blob: Uint8Array, viewer: number): KernelState {
    const what = 'kernelViewFromPacked';
    if (!blob || blob.length <= 0) throw badArg(what);
    const state = readState(new Cur(blob, what), viewer);
    // A waiting lobby legitimately holds a single player (the creator, before
    // anyone joins), so only 0 is malformed - `playing` needs two, but that is
    // the play path's concern, not this decoder's.
    if (state.numPlayers < 1) throw parseErr(what);
    if (viewer !== -1 && (viewer < 0 || viewer >= state.numPlayers)) throw badArg(what);
    return state;
}

/**
 * Decode a packed evwire sequence (the bytes live play broadcasts and a replay
 * frame carries) into its events, each with the board as of that step.
 */
export function kernelEventsFromPacked(bytes: Uint8Array): KernelSequence {
    const what = 'kernelEventsFromPacked';
    if (!bytes || bytes.length < 4) throw badArg(what);
    if (bytes[0] !== EVWIRE_FORMAT_VERSION) throw parseErr(what);
    const viewer = bytes[1] === EVW_SEAT_NONE ? -1 : bytes[1];
    const actor = bytes[2] === EVW_SEAT_NONE ? -1 : bytes[2];
    const nEvents = bytes[3];

    const c = new Cur(bytes, what, 4);
    const events: KernelEvent[] = [];
    for (let i = 0; i < nEvents; i++) {
        const type = c.u8();
        const seatB = c.u8();
        const msg = c.u8();
        const from = c.u8();
        const to = c.u8();
        const flags = c.u8();
        const nCards = c.u8();
        const cardsWire = c.take(nCards);
        const ev: KernelEvent = {
            type,
            seat: seatB === EVW_SEAT_NONE ? -1 : seatB,
            msg, from, to,
            // The DEAL/REFILL redaction, already applied by the writer: a card
            // bound for someone else's hand crossed as WIRE_CARD_HIDDEN and
            // stays a card back here. There is nothing to leak - it never came.
            cards: Array.from(cardsWire, b => (b === WIRE_CARD_HIDDEN ? null : cardFromWire(b))),
            state: null as unknown as KernelState,
        };
        if (flags & 1) ev.target = cardFromWire(c.u8());
        if (flags & 2) ev.battle = c.u8();
        const snap = c.take(c.u16());
        ev.state = readState(new Cur(snap, what), viewer);
        events.push(ev);
    }
    // Trailer: the committed final board - the sequence's `game`.
    const final = c.take(c.u16());
    return { viewer, actor, events, game: readState(new Cur(final, what), viewer) };
}
