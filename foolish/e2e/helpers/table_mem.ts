// table_mem.ts - whole games on the C Table in memory, with no database
// (docs/C_GAME_SHAPE_MIGRATION.md Phase 8).
//
// Pure suites and benches used to build a TypeScript Game and drive it through
// the TS action handlers and bot shims. They now hold a row the way the server
// does - the durable state blob and roster - and run every operation on the
// fixtures' C Table (./table_fixture.ts): load, then act, drive or ask, then copy
// the products out. The board is read through the generated accessors
// (sdk/ts/gen/game_layout.bots.ts), the legal moves come from the kernel's own
// enumerator (legal.c, read through sdk/ts/gen/anim.bots.ts), and what a seat is
// shown is its envelope read by the web's reader (sdk/ts/table/client_table.ts).
//
// It is ./table_play.ts without the database: that module reads stored rows and
// imports the Postgres harness, which a pure suite must not open. It knows no
// byte layout.

import * as L from '../../sdk/ts/gen/game_layout.bots.ts';
import * as A from '../../sdk/ts/gen/anim.bots.ts';
import { AWIRE_KIND, encodeAction, type AwireMove } from '../../sdk/ts/wire/awire.ts';
import { clientTable, type TableView } from '../../sdk/ts/table/client_table.ts';
import type { TableDrive, TableProducts } from '../../sdk/ts/table/server_table.ts';
import { fixture, fixtureExports, fixtureTable, reasonOf, READY, IDLE, type FixtureSeat, type TableFixture } from './table_fixture.ts';

export interface MemCard { suit: number; value: number }

export interface MemSeat {
    id: string;
    name: string;
    /** bot_roster key; '' for a human. */
    brain: string;
    /** PLAYER_STATUS_* */
    status: number;
    hand: MemCard[];
    awaiting: boolean;
}

/** The whole board of a loaded table (server-only state: tests only). */
export interface MemBoard {
    /** GAME_STATUS_* */
    status: number;
    seats: MemSeat[];
    defender: number;
    firstAttacker: number;
    powerSuit: number;
    deck: MemCard[];
    discard: number;
    trump: MemCard | null;
    battles: { attack: MemCard; defense: MemCard | null }[];
    eliminated: number[];
    goodMask: number;
    hasGoodTimestamp: boolean;
    deterministic: boolean;
}

export type MoveKind = keyof typeof AWIRE_KIND;

export interface MemMove {
    seat: number;
    kind: MoveKind;
    cards: MemCard[];
    attack_cards?: MemCard[];
    /** The awire bytes a client sends for it. */
    wire: Uint8Array;
}

const card = (raw: number): MemCard => ({ suit: L.Card_unpack_suit(raw), value: L.Card_unpack_value(raw) });
const isCard = (c: MemCard) => c.suit >= 0 && c.value > 0;
const KIND_OF = Object.fromEntries(Object.entries(AWIRE_KIND).map(([k, v]) => [v, k])) as Record<number, MoveKind>;
const EMPTY = new Uint8Array(0);

export const hexOf = (b: Uint8Array): string => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');

function concat(a: Uint8Array, b: Uint8Array): Uint8Array {
    const out = new Uint8Array(a.length + b.length);
    out.set(a);
    out.set(b, a.length);
    return out;
}

function must(what: string, rc: number): void {
    if (rc < 0) throw new Error(`table_mem: ${what} refused: ${reasonOf(rc, ['TABLE_E_', 'GAME_INVALID_', 'ROSTER_E_'])} (${rc})`);
}

/** The board the fixtures' table holds right now. */
export function residentBoard(): MemBoard {
    const ex = fixtureExports();
    const m = L.memOf(ex.memory.buffer);
    const g = ex.wasm_game_ptr_internal();
    const seats: MemSeat[] = fixtureTable().seats().map((s, i) => {
        const p = L.Game_players_at(g, i);
        return {
            ...s, status: L.Player_get_status(m, p), awaiting: L.Player_get_awaiting_attack(m, p),
            hand: Array.from({ length: L.Player_get_hand_count(m, p) }, (_, j) => card(L.Card_raw_get(m, L.Player_hand_at(p, j)))),
        };
    });
    return {
        status: L.Game_get_status(m, g),
        seats,
        defender: L.Game_get_defender(m, g),
        firstAttacker: L.Game_get_first_attacker(m, g),
        powerSuit: L.Game_get_power_suit(m, g),
        deck: Array.from({ length: L.Game_get_deck_count(m, g) }, (_, i) => card(L.Card_raw_get(m, L.Game_deck_at(g, i)))),
        discard: L.Game_get_discard_pile_length(m, g),
        trump: L.Game_get_has_flipped(m, g) ? card(L.Card_raw_get(m, L.Game_flipped_at(g))) : null,
        battles: Array.from({ length: L.Game_get_num_battles(m, g) }, (_, i) => {
            const b = L.Game_table_battles_at(g, i);
            const d = card(L.Card_raw_get(m, L.Battle_defense_at(b)));
            return { attack: card(L.Card_raw_get(m, L.Battle_attack_at(b))), defense: isCard(d) ? d : null };
        }),
        eliminated: Array.from({ length: L.Game_get_num_eliminated(m, g) }, (_, i) => L.Game_get_elimination_order(m, g, i)),
        goodMask: L.Game_get_good_players_mask(m, g),
        hasGoodTimestamp: L.Game_get_has_good_timestamp(m, g),
        deterministic: L.Game_get_deterministic_deck(m, g),
    };
}

/** Every legal move `seat` has on the fixtures' loaded table, as the kernel enumerates them (a wait is no move). */
export function residentMoves(seat: number): MemMove[] {
    const ex = fixtureExports();
    const n = ex.wasm_legal_moves(seat);
    const m = A.memOf(ex.memory.buffer);
    const lm = ex.wasm_moves_ptr_internal();
    const out: MemMove[] = [];
    for (let i = 0; i < n; i++) {
        const mv = A.LegalMoves_moves_at(lm, i);
        const kind = KIND_OF[A.LegalMove_get_type(m, mv)];
        if (!kind) continue;
        const k = A.LegalMove_get_n_cards(m, mv);
        const cards = Array.from({ length: k }, (_, j) => card(A.Card_raw_get(m, A.LegalMove_cards_at(mv, j))));
        const move: MemMove = { seat, kind, cards, wire: EMPTY };
        if (kind === 'cover') move.attack_cards = Array.from({ length: k }, (_, j) => card(A.Card_raw_get(m, A.LegalMove_attack_cards_at(mv, j))));
        move.wire = encodeAction({ kind, cards, attack_cards: move.attack_cards } as AwireMove);
        out.push(move);
    }
    return out;
}

/**
 * One games row held in memory: the state and roster a commit wrote, the
 * version, the deal seed and the session log (games.logs_packed), updated the
 * way commit_table updates them. Every call is one kernel section on the
 * fixtures' table.
 */
export class MemTable {
    version = 0;
    log: Uint8Array = EMPTY;
    seedHex: string | null = null;

    constructor(public state: Uint8Array, public roster: Uint8Array, public gameId = 'mem') {}

    /** A row a fixture built. */
    static of(f: TableFixture, gameId = 'mem'): MemTable {
        return new MemTable(f.state, f.roster, gameId);
    }

    /**
     * A table dealt from `seed` (32 bytes): a lobby of `seats`, every seat ready
     * but the first, which readies and so deals, as the server's last ready does.
     */
    static deal(seats: FixtureSeat[], seed: Uint8Array, gameId = 'mem'): MemTable {
        let b = fixture().seats(seats);
        seats.forEach((_, i) => { b = b.seatStatus(i, i === 0 ? IDLE : READY); });
        const t = MemTable.of(b.build(), gameId);
        t.load();
        must('ready', fixtureTable().ready(seats[0].id, seed));
        const p = t.commit();
        if (!p.dealtNow) throw new Error('table_mem: the last ready did not deal');
        t.seedHex = hexOf(seed);
        return t;
    }

    /** Loads the row into the fixtures' table and sets its deal seed, as every server section does. */
    load(): void {
        const table = fixtureTable();
        must('load', table.load(this.state, this.roster));
        must('deal seed', table.setDealSeed(this.seedHex));
    }

    /** The row's board. */
    board(): MemBoard {
        this.load();
        return residentBoard();
    }

    get status(): number { return this.board().status; }

    /** Every legal move of every seat still IN (or of the seats `allow` admits), in seat order. */
    moves(allow?: (seat: number) => boolean): MemMove[] {
        const b = this.board();
        return b.seats.flatMap((s, seat) =>
            (s.status !== L.PLAYER_STATUS_IN || (allow && !allow(seat)) ? [] : residentMoves(seat)));
    }

    /** What the server's table_act says of `wire` by `seat`, committing nothing: TABLE_APPLIED, TABLE_REJECTED, ... */
    probe(seat: number, wire: Uint8Array): { rc: number; reject: number } {
        this.load();
        const table = fixtureTable();
        const rc = table.act(table.seats()[seat].id, wire, null, 0);
        return { rc, reject: table.reject() };
    }

    /** `wire` by `seat`, committed when the table applies it. */
    act(seat: number, wire: Uint8Array): { rc: number; reject: number; products: TableProducts | null } {
        const r = this.probe(seat, wire);
        return { ...r, products: r.rc === L.TABLE_APPLIED ? this.commit() : null };
    }

    /**
     * One bot cycle (table_bot_drive), committed when it applied anything. The
     * row's session log is handed over as the bot loop hands it over; `log`
     * overrides it (an empty log, to take the memory away).
     */
    drive(maxActions = 0, log?: Uint8Array): { drive: TableDrive; products: TableProducts | null } {
        this.load();
        const table = fixtureTable();
        must('session log', table.setSessionLog(log ?? this.log));
        const d = table.botDrive(null, maxActions);
        if (typeof d === 'number') throw new Error(`table_mem: bot drive refused (${d})`);
        return { drive: d, products: d.n > 0 ? this.commit() : null };
    }

    /** The products of the operation just run on the loaded table, written into the row as commit_table writes them. */
    commit(nowMs = 1_760_000_000_000): TableProducts {
        const p = fixtureTable().commit(this.gameId, this.version + 1, nowMs);
        if (typeof p === 'number') throw new Error(`table_mem: commit refused (${p})`);
        this.version++;
        this.state = p.state;
        this.roster = p.roster;
        this.log = p.logsReset ? (p.logs ?? EMPTY) : p.logs ? concat(this.log, p.logs) : this.log;
        return p;
    }

    /** The response envelope the server writes for `viewer` (a seat, or -1 for a spectator). */
    envelope(viewer: number): Uint8Array {
        this.load();
        const env = fixtureTable().envelope(this.gameId, viewer, this.version);
        if (typeof env === 'number') throw new Error(`table_mem: envelope refused (${env})`);
        return env;
    }

    /** The board `viewer` is shown, read by the web's reader. */
    view(viewer: number): TableView {
        const v = clientTable().adoptEnvelope(this.envelope(viewer));
        if (!v) throw new Error(`table_mem: the client refused the envelope (${JSON.stringify(clientTable().lastRefusal())})`);
        return v;
    }
}

const RANKS = '23456789TJQKA';

/** A card in the kernel's card notation ("6h", "Td"), the text a fixture builder takes. */
export function cardText(c: MemCard): string {
    const rank = RANKS[c.value - 1];
    const suit = Object.entries(L).find(([k, v]) => k.startsWith('SUIT_') && v === c.suit)?.[0].slice('SUIT_'.length, 'SUIT_'.length + 1).toLowerCase();
    if (!rank || !suit) throw new RangeError(`cardText: {suit ${c.suit}, value ${c.value}} is not a card`);
    return `${rank}${suit}`;
}

export const cardsText = (cs: readonly MemCard[]): string => cs.map(cardText).join(' ');

/** A battle as the fixture builder takes it: 'attack' or 'attack/cover'. */
export const battleText = (b: { attack: MemCard; defense: MemCard | null }): string =>
    (b.defense ? `${cardText(b.attack)}/${cardText(b.defense)}` : cardText(b.attack));

export interface BoardSpec {
    /** Each seat's hand: its cards, or a count the builder fills with cards nobody else holds. */
    hands: (readonly MemCard[] | number)[];
    table?: { attack: MemCard; defense: MemCard | null }[];
    powerSuit: number;
    attacker: number;
    defender: number;
    /** GAME_STATUS_* (default PLAYING). */
    status?: number;
    /** Seats already out, first out first. */
    out?: number[];
}

/**
 * A dealt board sealed by the kernel, humans P0, P1, ... in seat order. A hand
 * given as a count is dealt from the 36-card deck, low cards first, skipping
 * every card the spec names.
 */
export function boardFixture(spec: BoardSpec): TableFixture {
    const named = [
        ...spec.hands.flatMap((h) => (typeof h === 'number' ? [] : h)),
        ...(spec.table ?? []).flatMap((b) => (b.defense ? [b.attack, b.defense] : [b.attack])),
    ];
    const free: MemCard[] = [];
    for (let value = 5; value <= 13; value++) {
        for (let suit = 0; suit < 4; suit++) {
            if (!named.some((c) => c.suit === suit && c.value === value)) free.push({ suit, value });
        }
    }
    let f = fixture().seats(spec.hands.map((_, i) => ({ id: `P${i}`, name: `P${i}` })))
        .status(spec.status ?? L.GAME_STATUS_PLAYING).powerSuit(spec.powerSuit)
        .attacker(spec.attacker).defender(spec.defender)
        .table(...(spec.table ?? []).map(battleText))
        .eliminated(...(spec.out ?? []));
    spec.hands.forEach((h, i) => {
        f = f.hand(i, cardsText(typeof h === 'number' ? free.splice(0, h) : h));
        if (spec.out?.includes(i)) f = f.seatStatus(i, L.PLAYER_STATUS_OUT);
    });
    return f.build();
}

/** The board `viewer` of a built fixture is shown, read by the web's reader. */
export function fixtureView(f: TableFixture, viewer: number, gameId = 'mem', version = 1): TableView {
    const t = MemTable.of(f, gameId);
    t.version = version;
    return t.view(viewer);
}
