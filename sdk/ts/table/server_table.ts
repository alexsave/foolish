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
// The Supabase server runs every game operation through it since Phase 4b
// (docs/C_GAME_SHAPE_MIGRATION.md; server/impls/supabase/functions/_shared/adapter/table_io.ts).

import * as L from '../gen/game_layout.bots.ts';
import { LAYOUT_HASH } from '../gen/layout_hash.bots.ts';
import { assertLayoutHash } from '../wasm/layout_hash.ts';
import { loadWasmGz, loadWasmGzAsync } from '../wasm/wasm_asset.ts';

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
    wasm_table_create(idLen: number, nameLen: number): number;
    wasm_table_join(idLen: number, nameLen: number): number;
    wasm_table_leave(idLen: number, targetLen: number): number;
    wasm_table_add_bot(idLen: number, botLen: number, nickLen: number, brainLen: number): number;
    wasm_table_remove_bot(idLen: number, botLen: number): number;
    wasm_table_ready(idLen: number): number;
    wasm_table_reseat(idLen: number, idsLen: number): number;
    wasm_table_retitle(idLen: number, titleLen: number): number;
    wasm_table_continue(idLen: number): number;
    wasm_table_rearrange_hand(idLen: number, n: number): number;
    wasm_table_redact(idLen: number, nameLen: number): number;
    wasm_table_seat_of(idLen: number): number;
    wasm_table_set_deal_seed(len: number): number;
    wasm_table_import_session_log(len: number): number;
    wasm_table_bot_drive(prefsLen: number, maxActions: number): number;
    wasm_table_drive_ptr(): number;
    wasm_table_drive_prefs(): number;
    wasm_table_cycle_delay_ms(): number;
    wasm_table_replay_code(seedLen: number, logLen: number): number;
    wasm_table_replay_extras(logLen: number): number;
    wasm_belief_probe_reset(): void;
    wasm_belief_probe_dump(): number;
}

/** One bot-loop cycle (table.h table_bot_drive): what it applied, and why it stopped. */
export interface TableDrive {
    /** Actions applied; 0 means nothing to commit. */
    n: number;
    /** BOT_STOP_* */
    stop: number;
    /** The acting seat of each applied action, in order (for logs). */
    seats: number[];
}

/** A deal seed: the bytes the host draws from crypto for a lobby edit that may deal. */
export const TABLE_DEAL_SEED_BYTES = 32;

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

    // ---- lobby edits (table.h): each returns TABLE_OK / TABLE_EMPTY / TABLE_MOOT, or a refusal ----

    /** table_create: a new lobby with the creator seated. */
    create(actorId: string, name: string): number {
        return this.ex.wasm_table_create(...this.put(enc.encode(actorId), enc.encode(name)) as [number, number]);
    }

    join(actorId: string, name: string): number {
        return this.ex.wasm_table_join(...this.put(enc.encode(actorId), enc.encode(name)) as [number, number]);
    }

    /** A human leaves, or a seated player removes another human. */
    leave(actorId: string, targetId: string): number {
        return this.ex.wasm_table_leave(...this.put(enc.encode(actorId), enc.encode(targetId)) as [number, number]);
    }

    /** `dealSeed`: TABLE_DEAL_SEED_BYTES from crypto, used if the bot makes every seat ready. */
    addBot(actorId: string, botId: string, nickname: string, brain: string, dealSeed: Uint8Array): number {
        if (dealSeed.length !== TABLE_DEAL_SEED_BYTES) throw new RangeError('table: a deal seed is 32 bytes');
        const [a, b, n, k] = this.put(enc.encode(actorId), enc.encode(botId), enc.encode(nickname), enc.encode(brain), dealSeed);
        return this.ex.wasm_table_add_bot(a, b, n, k);
    }

    removeBot(actorId: string, botId: string): number {
        return this.ex.wasm_table_remove_bot(...this.put(enc.encode(actorId), enc.encode(botId)) as [number, number]);
    }

    /** `dealSeed`: TABLE_DEAL_SEED_BYTES from crypto, used if every seat is now ready. */
    ready(actorId: string, dealSeed: Uint8Array): number {
        if (dealSeed.length !== TABLE_DEAL_SEED_BYTES) throw new RangeError('table: a deal seed is 32 bytes');
        const [a] = this.put(enc.encode(actorId), dealSeed);
        return this.ex.wasm_table_ready(a);
    }

    /** The new seat order, as the seated ids. */
    reseat(actorId: string, ids: string[]): number {
        const parts = ids.map((id) => enc.encode(id));
        if (parts.some((p) => p.length > 255)) return L.TABLE_E_WIRE;
        const list = new Uint8Array(parts.reduce((n, p) => n + 1 + p.length, 0));
        let at = 0;
        for (const p of parts) { list[at++] = p.length; list.set(p, at); at += p.length; }
        return this.ex.wasm_table_reseat(...this.put(enc.encode(actorId), list) as [number, number]);
    }

    retitle(actorId: string, title: string): number {
        return this.ex.wasm_table_retitle(...this.put(enc.encode(actorId), enc.encode(title)) as [number, number]);
    }

    continueGame(actorId: string): number {
        return this.ex.wasm_table_continue(...this.put(enc.encode(actorId)) as [number]);
    }

    /** New card i is old card indices[i], of the actor's own hand. */
    rearrangeHand(actorId: string, indices: number[]): number {
        if (!indices.every((i) => Number.isInteger(i) && i >= 0 && i <= 255)) return L.TABLE_E_WIRE;
        return this.ex.wasm_table_rearrange_hand(...this.put(enc.encode(actorId), Uint8Array.from(indices)) as [number, number]);
    }

    redact(userId: string, name: string): number {
        return this.ex.wasm_table_redact(...this.put(enc.encode(userId), enc.encode(name)) as [number, number]);
    }

    /** The ENGINE_REJECT_* behind the last TABLE_REJECTED. */
    reject(): number { return this.ex.wasm_table_reject(); }

    /** The ROSTER_E_* behind the last TABLE_E_ROSTER. */
    detail(): number { return this.ex.wasm_table_detail(); }

    needsBots(): boolean { return this.ex.wasm_table_needs_bots() !== 0; }

    botsNeedLogs(): boolean { return this.ex.wasm_table_bots_need_logs() !== 0; }

    /** table_seat_of: the seat whose roster id is exactly `actorId`, or -1 (or TABLE_E_NOT_LOADED). */
    seatOf(actorId: string): number {
        const [i] = this.put(enc.encode(actorId));
        return this.ex.wasm_table_seat_of(i);
    }

    // ---- the bot cycle (table.h): after load, in one kernel section ----

    /** table_set_deal_seed: the loaded game's deal seed as its stored hex text (null: none). */
    setDealSeed(seedHex: string | null): number {
        const [n] = this.put(enc.encode(seedHex ?? ''));
        return this.ex.wasm_table_set_deal_seed(n);
    }

    /** table_import_session_log: the stored session log (games.logs_packed bytes). Records loaded, or a refusal. */
    importSessionLog(log: Uint8Array): number {
        const [n] = this.put(log);
        return this.ex.wasm_table_import_session_log(n);
    }

    /** table_bot_drive, offered `prefs` (a drivePrefs() blob of a failed attempt, or null). A TableDrive, or a refusal. */
    botDrive(prefs: Uint8Array | null, maxActions = 0): TableDrive | number {
        const [p] = this.put(prefs ?? new Uint8Array(0));
        const n = this.ex.wasm_table_bot_drive(p, maxActions);
        if (n < 0) return n;
        const m = this.m();
        const d = this.ex.wasm_table_drive_ptr();
        const count = L.BotDriveOut_get_n(m, d);
        return {
            n: count,
            stop: L.BotDriveOut_get_stop(m, d),
            seats: Array.from({ length: count }, (_, i) => L.BotDriveAction_get_seat(m, L.BotDriveOut_actions_at(d, i))),
        };
    }

    /** table_drive_prefs: the opaque blob to offer a retry of the last cycle. */
    drivePrefs(): Uint8Array {
        const n = this.ex.wasm_table_drive_prefs();
        if (n < 0) throw new Error(`table: drive prefs refused (${n})`);
        return this.out(n);
    }

    /** table_cycle_delay_ms: how long to wait after the last cycle. */
    cycleDelayMs(): number { return this.ex.wasm_table_cycle_delay_ms(); }

    // ---- the end of a game ----

    /** table_replay_code: the verified v6 replay code of the loaded finished game, or a refusal. */
    replayCode(seed: Uint8Array, log: Uint8Array): Uint8Array | number {
        const [s, l] = this.put(seed, log);
        const n = this.ex.wasm_table_replay_code(s, l);
        return n < 0 ? n : this.out(n);
    }

    /** table_replay_extras: the replay extras blob (names from the roster, times from the log), or a refusal. */
    replayExtras(log: Uint8Array): Uint8Array | number {
        const [l] = this.put(log);
        const n = this.ex.wasm_table_replay_extras(l);
        return n < 0 ? n : this.out(n);
    }

    /** The module's linear memory, for the edge memory log line. */
    memoryBytes(): number { return this.ex.memory.buffer.byteLength; }

    /** Test observability: arm the belief probe of THIS instance (bots.ts wasmBeliefProbeReset has the why). */
    __beliefProbeReset(): void { this.ex.wasm_belief_probe_reset(); }

    /** Test observability: the probe's raw records and their count (the layout bots.ts parseBeliefProbe reads). */
    __beliefProbeDump(): { bytes: Uint8Array; n: number } {
        const n = this.ex.wasm_belief_probe_dump();
        return { bytes: this.out(n * 11), n };
    }

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

/**
 * The generated name of a kernel result code among the constant families
 * `prefixes` name ('TABLE_E_', 'ROSTER_E_', 'GAME_INVALID_', ...), for logs and
 * error messages. UNKNOWN(<code>) when no constant has that value.
 */
export function tableCodeName(code: number, prefixes: string[]): string {
    for (const [name, value] of Object.entries(L)) {
        if (value === code && prefixes.some((p) => name.startsWith(p))) return name;
    }
    return `UNKNOWN(${code})`;
}

/**
 * The database's spelling of a GAME_STATUS_* value: the constant's generated
 * name after the prefix, lower case ('waiting', 'playing', 'game_over'), which
 * is how the game_status enum spells its labels in the kernel's order.
 */
export function gameStatusLabel(status: number): string {
    const name = tableCodeName(status, ['GAME_STATUS_']);
    if (name.startsWith('UNKNOWN')) throw new RangeError(`table: ${status} is not a GAME_STATUS`);
    return name.slice('GAME_STATUS_'.length).toLowerCase();
}

/** A table over a PRIVATE bots.wasm instance: nothing else shares its resident slot. */
export function createServerTable(): ServerTable {
    const inst = new WebAssembly.Instance(new WebAssembly.Module(loadWasmGz('bots') as BufferSource), {});
    const ex = inst.exports as unknown as TableExports;
    assertLayoutHash('bots.wasm', ex, LAYOUT_HASH, 'sdk/ts/gen/layout_hash.bots.ts');
    ex.wasm_init();
    return new ServerTable(ex);
}

let shared: Promise<ServerTable> | null = null;

/**
 * The server's one table: a private bots.wasm instance, loaded once per isolate.
 * The bytes are read asynchronously first (Deno must not read a file
 * synchronously inside a request handler), then the module is instantiated.
 */
export function serverTable(): Promise<ServerTable> {
    return (shared ??= loadWasmGzAsync('bots').then(() => createServerTable()));
}
