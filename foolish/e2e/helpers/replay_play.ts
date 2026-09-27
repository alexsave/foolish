// replay_play.ts - a bots-only game played by the kernel, with what its players
// were really served along the way: the truth a replay of it is held against.
//
// The replay suites used to hold a code against the TypeScript Game that played
// it (its hands, its logs, its discard pile). There is no such object now: the
// game is a C Table played through its bot cycle (./bot_table.ts), and what the
// game WAS is read the way a client reads it, from the table itself -
//
//   - every live push the spectator channel was sent (table_push), read by the
//     client slot into PushEvents and boards, in order: what happened;
//   - the closing board as each seat and a spectator are served it
//     (table_envelope), read by the client slot: where the game ended, each
//     seat's own hand face up.
//
// A replay is a different path to the same boards (replay_steps.c rebuilds the
// game from the code and plays it through the engine), so a replay that agrees
// with these agrees with the game that was played. Nothing here knows a byte
// layout. Pure kernel: no Postgres.

import { clientTable, type PushEvent, type TableView } from '../../sdk/ts/table/client_table.ts';
import type { ServerTable } from '../../sdk/ts/table/server_table.ts';
import * as V from '../../sdk/ts/gen/view_layout.bots.ts';
import { botCycle, dealBotTable, replayCodeOf, type BotTableOptions, type BotTableRow, type PlayedBotTable } from './bot_table.ts';
import { fixtureTable } from './table_fixture.ts';
import * as L from '../../sdk/ts/gen/game_layout.bots.ts';

export interface RecordedGame extends PlayedBotTable {
    /** Every event the spectator channel was pushed, deal first, in order. */
    events: PushEvent[];
    /** The closing board as each seat is served it (index = seat): its own hand face up. */
    seatViews: TableView[];
    /** The closing board as a spectator is served it. */
    spectatorView: TableView;
}

/** The spectator push of the operation the table just committed, read by the client slot. */
function spectatorPush(table: ServerTable, row: BotTableRow): PushEvent[] {
    const bytes = table.push(row.gameId, -1);
    if (typeof bytes === 'number') throw new Error(`replay_play: push refused (${bytes})`);
    const read = clientTable().readPush(bytes, { as3: true, identity: 'none' });
    if (!read) throw new Error(`replay_play: the push at version ${row.version} did not read (${clientTable().lastRefusal().code})`);
    return read.steps.map((s) => s.event);
}

function envelopeView(table: ServerTable, row: BotTableRow, viewer: number): TableView {
    const bytes = table.envelope(row.gameId, viewer, row.version);
    if (typeof bytes === 'number') throw new Error(`replay_play: envelope refused (${bytes})`);
    const view = clientTable().adoptEnvelope(bytes);
    if (!view) throw new Error(`replay_play: the envelope for ${viewer} did not read (${clientTable().lastRefusal().code})`);
    return view;
}

/** A seeded bots-only game played to its end by the kernel, with its pushes and closing boards, not yet encoded. */
export function recordBotTable(brains: string[], seed: Uint8Array, opts: BotTableOptions = {}): Omit<RecordedGame, 'code'> {
    const table = opts.table ?? fixtureTable();
    let row = dealBotTable(brains, seed, { ...opts, table });
    const events = spectatorPush(table, row);
    let actions = 0;
    for (let cycle = 0; row.status === L.GAME_STATUS_PLAYING; cycle++) {
        if (cycle > 5000) throw new Error(`replay_play: ${row.gameId} did not end`);
        const c = botCycle(row, { ...opts, table });
        if (c.drive.n === 0) throw new Error(`replay_play: ${row.gameId} is playing and no bot has a move`);
        row = c.row;
        actions += c.drive.n;
        events.push(...spectatorPush(table, row));
    }
    const rc = table.load(row.state, row.roster);
    if (rc < 0) throw new Error(`replay_play: the final state does not load (${rc})`);
    const seatViews = brains.map((_, s) => envelopeView(table, row, s));
    const spectatorView = envelopeView(table, row, -1);
    return { ...row, seed, actions, events, seatViews, spectatorView };
}

/** A seeded bots-only game, played to its end by the kernel, with its pushes, closing boards and v6 code. */
export function playRecorded(brains: string[], seed: Uint8Array, opts: BotTableOptions = {}): RecordedGame {
    const played = recordBotTable(brains, seed, opts);
    return { ...played, code: replayCodeOf(played, seed, { table: opts.table ?? fixtureTable() }) };
}

/** The moves a game's pushes show, by kind: one ATTACK_PASS event per attack or pass, told apart by its message. */
export function movesOf(events: PushEvent[]): { attack: number; pass: number; cover: number; pickup: number } {
    const n = (type: number, msg?: number) => events.filter((e) => e.type === type && (msg === undefined || e.msg === msg)).length;
    return {
        attack: n(V.EVW_T_ATTACK_PASS, V.EVW_MSG_ATTACKED),
        pass: n(V.EVW_T_ATTACK_PASS, V.EVW_MSG_PASSED),
        cover: n(V.EVW_T_COVER),
        pickup: n(V.EVW_T_PICKUP),
    };
}

/** A card as a sortable key. */
export const cardKey = (c: { suit: number; value: number }): string => `${c.suit}-${c.value}`;
