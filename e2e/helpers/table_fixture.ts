// table_fixture.ts - a test's game, built by the kernel (docs/C_GAME_SHAPE_MIGRATION.md Phase 3c).
//
//   const { state, roster } = fixture()
//       .seats([{ id: 'a', name: 'Ann' }, { id: 'b', name: 'Bot', brain: 'cordite' }])
//       .status(PLAYING).hand(0, '6h 7h Qs').hand(1, '8d').table('7c/8c', '9d')
//       .deck('Ts Jd').trump('As').good(1)
//       .build();
//
// build() returns the durable state blob and the durable roster of a row the C
// Table has ALREADY LOADED (table_seal: game_validate and roster_validate, a
// brain this build links, seat counts that agree). A board the kernel could not
// have produced never reaches a test: it throws FixtureRefused with the kernel's
// reason code and its generated name.
//
// This file knows no byte layout. Fields are written through the generated
// setters in sdk/ts/gen/game_layout.bots.ts onto the resident game of a PRIVATE
// bots.wasm instance, card text is read by the kernel's card_list_parse
// (c/src/card.h, whose grammar is the one documented there), and the bytes come
// back from wasm_fixture_seal. fixtureTable() is the ServerTable over that same
// instance, for a test that wants to load, act on or envelope what it built.
//
// Server and tests only: it writes and returns the unmasked state blob
// (e2e/security_client_boundary.test.ts denies it to the browser).

import * as L from '../../sdk/ts/gen/game_layout.bots.ts';
import { LAYOUT_HASH } from '../../sdk/ts/gen/layout_hash.bots.ts';
import { ServerTable, type TableExports } from '../../sdk/ts/table/server_table.ts';
import { assertLayoutHash } from '../../sdk/ts/wasm/layout_hash.ts';
import { botsTestWasm } from './bots_test_wasm.ts';

export const WAITING = L.GAME_STATUS_WAITING;
export const PLAYING = L.GAME_STATUS_PLAYING;
export const GAME_OVER = L.GAME_STATUS_GAME_OVER;
export const IDLE = L.PLAYER_STATUS_IDLE;
export const READY = L.PLAYER_STATUS_READY;
export const IN = L.PLAYER_STATUS_IN;
export const OUT = L.PLAYER_STATUS_OUT;

/** One seat of the roster. `brain` is a bot_roster key ('cordite'); omitted or '' is a human. */
export interface FixtureSeat { id: string; name: string; brain?: string }

/** A row the kernel accepted: the games.state and games.roster bytes. */
export interface TableFixture { state: Uint8Array; roster: Uint8Array }

interface FixtureExports extends TableExports {
    wasm_fixture_begin(): number;
    wasm_fixture_title(len: number): number;
    wasm_fixture_seat(idLen: number, nameLen: number, brainLen: number): number;
    wasm_fixture_seal(): number;
    wasm_card_list_parse(len: number, cap: number, battles: number): number;
    wasm_cards_a_ptr(): number;
    wasm_cards_b_ptr(): number;
    wasm_unambiguous_cover(nCover: number, nBattles: number, powerSuit: number): number;
}

let kernel: { ex: FixtureExports; table: ServerTable } | null = null;
function k(): { ex: FixtureExports; table: ServerTable } {
    if (!kernel) {
        const inst = new WebAssembly.Instance(new WebAssembly.Module(botsTestWasm() as BufferSource), {});
        const ex = inst.exports as unknown as FixtureExports;
        assertLayoutHash('bots_test.wasm', ex, LAYOUT_HASH, 'sdk/ts/gen/layout_hash.bots.ts');
        ex.wasm_init();
        kernel = { ex, table: new ServerTable(ex) };
    }
    return kernel;
}

/** The C Table over the fixtures' own bots.wasm instance. */
export function fixtureTable(): ServerTable { return k().table; }

/** The raw exports of that instance, for a test helper that reads the loaded board through the generated accessors. */
export function fixtureExports(): TableExports & { wasm_game_ptr_internal(): number; wasm_moves_ptr_internal(): number; wasm_legal_moves(seat: number): number } {
    return k().ex as unknown as ReturnType<typeof fixtureExports>;
}

/** The generated name of a kernel result code, among the constant families `prefixes` name. */
export function reasonOf(code: number, prefixes: string[]): string {
    for (const [name, value] of Object.entries(L)) {
        if (value === code && prefixes.some((p) => name.startsWith(p))) return name;
    }
    return `UNKNOWN(${code})`;
}

/** The kernel refused a fixture: `reason` is the generated name of `code`, `detail` the ROSTER_E_* behind a TABLE_E_ROSTER. */
export class FixtureRefused extends Error {
    constructor(
        readonly step: string,
        readonly code: number,
        readonly reason: string,
        readonly detail?: { code: number; reason: string },
    ) {
        super(`table fixture refused at ${step}: ${reason} (${code})${detail ? ` - ${detail.reason} (${detail.code})` : ''}`);
        this.name = 'FixtureRefused';
    }
}

const enc = new TextEncoder();

class Builder {
    private seatList: FixtureSeat[] = [];
    private titleText = '';
    private gameStatus: number = WAITING;
    private playerCount: number | null = null;
    private statuses = new Map<number, number>();
    private hands = new Map<number, string>();
    private battles: string[] = [];
    private deckText = '';
    private trumpText: string | null = null;
    private suit: number | null = null;
    private attackerSeat: number | null = null;
    private defenderSeat: number | null = null;
    private discardCount = 0;
    private out: number[] = [];
    private goods: number[] = [];
    private goodTs = false;
    private awaitingSeats = new Set<number>();
    private seeded = false;

    /** The roster, in seat order. The state's seat count follows it unless numPlayers() says otherwise. */
    seats(list: FixtureSeat[]): this { this.seatList = list; return this; }
    /** The table title (default empty). */
    title(t: string): this { this.titleText = t; return this; }
    /** GAME_STATUS_* (default WAITING). */
    status(s: number): this { this.gameStatus = s; return this; }
    /**
     * PLAYER_STATUS_* for one seat. Default: IN while PLAYING (OUT once eliminated);
     * otherwise READY for a bot and IDLE for a human, as the kernel parks them.
     */
    seatStatus(seat: number, s: number): this { this.statuses.set(seat, s); return this; }
    /** The state's seat count, for a board that must disagree with its roster. */
    numPlayers(n: number): this { this.playerCount = n; return this; }
    /** A seat's hand, in order. */
    hand(seat: number, cards: string): this { this.hands.set(seat, cards); return this; }
    /** The table, one argument per battle: an attack, or 'attack/cover'. */
    table(...battles: string[]): this { this.battles = battles; return this; }
    /** The deck in array order; a seed-dealt (deterministic) game draws from the front. */
    deck(cards: string): this { this.deckText = cards; return this; }
    /** The face-up trump; the power suit is its suit. */
    trump(card: string): this { this.trumpText = card; return this; }
    /** The power suit (SUIT_*) of a game whose trump is no longer face up. */
    powerSuit(suit: number): this { this.suit = suit; return this; }
    /** The first attacker (default 0). */
    attacker(seat: number): this { this.attackerSeat = seat; return this; }
    /** The defender (default the seat after the attacker once dealt, 0 in a lobby). */
    defender(seat: number): this { this.defenderSeat = seat; return this; }
    /** The discard pile's size. */
    discard(n: number): this { this.discardCount = n; return this; }
    /** The elimination order, first out first. */
    eliminated(...seats: number[]): this { this.out = seats; return this; }
    /** The seats that have said good. */
    good(...seats: number[]): this { this.goods = seats; return this; }
    /** The good timestamp is set (every attack covered, the clock running). */
    goodTimestamp(on = true): this { this.goodTs = on; return this; }
    /** Seats flagged awaiting an attack. */
    awaiting(...seats: number[]): this { this.awaitingSeats = new Set(seats); return this; }
    /** A seed-dealt game: every draw takes the deck's first card (game.c draw_index). */
    deterministic(on = true): this { this.seeded = on; return this; }

    /** Composes the board in the kernel and seals it: bytes the C Table has loaded, or FixtureRefused. */
    build(): TableFixture {
        const { ex } = k();
        const hands = new Map<number, number[]>();
        for (const [seat, text] of this.hands) hands.set(seat, parseCards(`hand(${seat})`, text, L.Player_hand_LEN, false).cards);
        const table = parseCards('table', this.battles.join(' '), L.Game_table_battles_LEN, true);
        const deck = parseCards('deck', this.deckText, L.Game_deck_LEN, false).cards;
        const trump = this.trumpText === null ? null : parseOne('trump', this.trumpText);

        const g = ex.wasm_fixture_begin();
        roster(`title`, ex.wasm_fixture_title(...put(enc.encode(this.titleText)) as [number]));
        this.seatList.forEach((s, i) => {
            const [a, b, c] = put(enc.encode(s.id), enc.encode(s.name), enc.encode(s.brain ?? ''));
            roster(`seat ${i}`, ex.wasm_fixture_seat(a, b, c));
        });

        const m = L.memOf(ex.memory.buffer);
        const np = this.playerCount ?? this.seatList.length;
        const dealt = this.gameStatus !== WAITING;
        const attacker = this.attackerSeat ?? 0;
        L.Game_set_status(m, g, this.gameStatus);
        L.Game_set_num_players(m, g, np);
        L.Game_set_power_suit(m, g, trump !== null ? L.Card_unpack_suit(trump) : this.suit ?? 0);
        L.Game_set_first_attacker(m, g, attacker);
        L.Game_set_defender(m, g, this.defenderSeat ?? (dealt && np >= 2 ? (attacker + 1) % np : 0));
        L.Game_set_discard_pile_length(m, g, this.discardCount);
        L.Game_set_deterministic_deck(m, g, this.seeded);
        if (trump !== null) {
            L.Game_set_has_flipped(m, g, true);
            L.Card_raw_set(m, L.Game_flipped_at(g), trump);
        }
        L.Game_set_deck_count(m, g, deck.length);
        deck.forEach((c, i) => L.Card_raw_set(m, L.Game_deck_at(g, i), c));
        L.Game_set_num_battles(m, g, table.cards.length);
        table.cards.forEach((c, i) => {
            const b = L.Game_table_battles_at(g, i);
            L.Card_raw_set(m, L.Battle_attack_at(b), c);
            L.Card_raw_set(m, L.Battle_defense_at(b), table.covers[i]);
        });
        for (let s = 0; s < Math.min(np, L.Game_players_LEN); s++) {
            const p = L.Game_players_at(g, s);
            const bot = (this.seatList[s]?.brain ?? '') !== '';
            const fallback = this.gameStatus === PLAYING ? (this.out.includes(s) ? OUT : IN) : bot ? READY : IDLE;
            L.Player_set_status(m, p, this.statuses.get(s) ?? fallback);
            L.Player_set_awaiting_attack(m, p, this.awaitingSeats.has(s));
            const hand = hands.get(s) ?? [];
            L.Player_set_hand_count(m, p, hand.length);
            hand.forEach((c, i) => L.Card_raw_set(m, L.Player_hand_at(p, i), c));
        }
        this.out.slice(0, L.Game_elimination_order_LEN).forEach((s, i) => L.Game_set_elimination_order(m, g, i, s));
        L.Game_set_num_eliminated(m, g, this.out.length);
        let mask = 0;
        for (const s of this.goods) {
            if (!Number.isInteger(s) || s < 0 || s > 31) throw new RangeError(`table fixture: good(${s}) is not a seat bit`);
            mask |= 1 << s;
        }
        L.Game_set_good_players_mask(m, g, mask >>> 0);
        L.Game_set_has_good_timestamp(m, g, this.goodTs);

        const n = ex.wasm_fixture_seal();
        if (n < 0) {
            const detail = n === L.TABLE_E_ROSTER ? ex.wasm_table_detail() : null;
            throw new FixtureRefused('seal', n, reasonOf(n, ['GAME_INVALID_', 'TABLE_E_']),
                detail === null ? undefined : { code: detail, reason: reasonOf(detail, ['ROSTER_E_']) });
        }
        const io = ex.wasm_io_ptr();
        const u8 = L.memOf(ex.memory.buffer).u8;
        return { state: u8.slice(io, io + n), roster: u8.slice(io + n, io + n + L.ROSTER_BYTES) };
    }
}

/** A new fixture: an empty WAITING table until the calls say otherwise. */
export function fixture(): Builder { return new Builder(); }

/** Writes byte strings back to back at the IO buffer; returns their lengths. */
function put(...parts: Uint8Array[]): number[] {
    const { ex } = k();
    const u8 = L.memOf(ex.memory.buffer).u8;
    let at = ex.wasm_io_ptr();
    if (parts.reduce((s, p) => s + p.length, 0) > ex.wasm_io_cap()) throw new RangeError('table fixture: input exceeds the IO buffer');
    return parts.map((p) => { u8.set(p, at); at += p.length; return p.length; });
}

function roster(step: string, rc: number): void {
    if (rc < 0) throw new FixtureRefused(step, rc, reasonOf(rc, ['ROSTER_E_']));
}

/** Card text through the kernel's card_list_parse: raw Card bytes (and covers for a battle list). */
function parseCards(step: string, text: string, cap: number, battles: boolean): { cards: number[]; covers: number[] } {
    const { ex } = k();
    const [len] = put(enc.encode(text));
    const n = ex.wasm_card_list_parse(len, cap, battles ? 1 : 0);
    if (n < 0) throw new FixtureRefused(step, n, reasonOf(n, ['CARD_PARSE_E_']));
    const m = L.memOf(ex.memory.buffer);
    const io = ex.wasm_io_ptr();
    const cards = Array.from({ length: n }, (_, i) => L.Card_raw_get(m, io + i));
    const covers = battles ? Array.from({ length: n }, (_, i) => L.Card_raw_get(m, io + n + i)) : [];
    return { cards, covers };
}

function parseOne(step: string, text: string): number {
    const { cards } = parseCards(step, text, 1, false);
    if (cards.length === 0) throw new FixtureRefused(step, L.CARD_PARSE_E_EMPTY, 'CARD_PARSE_E_EMPTY');
    return cards[0];
}

// ---- the one-tap cover resolver (legal.c unambiguous_cover) -----------------
//
// "Do these cover cards cover the uncovered attacks in exactly ONE way" - a
// different question from the menu's, and one no shipped host asks any more: the
// web's Cover button moved to client_play (client_table.h) and iOS never called
// it. So wasm_unambiguous_cover is a TEST-BUILD export now (c/Makefile
// WASM_BOTS_UNSHIPPED_API), and this is the only thing that reaches it. The
// packing lives here for the same reason the export does - it is a test's
// business, not a product's.

/** The paired result: cover card i defends attackCards[i]. */
export interface CoverCombination { coverCards: PlayCardLike[]; attackCards: PlayCardLike[] }

interface PlayCardLike { suit: number; value: number }

// The wire card byte (c/wasm/wire.h): 0..51 = suit*13 + (value-1), 0xFF no card.
const WIRE_NONE = 0xff;
const wireCard = (c: PlayCardLike): number => c.suit * 13 + (c.value - 1);

/**
 * The pairing, or null when the selection covers the table in no way or in more
 * than one. An uncovered battle's defense is null, or the kernel's CARD_NONE on
 * a board a client holds.
 */
export function unambiguousCover(
    coverCards: readonly PlayCardLike[],
    battles: readonly { attack: PlayCardLike; defense: PlayCardLike | null }[],
    powerSuit: number,
): CoverCombination | null {
    if (coverCards.length === 0) return null;
    const ex = fixtureExports() as unknown as FixtureExports;
    const buf = new Uint8Array(ex.memory.buffer);
    const a = ex.wasm_cards_a_ptr();
    coverCards.forEach((c, i) => { buf[a + i] = wireCard(c); });
    const b = ex.wasm_cards_b_ptr();
    battles.forEach((t, i) => {
        buf[b + 2 * i] = wireCard(t.attack);
        const d = t.defense;
        buf[b + 2 * i + 1] = !d || (d.suit === -2 && d.value === -2) ? WIRE_NONE : wireCard(d);
    });
    const n = ex.wasm_unambiguous_cover(coverCards.length, battles.length, powerSuit);
    if (n <= 0) return null;
    // Re-fetch: a wasm call can grow (and detach) the buffer.
    const out = new Uint8Array(ex.memory.buffer);
    const io = ex.wasm_io_ptr();
    const attackCards: PlayCardLike[] = [];
    for (let i = 0; i < n; i++) {
        const v = out[io + i];
        attackCards.push({ suit: (v / 13) | 0, value: (v % 13) + 1 });
    }
    return { coverCards: [...coverCards], attackCards };
}
