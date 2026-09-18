// kernel_board.ts - boards and tables for the kernel suites, built and moved by
// the kernel (docs/C_GAME_SHAPE_MIGRATION.md Phase 8).
//
// Two things the pure-kernel suites need beyond table_fixture.ts and bot_table.ts:
//
//   - randomPlayingBoard: a random board a fuzz can start from, sealed by the
//     kernel (a board game_validate refuses is skipped, never forced in);
//   - dealTable / tableMove: a dealt table with human seats as well as bots,
//     and one move by a seat through table_act, committed.
//
// No byte layout and no game rule lives here: cards are the kernel's notation
// (card.h) through table_play.ts cardText, and every legality question is the
// kernel's (table_act, legal.c through table_play.ts legalMoves).

import * as L from '../../sdk/ts/gen/game_layout.bots.ts';
import { tableCodeName, type ServerTable } from '../../sdk/ts/table/server_table.ts';
import { fixture, fixtureTable, FixtureRefused, IN, OUT, READY, type FixtureSeat, type TableFixture } from './table_fixture.ts';
import { cardText, type PlayCard } from './table_play.ts';
import type { BotTableRow } from './bot_table.ts';

/** A uniform draw in [0, 1). */
export type Rnd = () => number;

/** A deterministic LCG in [0, 1), so a red fuzz replays from its seed. */
export function lcg(seed: number): Rnd {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 4294967296; };
}

/** The deck a table of `np` seats plays with (card.h: 36 cards to 5 seats, 52 from 6). */
export function deckFor(np: number): PlayCard[] {
    const out: PlayCard[] = [];
    for (let suit = 0; suit < 4; suit++) for (let value = np >= 6 ? 1 : 5; value <= 13; value++) out.push({ suit, value });
    return out;
}

const text = (cards: PlayCard[]) => cards.map(cardText).join(' ');

/**
 * A random PLAYING board of 2 to 6 seats: the first attacker leads on an empty
 * table (or, with `battles`, a few attacks, some covered), some seats are out,
 * a random stock and maybe a face-up trump, every card from one deck. Seats are `brain(seat)` bots (default: all humans).
 * Returns null when the kernel refuses the board.
 */
export function randomPlayingBoard(rnd: Rnd, brain: (seat: number) => string = () => '', opts: { battles?: boolean } = {}): TableFixture | null {
    const ri = (n: number) => Math.floor(rnd() * n);
    const np = 2 + ri(5);
    const cards = deckFor(np);
    for (let i = cards.length - 1; i > 0; i--) { const j = ri(i + 1); [cards[i], cards[j]] = [cards[j], cards[i]]; }
    let k = 0;
    const hands: PlayCard[][] = [];
    const inSeats: number[] = [];
    const out: number[] = [];
    for (let i = 0; i < np; i++) {
        const isOut = i > 0 && rnd() < 0.25;
        const n = isOut ? 0 : 1 + ri(6);
        hands.push(cards.slice(k, k + n));
        k += n;
        if (isOut) out.push(i); else inSeats.push(i);
    }
    if (inSeats.length < 2) return null;
    const rest = cards.slice(k);
    const flipped = rnd() < 0.5 && rest.length > 0 ? rest.shift()! : null;
    // With `battles`, a few attacks on the table, some covered by an arbitrary card.
    const battles: string[] = [];
    for (let i = opts.battles ? ri(4) : 0; i > 0 && rest.length > 1; i--) {
        const attack = rest.shift()!;
        battles.push(rnd() < 0.4 ? `${cardText(attack)}/${cardText(rest.shift()!)}` : cardText(attack));
    }
    const deck = rest.slice(0, ri(Math.min(rest.length, 8) + 1));
    const discard = rest.length - deck.length;
    const seats: FixtureSeat[] = Array.from({ length: np }, (_, i) => ({ id: `seat-${i}`, name: `P${i + 1}`, brain: brain(i) }));
    let f = fixture().title('fuzz').seats(seats).status(L.GAME_STATUS_PLAYING)
        .attacker(inSeats[0]).defender(inSeats[1]).awaiting(inSeats[0])
        .table(...battles).deck(text(deck)).discard(discard).eliminated(...out).deterministic(true);
    f = flipped ? f.trump(cardText(flipped)) : f.powerSuit(ri(4));
    hands.forEach((h, i) => { f = f.hand(i, text(h)).seatStatus(i, out.includes(i) ? OUT : IN); });
    try {
        return f.build();
    } catch (e) {
        if (e instanceof FixtureRefused) return null;
        throw e;
    }
}

const bareHex = (b: Uint8Array): string => Buffer.from(b).toString('hex');

function refused(what: string, rc: number): Error {
    return new Error(`kernel_board: ${what} refused: ${tableCodeName(rc, ['TABLE_E_', 'GAME_INVALID_', 'ROSTER_E_'])} (${rc})`);
}

/** A lobby of `seats` (humans and bots), every seat ready, dealt from `seed` by table_ready and committed. */
export function dealTable(seats: FixtureSeat[], seed: Uint8Array, opts: { gameId?: string; title?: string; table?: ServerTable; nowMs?: number } = {}): BotTableRow {
    const table = opts.table ?? fixtureTable();
    const gameId = opts.gameId ?? 'g';
    let f = fixture().title(opts.title ?? gameId).seats(seats);
    seats.forEach((_, i) => { f = f.seatStatus(i, READY); });
    const lobby = f.build();
    let rc = table.load(lobby.state, lobby.roster);
    if (rc < 0) throw refused('lobby load', rc);
    rc = table.ready(seats[0].id, seed);
    if (rc !== L.TABLE_OK) throw refused('ready', rc);
    const p = table.commit(gameId, 1, opts.nowMs ?? 1_700_000_000_000);
    if (typeof p === 'number') throw refused('commit', p);
    return { gameId, version: 1, state: p.state, roster: p.roster, seedHex: bareHex(seed), log: p.logs ?? new Uint8Array(0), status: p.status, fool: p.fool };
}

/** One move by the seat `actorId` through table_act, committed; the rc when it did not apply. */
export function tableMove(row: BotTableRow, actorId: string, wire: Uint8Array, opts: { table?: ServerTable; nowMs?: number } = {}): BotTableRow | number {
    const table = opts.table ?? fixtureTable();
    let rc = table.load(row.state, row.roster);
    if (rc < 0) throw refused('load', rc);
    rc = table.setDealSeed(row.seedHex);
    if (rc < 0) throw refused('deal seed', rc);
    rc = table.act(actorId, wire, null, 0);
    if (rc !== L.TABLE_APPLIED) return rc;
    const p = table.commit(row.gameId, row.version + 1, opts.nowMs ?? 1_700_000_000_000 + row.version * 1000);
    if (typeof p === 'number') throw refused('commit', p);
    const log = p.logs === null ? row.log : p.logsReset ? p.logs : Buffer.concat([row.log, p.logs]);
    return { ...row, version: row.version + 1, state: p.state, roster: p.roster, log: new Uint8Array(log), status: p.status, fool: p.fool };
}
