// table_golden.ts - the frozen record of the Phase 3 equivalence gate
// (docs/C_GAME_SHAPE_MIGRATION.md Phase 4b, e2e/table_parity.test.ts).
//
// Until Phase 4b, table_parity drove every request through today's TS pipeline
// and through the C Table side by side and asserted byte equality of every
// product. Phase 4b deletes the TS pipeline. Before it did, the gate ran once
// more with a recorder on: every request it drove (actor, wire or lobby body),
// every outcome, and a digest of every product the C side wrote - each one
// asserted equal to the TS side's in that same run - went into
// e2e/fixtures/table_parity/golden.json. The frozen test replays those requests
// through the C Table and holds each digest, so the equivalence evidence stays a
// test and not a memory.
//
// The digest is SHA-256 over every product in a fixed order, each field framed
// by its name and length, so two different product sets cannot collide by
// concatenation.

import { createHash } from 'node:crypto';
import type { ServerTable, TableProducts } from '../../sdk/ts/table/server_table.ts';

export const hexOf = (b: Uint8Array): string => Buffer.from(b).toString('hex');
export const bytesOf = (h: string): Uint8Array => Uint8Array.from(Buffer.from(h, 'hex'));

/**
 * Every product of a commit on `table`, and the pushes for every human seat and
 * the spectator, in seat order. `withRankings`: also the finish order (a game
 * that just ended).
 */
export function productsDigest(table: ServerTable, gameId: string, p: TableProducts, withRankings: boolean): string {
    const h = createHash('sha256');
    const field = (name: string, value: string) => {
        h.update(`${name}:${value.length}:`);
        h.update(value);
        h.update(';');
    };
    field('state', hexOf(p.state));
    field('roster', hexOf(p.roster));
    field('logs', p.logs ? hexOf(p.logs) : '-');
    field('scalars', JSON.stringify([p.status, p.fool, p.numPlayers, p.nEvents, p.needsBots, p.closedRound, p.logsReset,
        p.ended, p.dealtNow, p.rosterChanged]));
    field('table', JSON.stringify([table.needsBots(), table.botsNeedLogs()]));
    p.views.forEach((v, s) => field(`view${s}`, v ? hexOf(v) : '-'));
    field('spectator', hexOf(p.spectator));
    const seats = table.seats();
    for (const viewer of [...seats.flatMap((s, i) => (s.brain ? [] : [i])), -1]) {
        const push = table.push(gameId, viewer);
        field(`push${viewer}`, typeof push === 'number' ? `refused ${push}` : hexOf(push));
    }
    if (withRankings) field('rankings', JSON.stringify(table.rankings()));
    return h.digest('hex');
}

// ---- the record ---------------------------------------------------------------

export type GoldenOutcome = 'applied' | 'rejected' | 'moot' | 'not_seated' | 'wire';

export interface GoldenMove {
    /** The actor as the request named it (a non-string actor crosses as ''). */
    actor: string;
    wire: string;
    outcome: GoldenOutcome;
    reason?: number;
    /** The action response body, for the outcomes that write one. */
    response?: string;
    /** The version the commit wrote (applied only). */
    version?: number;
    digest?: string;
    ended?: boolean;
}

export interface GoldenDrive {
    label: string;
    gameId: string;
    version: number;
    state: string;
    roster: string;
    moves: GoldenMove[];
}

export interface GoldenLobbyEdit {
    label: string;
    actor: string;
    actorName: string;
    body: Record<string, unknown>;
    /** The C Table's result code. */
    rc: number;
    /** A decided divergence (plan Q9): TS applied what C refuses. */
    expect?: string;
    version?: number;
    digest?: string;
}

export interface GoldenLobby {
    label: string;
    gameId: string;
    /** The starting table: create's products, or a fixture lobby's blobs. */
    create?: { actor: string; name: string; digest: string };
    version: number;
    state: string;
    roster: string;
    edits: GoldenLobbyEdit[];
    /** The dealt lobby played on through the move chain. */
    drive?: GoldenDrive;
    /** Edits after the drive. */
    after?: GoldenLobbyEdit[];
}

export interface GoldenRow {
    id: string;
    status: string;
    version: number;
    state: string;
    roster: string;
    /** A WAITING row's stale finished blob (the damaged shape the expand migration repairs). */
    staleState?: string;
    digest: string;
}

export interface Golden {
    format: 1;
    now: number;
    dealSeed: string;
    botRows: { id: string; nickname: string; strategy_key: string }[];
    rows: GoldenRow[];
    fixtureDrives: GoldenDrive[];
    fuzzDrives: GoldenDrive[];
    lobbies: GoldenLobby[];
    counts: Record<string, unknown>;
}
