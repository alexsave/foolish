// server_table.ts - the C Table (c/src/table.h) for a server, over bots.wasm.
//
// A thin, synchronous call-and-copy wrapper. It knows the export names, the
// generated accessor names in sdk/ts/gen/game_layout.bots.ts, and nothing else:
// no byte layout, no seat rule, no status string. Every input it hands the
// kernel is opaque bytes it was given (a state blob, a durable roster, an action
// wire) or a UTF-8 string (an auth id, a game id); every output is bytes copied
// out of the module or a number read through a generated accessor.
//
// ONE KERNEL SECTION PER OPERATION. The table's board is the module's resident
// Game. A caller runs load, then an operation, then copies every product out
// (commit, push), all before its next await. Nothing returned here points into
// wasm memory: products are copies, so they survive later kernel calls.
//
// Server and tests only. The browser must never reach this module: it hands out
// the unmasked state blob (e2e/security_client_boundary.test.ts denies it).
// No production caller yet: the server moves onto it in Phase 4b
// (docs/C_GAME_SHAPE_MIGRATION.md).

import * as L from '../gen/game_layout.bots.ts';
import { LAYOUT_HASH } from '../gen/layout_hash.bots.ts';
import { assertLayoutHash } from '../wasm/layout_hash.ts';
import { loadWasmGz } from '../wasm/wasm_asset.ts';

/** The bots.wasm exports this wrapper calls (c/wasm/wasm_table_api.c). */
export interface TableExports {
    memory: WebAssembly.Memory;
    wasm_init(): void;
    wasm_layout_hash(): number;
    wasm_io_ptr(): number;
    wasm_io_cap(): number;
    wasm_table_roster_ptr(): number;
    wasm_table_commit_ptr(): number;
    wasm_table_request_ptr(): number;
    wasm_table_elo_ptr(): number;
    wasm_table_detail(): number;
    wasm_table_reject(): number;
    wasm_table_request_decode(len: number): number;
    wasm_table_load(stateLen: number, rosterLen: number): number;
    wasm_table_act(idLen: number, wireLen: number, intent: number, roundEpoch: number): number;
    wasm_table_needs_bots(): number;
    wasm_table_bots_need_logs(): number;
    wasm_table_commit_products(gidLen: number, nextVersion: number, nowMs: number): number;
    wasm_table_push(gidLen: number, viewer: number): number;
    wasm_table_envelope(gidLen: number, viewer: number, version: number): number;
    wasm_table_action_response(result: number, reject: number, version: number): number;
    wasm_table_rankings(): number;
    wasm_elo_deltas(n: number): number;
}

/** A decoded action request: the game id, the action wire, and the intent version when the request carried one. */
export interface TableActionRequest {
    gameId: string;
    wire: Uint8Array;
    intent: number | null;
}

/** Every product of a commit (table.h TableCommit), copied out of the module. */
export interface TableProducts {
    /** GAME_STATUS_* */
    status: number;
    fool: number;
    numPlayers: number;
    nEvents: number;
    needsBots: boolean;
    closedRound: boolean;
    logsReset: boolean;
    ended: boolean;
    dealtNow: boolean;
    rosterChanged: boolean;
    state: Uint8Array;
    roster: Uint8Array;
    /** The operation's session-log records, or null when it wrote none. */
    logs: Uint8Array | null;
    /** The response envelope per seat; null for a bot seat. */
    views: (Uint8Array | null)[];
    spectator: Uint8Array;
}

/** One seat of the loaded roster. */
export interface TableSeat {
    id: string;
    name: string;
    /** The bot_roster key; '' for a human. */
    brain: string;
}

const enc = new TextEncoder();

export class ServerTable {
    private readonly ex: TableExports;
    private mem: L.Mem;

    constructor(ex: TableExports) {
        this.ex = ex;
        this.mem = L.memOf(ex.memory.buffer);
    }

    private m(): L.Mem {
        if (this.mem.u8.buffer !== this.ex.memory.buffer) this.mem = L.memOf(this.ex.memory.buffer);
        return this.mem;
    }

    private io(): number { return this.ex.wasm_io_ptr(); }

    /** Writes byte strings back to back at the IO buffer; returns their lengths. */
    private put(...parts: Uint8Array[]): number[] {
        const u8 = this.m().u8;
        let at = this.io();
        const cap = this.ex.wasm_io_cap();
        const total = parts.reduce((n, p) => n + p.length, 0);
        if (total > cap) throw new RangeError(`table: ${total} input bytes exceed the IO buffer (${cap})`);
        return parts.map((p) => { u8.set(p, at); at += p.length; return p.length; });
    }

    private out(len: number): Uint8Array {
        const at = this.io();
        return this.m().u8.slice(at, at + len);
    }

    /** table_request_decode: the request's parts, or a TABLE_E_* refusal. */
    requestDecode(body: Uint8Array): TableActionRequest | number {
        const [len] = this.put(body);
        const rc = this.ex.wasm_table_request_decode(len);
        if (rc < 0) return rc;
        const m = this.m();
        const q = this.ex.wasm_table_request_ptr();
        const gidOff = L.TableRequest_get_gid_off(m, q);
        const wireOff = L.TableRequest_get_wire_off(m, q);
        return {
            gameId: new TextDecoder().decode(body.subarray(gidOff, gidOff + L.TableRequest_get_gid_len(m, q))),
            wire: body.slice(wireOff, wireOff + L.TableRequest_get_wire_len(m, q)),
            intent: L.TableRequest_get_has_intent(m, q) ? L.TableRequest_get_intent(m, q) : null,
        };
    }

    /** table_load: TABLE_OK, or a GAME_INVALID_* / TABLE_E_* refusal (see detail()). */
    load(state: Uint8Array, roster: Uint8Array): number {
        const [s, r] = this.put(state, roster);
        return this.ex.wasm_table_load(s, r);
    }

    /** table_act by the seat whose roster id is `actorId`. `intent` null: the request carried none. */
    act(actorId: string, wire: Uint8Array, intent: number | null, roundEpoch: number): number {
        const [i, w] = this.put(enc.encode(actorId), wire);
        return this.ex.wasm_table_act(i, w, intent ?? -1, roundEpoch);
    }

    /** The ENGINE_REJECT_* behind the last TABLE_REJECTED. */
    reject(): number { return this.ex.wasm_table_reject(); }

    /** The ROSTER_E_* behind the last TABLE_E_ROSTER. */
    detail(): number { return this.ex.wasm_table_detail(); }

    needsBots(): boolean { return this.ex.wasm_table_needs_bots() !== 0; }

    botsNeedLogs(): boolean { return this.ex.wasm_table_bots_need_logs() !== 0; }

    /** The loaded roster's seats, in seat order. */
    seats(): TableSeat[] {
        const m = this.m();
        const r = this.ex.wasm_table_roster_ptr();
        const out: TableSeat[] = [];
        for (let s = 0; s < L.Roster_get_n(m, r); s++) {
            const p = L.Roster_seats_at(r, s);
            out.push({ id: L.RosterSeat_get_id_str(m, p), name: L.RosterSeat_get_name_str(m, p), brain: L.RosterSeat_get_brain_str(m, p) });
        }
        return out;
    }

    /** table_commit_products: every product of the loaded table and its last operation, or a refusal. */
    commit(gameId: string, nextVersion: number, nowMs: number): TableProducts | number {
        const [g] = this.put(enc.encode(gameId));
        const n = this.ex.wasm_table_commit_products(g, nextVersion, nowMs);
        if (n < 0) return n;
        const m = this.m();
        const c = this.ex.wasm_table_commit_ptr();
        const bytes = (p: number): Uint8Array => {
            const off = this.io() + L.Span_get_off(m, p);
            return m.u8.slice(off, off + L.Span_get_len(m, p));
        };
        const numPlayers = L.TableCommit_get_num_players(m, c);
        const views: (Uint8Array | null)[] = [];
        for (let s = 0; s < numPlayers; s++) {
            const p = L.TableCommit_views_at(c, s);
            views.push(L.Span_get_len(m, p) > 0 ? bytes(p) : null);
        }
        const logs = L.TableCommit_logs_at(c);
        return {
            status: L.TableCommit_get_status(m, c),
            fool: L.TableCommit_get_fool(m, c),
            numPlayers,
            nEvents: L.TableCommit_get_n_events(m, c),
            needsBots: L.TableCommit_get_needs_bots(m, c),
            closedRound: L.TableCommit_get_closed_round(m, c),
            logsReset: L.TableCommit_get_logs_reset(m, c),
            ended: L.TableCommit_get_ended(m, c),
            dealtNow: L.TableCommit_get_dealt_now(m, c),
            rosterChanged: L.TableCommit_get_roster_changed(m, c),
            state: bytes(L.TableCommit_state_at(c)),
            roster: bytes(L.TableCommit_roster_at(c)),
            logs: L.Span_get_len(m, logs) > 0 ? bytes(logs) : null,
            views,
            spectator: bytes(L.TableCommit_spectator_at(c)),
        };
    }

    /** table_push: one viewer's as3 payload (seat, or -1 for the spectator channel), or a refusal. */
    push(gameId: string, viewer: number): Uint8Array | number {
        const [g] = this.put(enc.encode(gameId));
        const n = this.ex.wasm_table_push(g, viewer);
        return n < 0 ? n : this.out(n);
    }

    /** table_envelope: one viewer's response envelope at `version`, or a refusal. */
    envelope(gameId: string, viewer: number, version: number): Uint8Array | number {
        const [g] = this.put(enc.encode(gameId));
        const n = this.ex.wasm_table_envelope(g, viewer, version);
        return n < 0 ? n : this.out(n);
    }

    /** table_action_response: the `action` endpoint's response body for a table_act result. */
    actionResponse(result: number, reject: number, version: number): Uint8Array {
        const n = this.ex.wasm_table_action_response(result, reject, version);
        if (n < 0) throw new Error(`table: action response refused (${n})`);
        return this.out(n);
    }

    /** table_rankings: seats, best first. */
    rankings(): number[] {
        const n = this.ex.wasm_table_rankings();
        if (n < 0) throw new Error(`table: rankings refused (${n})`);
        const m = this.m();
        const e = this.ex.wasm_table_elo_ptr();
        return Array.from({ length: n }, (_, i) => L.TableElo_get_order(m, e, i));
    }

    /** elo_deltas: each seat's rating change, from ratings by seat and the finish order (seats, best first). */
    eloDeltas(ratingsBySeat: number[], order: number[]): number[] {
        const m = this.m();
        const e = this.ex.wasm_table_elo_ptr();
        if (ratingsBySeat.length > L.TableElo_ratings_LEN || order.length !== ratingsBySeat.length) {
            throw new RangeError('table: elo input does not fit');
        }
        ratingsBySeat.forEach((r, s) => L.TableElo_set_ratings(m, e, s, r));
        order.forEach((s, i) => L.TableElo_set_order(m, e, i, s));
        const n = this.ex.wasm_elo_deltas(order.length);
        if (n < 0) throw new Error(`table: elo refused (${n})`);
        return Array.from({ length: n }, (_, s) => L.TableElo_get_deltas(m, e, s));
    }
}

/** A table over a PRIVATE bots.wasm instance: nothing else shares its resident slot. */
export function createServerTable(): ServerTable {
    const inst = new WebAssembly.Instance(new WebAssembly.Module(loadWasmGz('bots') as BufferSource), {});
    const ex = inst.exports as unknown as TableExports;
    assertLayoutHash('bots.wasm', ex, LAYOUT_HASH, 'sdk/ts/gen/layout_hash.bots.ts');
    ex.wasm_init();
    return new ServerTable(ex);
}
