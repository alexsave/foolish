// table_play.ts - read a stored games row through the C Table, and list and
// send the moves its seats could make (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b).
//
// Tests on the server used to rebuild a TypeScript Game from the row
// (loadCompleteGame) to find out whose move it was and what a hand held. The
// server has no such object any more, and neither do these tests: the row's
// state and roster are loaded into the fixtures' C Table, the board is read
// through the generated accessors (sdk/ts/gen/game_layout.bots.ts), and the
// legal moves come from the kernel's own enumerator (legal.c), read through
// sdk/ts/gen/anim.bots.ts. A move leaves as the awire bytes a client sends.

import * as L from '../../sdk/ts/gen/game_layout.bots.ts';
import * as A from '../../sdk/ts/gen/anim.bots.ts';
import { AWIRE_KIND, encodeAction, encodeActionRequest, type AwireMove } from '../../sdk/ts/wire/awire.ts';
import { pgPool } from '../harness.ts';
import { fixture, fixtureExports, fixtureTable, reasonOf } from './table_fixture.ts';

export interface PlayCard { suit: number; value: number }

export interface SeatState {
    id: string;
    name: string;
    /** bot_roster key; '' for a human. */
    brain: string;
    /** PLAYER_STATUS_* */
    status: number;
    hand: PlayCard[];
    awaiting: boolean;
}

export interface TableState {
    gameId: string;
    version: number;
    roundEpoch: number;
    /** The status column's label. */
    statusColumn: string;
    needsBotsColumn: boolean;
    gameSeed: string | null;
    logsPacked: string | null;
    state: Uint8Array;
    roster: Uint8Array;
    /** GAME_STATUS_* from the blob. */
    status: number;
    title: string;
    seats: SeatState[];
    defender: number;
    firstAttacker: number;
    powerSuit: number;
    deckCount: number;
    /** The deck in array order (the server-only full state: tests only). */
    deck: PlayCard[];
    discard: number;
    trump: PlayCard | null;
    battles: { attack: PlayCard; defense: PlayCard | null }[];
    eliminated: number[];
    goodMask: number;
    hasGoodTimestamp: boolean;
    /** A seed-dealt game: draws take the deck's first card. */
    deterministic: boolean;
}

const bytes = (hex: string): Uint8Array => Buffer.from(hex.replace(/^\\x/, ''), 'hex');
const card = (raw: number): PlayCard => ({ suit: L.Card_unpack_suit(raw), value: L.Card_unpack_value(raw) });
const isCard = (c: PlayCard) => c.suit >= 0 && c.value > 0;

/** Loads a stored table into the fixtures' kernel; throws with the kernel's reason if it does not load. */
function load(gameId: string, state: Uint8Array, roster: Uint8Array): void {
    const rc = fixtureTable().load(state, roster);
    if (rc < 0) throw new Error(`table_play: game ${gameId} does not load: ${reasonOf(rc, ['TABLE_E_', 'GAME_INVALID_'])}`);
}

/** The stored row, read through the kernel. null when there is no such row. */
export async function readTable(gameId: string): Promise<TableState | null> {
    const { rows } = await pgPool.query(
        'SELECT version, round_epoch, status, needs_bots, game_seed, logs_packed, state, roster FROM games WHERE id = $1', [gameId]);
    if (rows.length === 0) return null;
    const r = rows[0];
    const state = bytes(r.state), roster = bytes(r.roster);
    load(gameId, state, roster);
    return {
        ...residentBoard(gameId, state, roster),
        version: Number(r.version), roundEpoch: Number(r.round_epoch), statusColumn: r.status, needsBotsColumn: r.needs_bots,
        gameSeed: r.game_seed, logsPacked: r.logs_packed,
    };
}

/** The columns of a row, as the kernel's loaded table says they would be (for a board that is not stored). */
export type BoardState = Omit<TableState, 'version' | 'roundEpoch' | 'statusColumn' | 'needsBotsColumn' | 'gameSeed' | 'logsPacked'>;

/**
 * The board the fixtures' table holds right now (after a load, or after an
 * operation run on it in memory). `state`/`roster` are carried as given.
 */
export function residentBoard(gameId: string, state: Uint8Array, roster: Uint8Array): BoardState {
    const ex = fixtureExports();
    const m = L.memOf(ex.memory.buffer);
    const g = ex.wasm_game_ptr_internal();
    const rp = ex.wasm_table_roster_ptr();
    const names = fixtureTable().seats();
    const seats: SeatState[] = names.map((s, i) => {
        const p = L.Game_players_at(g, i);
        const n = L.Player_get_hand_count(m, p);
        return {
            ...s, status: L.Player_get_status(m, p), awaiting: L.Player_get_awaiting_attack(m, p),
            hand: Array.from({ length: n }, (_, j) => card(L.Card_raw_get(m, L.Player_hand_at(p, j)))),
        };
    });
    const battles = Array.from({ length: L.Game_get_num_battles(m, g) }, (_, i) => {
        const b = L.Game_table_battles_at(g, i);
        const d = card(L.Card_raw_get(m, L.Battle_defense_at(b)));
        return { attack: card(L.Card_raw_get(m, L.Battle_attack_at(b))), defense: isCard(d) ? d : null };
    });
    return {
        gameId, state, roster,
        status: L.Game_get_status(m, g),
        title: L.Roster_get_title_str(m, rp),
        seats,
        defender: L.Game_get_defender(m, g),
        firstAttacker: L.Game_get_first_attacker(m, g),
        powerSuit: L.Game_get_power_suit(m, g),
        deckCount: L.Game_get_deck_count(m, g),
        deck: Array.from({ length: L.Game_get_deck_count(m, g) }, (_, i) => card(L.Card_raw_get(m, L.Game_deck_at(g, i)))),
        discard: L.Game_get_discard_pile_length(m, g),
        trump: L.Game_get_has_flipped(m, g) ? card(L.Card_raw_get(m, L.Game_flipped_at(g))) : null,
        battles,
        eliminated: Array.from({ length: L.Game_get_num_eliminated(m, g) }, (_, i) => L.Game_get_elimination_order(m, g, i)),
        goodMask: L.Game_get_good_players_mask(m, g),
        hasGoodTimestamp: L.Game_get_has_good_timestamp(m, g),
        deterministic: L.Game_get_deterministic_deck(m, g),
    };
}

/** Like readTable, but the row must exist. */
export async function mustReadTable(gameId: string): Promise<TableState> {
    const t = await readTable(gameId);
    if (!t) throw new Error(`table_play: no games row ${gameId}`);
    return t;
}

export type MoveKind = keyof typeof AWIRE_KIND;

export interface PlayMove {
    seat: number;
    playerId: string;
    kind: MoveKind;
    cards: PlayCard[];
    attack_cards?: PlayCard[];
    /** The awire bytes a client sends for it. */
    wire: Uint8Array;
}

const KIND_OF = Object.fromEntries(Object.entries(AWIRE_KIND).map(([k, v]) => [v, k])) as Record<number, MoveKind>;

/** Every legal move of every seat still IN (or only the seats `allow` admits), in seat order, as the kernel enumerates them. */
export function legalMoves(t: BoardState, allow?:(seat: SeatState, index: number) => boolean): PlayMove[] {
    load(t.gameId, t.state, t.roster);
    const ex = fixtureExports();
    const out: PlayMove[] = [];
    t.seats.forEach((s, seat) => {
        if (s.status !== L.PLAYER_STATUS_IN || (allow && !allow(s, seat))) return;
        const n = ex.wasm_legal_moves(seat);
        const m = A.memOf(ex.memory.buffer);
        const lm = ex.wasm_moves_ptr_internal();
        for (let i = 0; i < n; i++) {
            const mv = A.LegalMoves_moves_at(lm, i);
            const kind = KIND_OF[A.LegalMove_get_type(m, mv)];
            if (!kind) continue;   // wait: not a move a client sends
            const k = A.LegalMove_get_n_cards(m, mv);
            const cards = Array.from({ length: k }, (_, j) => card(A.Card_raw_get(m, A.LegalMove_cards_at(mv, j))));
            const move: PlayMove = { seat, playerId: s.id, kind, cards, wire: new Uint8Array() };
            if (kind === 'cover') move.attack_cards = Array.from({ length: k }, (_, j) => card(A.Card_raw_get(m, A.LegalMove_attack_cards_at(mv, j))));
            move.wire = encodeAction({ kind, cards, attack_cards: move.attack_cards } as AwireMove);
            out.push(move);
        }
    });
    return out;
}

/** The `action` request body for a move. */
export const actionRequest = (gameId: string, move: PlayMove, intentVersion?: number): Uint8Array =>
    encodeActionRequest(gameId, move.wire, intentVersion);

/**
 * Card conservation of a stored dealt table, read straight from its blob: the
 * deck, the face-up trump, every hand, the table and the discard pile add up to
 * the full deck, with no duplicate and no card back.
 */
export async function checkCardConservation(gameId: string): Promise<{ ok: boolean; detail: string }> {
    const t = await mustReadTable(gameId);
    const ex = fixtureExports();
    const m = L.memOf(ex.memory.buffer);
    const g = ex.wasm_game_ptr_internal();
    const live: PlayCard[] = [];
    for (let i = 0; i < t.deckCount; i++) live.push(card(L.Card_raw_get(m, L.Game_deck_at(g, i))));
    for (const s of t.seats) live.push(...s.hand);
    for (const b of t.battles) { live.push(b.attack); if (b.defense) live.push(b.defense); }
    if (t.trump) live.push(t.trump);
    const key = (c: PlayCard) => `${c.suit}:${c.value}`;
    const seen = new Map<string, number>();
    let backs = 0;
    for (const c of live) { if (!isCard(c)) backs++; seen.set(key(c), (seen.get(key(c)) ?? 0) + 1); }
    const dupes = [...seen.entries()].filter(([, n]) => n > 1).map(([k, n]) => `${k}x${n}`);
    const expected = t.seats.length >= 6 ? 52 : 36;
    const total = live.length + t.discard;
    const ok = total === expected && dupes.length === 0 && backs === 0;
    return { ok, detail: `total=${total}/${expected} live=${live.length} discard=${t.discard}${dupes.length ? ` DUP[${dupes.join(',')}]` : ''}${backs ? ` BACKS=${backs}` : ''}` };
}

/** A card as the kernel's card notation writes it ("6h", "Td"): the text a fixture builder takes. */
export function cardText(c: PlayCard): string {
    const rank = '23456789TJQKA'[c.value - 1];
    const suit = Object.entries(L).find(([k, v]) => k.startsWith('SUIT_') && v === c.suit)?.[0].slice('SUIT_'.length, 'SUIT_'.length + 1).toLowerCase();
    if (!rank || !suit) throw new RangeError(`cardText: {suit ${c.suit}, value ${c.value}} is not a card`);
    return `${rank}${suit}`;
}

/**
 * A fixture builder holding exactly `b`'s board and roster, to edit and seal
 * again (a test that tampers with a stored row: its statuses, its hands, its
 * deck). Every field the board carries is set, so an unedited rebuild seals
 * to the same state blob.
 */
export function rebuild(b: BoardState): ReturnType<typeof fixture> {
    const cards = (cs: PlayCard[]) => cs.map(cardText).join(' ');
    let f = fixture().title(b.title).seats(b.seats.map(({ id, name, brain }) => ({ id, name, brain })))
        .status(b.status).powerSuit(b.powerSuit).attacker(b.firstAttacker).defender(b.defender)
        .discard(b.discard).deck(cards(b.deck))
        .table(...b.battles.map((x) => (x.defense ? `${cardText(x.attack)}/${cardText(x.defense)}` : cardText(x.attack))))
        .eliminated(...b.eliminated)
        .good(...b.seats.flatMap((_, i) => ((b.goodMask >>> i) & 1 ? [i] : [])))
        .goodTimestamp(b.hasGoodTimestamp).deterministic(b.deterministic)
        .awaiting(...b.seats.flatMap((s, i) => (s.awaiting ? [i] : [])));
    if (b.trump) f = f.trump(cardText(b.trump));
    b.seats.forEach((s, i) => { f = f.seatStatus(i, s.status).hand(i, cards(s.hand)); });
    return f;
}
