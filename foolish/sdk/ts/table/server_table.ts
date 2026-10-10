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

import type { BotsExports } from '../gen/exports.bots.ts';
import * as L from '../gen/game_layout.bots.ts';
import { LAYOUT_HASH } from '../gen/layout_hash.bots.ts';
import { assertLayoutHash } from '../wasm/layout_hash.ts';
import { loadWasmGz, loadWasmGzAsync } from '../wasm/wasm_asset.ts';

/** One recorded bot SEARCH, as the kernel saw it (c/src/bot_drive.h BeliefProbe). */
export interface BeliefProbeRecord {
    seat: number;
    /** Log records spliced into the Game the strategy was about to read. */
    nLogs: number;
    /** Real cards visible in that log - `${suit}:${value}` ids. */
    cards: Set<string>;
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

/** A decoded action request: the game id, the action wire, and the intent version when the request carried one. */
export interface TableActionRequest {
    gameId: string;
    wire: Uint8Array;
    intent: number | null;
}

/** A decoded ask (ask.h TableAsk): a request that is not a move, as the kernel read its body. */
export type TableAsk = L.TableAsk_Snap;

/** The bot row an add-bot ask seats (table.h TableAskBot). */
export interface TableAskBot { id: string; nickname: string; brain: string }

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
    /**
     * The operation moved the goods, so a push must go out even with no events.
     * A `good` flies no card and therefore emits none; gating the broadcast on
     * `nEvents > 0` alone meant a good was never sent on its own.
     */
    goodsChanged: boolean;
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
    private readonly ex: BotsExports;
    private mem: L.Mem;

    constructor(ex: BotsExports) {
        this.ex = ex;
        this.mem = L.memOf(ex.memory.buffer);
    }

    private m(): L.Mem {
        if (this.mem.u8.buffer !== this.ex.memory.buffer) this.mem = L.memOf(this.ex.memory.buffer);
        return this.mem;
    }

    private io(): number { return this.ex.wasm_io_ptr(); }

    /** Writes byte strings (a string as its UTF-8) back to back at the IO buffer; returns their lengths. */
    private put<P extends (Uint8Array | string)[]>(...parts: P): { [K in keyof P]: number } {
        const bytes = parts.map((p) => (typeof p === 'string' ? enc.encode(p) : p));
        const cap = this.ex.wasm_io_cap();
        const total = bytes.reduce((n, p) => n + p.length, 0);
        if (total > cap) throw new RangeError(`table: ${total} input bytes exceed the IO buffer (${cap})`);
        const u8 = this.m().u8;
        let at = this.io();
        return bytes.map((p) => { u8.set(p, at); at += p.length; return p.length; }) as { [K in keyof P]: number };
    }

    /** The `n` bytes a door left at the IO buffer, copied out; a refusal (n < 0) passes through. */
    private out(n: number): Uint8Array | number {
        const at = this.io();
        return n < 0 ? n : this.m().u8.slice(at, at + n);
    }

    /** out() for a door that cannot refuse what this wrapper hands it. */
    private must(what: string, n: number): Uint8Array {
        if (n < 0) throw new Error(`table: ${what} refused (${n})`);
        return this.out(n) as Uint8Array;
    }

    /** table_request_decode: the request's parts, or a TABLE_E_* refusal. */
    requestDecode(body: Uint8Array): TableActionRequest | number {
        const rc = this.ex.wasm_table_request_decode(...this.put(body));
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
        return this.ex.wasm_table_load(...this.put(state, roster));
    }

    /** table_act by the seat whose roster id is `actorId`. `intent` null: the request carried none. */
    act(actorId: string, wire: Uint8Array, intent: number | null, roundEpoch: number): number {
        return this.ex.wasm_table_act(...this.put(actorId, wire), intent ?? -1, roundEpoch);
    }

    // ---- lobby edits (table.h): each returns TABLE_OK / TABLE_EMPTY / TABLE_MOOT, or a refusal.
    // The server runs them through ask(); these are the doors tests and benches build a table with. ----

    /** table_create: a new lobby with the creator seated. */
    create(actorId: string, name: string): number {
        return this.ex.wasm_table_create(...this.put(actorId, name));
    }

    join(actorId: string, name: string): number {
        return this.ex.wasm_table_join(...this.put(actorId, name));
    }

    /** A human leaves, or a seated player removes another human. */
    leave(actorId: string, targetId: string): number {
        return this.ex.wasm_table_leave(...this.put(actorId, targetId));
    }

    /** `dealSeed`: FOOLISH_SEED_LEN bytes from crypto, used if the bot makes every seat ready. */
    addBot(actorId: string, botId: string, nickname: string, brain: string, dealSeed: Uint8Array): number {
        if (dealSeed.length !== L.FOOLISH_SEED_LEN) throw new RangeError('table: a deal seed is 32 bytes');
        const [a, b, n, k] = this.put(actorId, botId, nickname, brain, dealSeed);
        return this.ex.wasm_table_add_bot(a, b, n, k);
    }

    /** `dealSeed`: FOOLISH_SEED_LEN bytes from crypto, used if every seat is now ready. */
    ready(actorId: string, dealSeed: Uint8Array): number {
        if (dealSeed.length !== L.FOOLISH_SEED_LEN) throw new RangeError('table: a deal seed is 32 bytes');
        return this.ex.wasm_table_ready(this.put(actorId, dealSeed)[0]);
    }

    continueGame(actorId: string): number {
        return this.ex.wasm_table_continue(...this.put(actorId));
    }

    /** New card i is old card indices[i], of the actor's own hand. */
    rearrangeHand(actorId: string, indices: number[]): number {
        if (!indices.every((i) => Number.isInteger(i) && i >= 0 && i <= 255)) return L.TABLE_E_WIRE;
        return this.ex.wasm_table_rearrange_hand(...this.put(actorId, Uint8Array.from(indices)));
    }

    redact(userId: string, name: string): number {
        return this.ex.wasm_table_redact(...this.put(userId, name));
    }

    /** The ENGINE_REJECT_* behind the last TABLE_REJECTED. */
    reject(): number { return this.ex.wasm_table_reject(); }

    /** The ROSTER_E_* behind the last TABLE_E_ROSTER. */
    detail(): number { return this.ex.wasm_table_detail(); }

    needsBots(): boolean { return this.ex.wasm_table_needs_bots() !== 0; }

    botsNeedLogs(): boolean { return this.ex.wasm_table_bots_need_logs() !== 0; }

    /** table_seat_of: the seat whose roster id is exactly `actorId`, or -1 (or TABLE_E_NOT_LOADED). */
    seatOf(actorId: string): number {
        return this.ex.wasm_table_seat_of(...this.put(actorId));
    }

    // ---- the bot cycle (table.h): after load, in one kernel section ----

    /** table_set_deal_seed: the loaded game's deal seed as its stored hex text (null: none). */
    setDealSeed(seedHex: string | null): number {
        return this.ex.wasm_table_set_deal_seed(...this.put(seedHex ?? ''));
    }

    /**
     * table_set_session_log: the row's stored session log (games.logs_packed bytes).
     * A bot cycle ALWAYS hands it over - its length is the progress every bot decision
     * is seeded from - and the kernel reads the records onto the board only for a brain
     * that consults them. Records the log holds, or a refusal.
     */
    setSessionLog(log: Uint8Array): number {
        return this.ex.wasm_table_set_session_log(...this.put(log));
    }

    /** table_bot_drive, offered `prefs` (a drivePrefs() blob of a failed attempt, or null). A TableDrive, or a refusal. */
    botDrive(prefs: Uint8Array | null, maxActions = 0): TableDrive | number {
        const n = this.ex.wasm_table_bot_drive(...this.put(prefs ?? ''), maxActions);
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
    drivePrefs(): Uint8Array { return this.must('drive prefs', this.ex.wasm_table_drive_prefs()); }

    /** table_bot_wait_ms: ms from `nowMs` to wait before driving the loaded board (0: drive now), or a refusal. */
    botWaitMs(nowMs: number): number { return this.ex.wasm_table_bot_wait_ms(nowMs); }

    // ---- the end of a game ----

    /** table_replay_code: the verified v6 replay code of the loaded finished game, or a refusal. */
    replayCode(seed: Uint8Array, log: Uint8Array): Uint8Array | number {
        return this.out(this.ex.wasm_table_replay_code(...this.put(seed, log)));
    }

    /** table_replay_extras: the replay extras blob (names from the roster, times from the log), or a refusal. */
    replayExtras(log: Uint8Array): Uint8Array | number {
        return this.out(this.ex.wasm_table_replay_extras(...this.put(log)));
    }

    /** The module's linear memory, for the edge memory log line. */
    memoryBytes(): number { return this.ex.memory.buffer.byteLength; }

    /**
     * Test observability: arm the belief probe of THIS instance.
     *
     * What a bot SEARCH saw of the session log, recorded by the kernel as the
     * strategy was about to read it (c/src/bot_drive.h BeliefProbe). A spy on this
     * side could only prove the log's bytes were handed over, never that they were
     * spliced into the Game the strategy read - and since the choose step moved
     * in-kernel there is no TS seam left to spy on. Off until this is called.
     */
    __beliefProbeReset(): void { this.ex.wasm_belief_probe_reset(); }

    /** The searches recorded since the last reset, in order, read through the generated accessors. */
    __beliefProbeDump(): BeliefProbeRecord[] {
        const m = this.m();
        const base = this.ex.wasm_belief_probe_ptr();
        const out: BeliefProbeRecord[] = [];
        for (let i = 0; i < this.ex.wasm_belief_probe_dump(); i++) {
            const p = base + i * L.BeliefProbe_SIZE;
            const bits = L.BeliefProbe_get_cards(m, p);
            const cards = new Set<string>();
            // bit (suit * 16 + value) per real card the log showed.
            for (let code = 0n; code < 64n; code++) {
                if ((bits >> code) & 1n) cards.add(`${Number(code / 16n)}:${Number(code % 16n)}`);
            }
            out.push({ seat: L.BeliefProbe_get_seat(m, p), nLogs: L.BeliefProbe_get_n_logs(m, p), cards });
        }
        return out;
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
        const n = this.ex.wasm_table_commit_products(...this.put(gameId), nextVersion, nowMs);
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
            goodsChanged: L.TableCommit_get_goods_changed(m, c),
            state: bytes(L.TableCommit_state_at(c)),
            roster: bytes(L.TableCommit_roster_at(c)),
            logs: L.Span_get_len(m, logs) > 0 ? bytes(logs) : null,
            views,
            spectator: bytes(L.TableCommit_spectator_at(c)),
        };
    }

    /** table_push: one viewer's as3 payload (seat, or -1 for the spectator channel), or a refusal. */
    push(gameId: string, viewer: number): Uint8Array | number {
        return this.out(this.ex.wasm_table_push(...this.put(gameId), viewer));
    }

    /** table_envelope: one viewer's response envelope at `version`, or a refusal. */
    envelope(gameId: string, viewer: number, version: number): Uint8Array | number {
        return this.out(this.ex.wasm_table_envelope(...this.put(gameId), viewer, version));
    }

    /** table_action_response: the `action` endpoint's response body for a table_act result. */
    actionResponse(result: number, reject: number, version: number): Uint8Array {
        return this.must('action response', this.ex.wasm_table_action_response(result, reject, version));
    }

    // ---- an ask (ask.h): every request that is not a move ----

    /** ask_decode: what a request body asks, or TABLE_E_WIRE. A copy: it survives later kernel calls. */
    askDecode(body: Uint8Array): TableAsk | number {
        if (body.length > this.ex.wasm_io_cap()) return L.TABLE_E_WIRE;
        const rc = this.ex.wasm_table_ask_decode(...this.put(body));
        return rc < 0 ? rc : L.readTableAsk(this.m(), this.ex.wasm_table_ask_ptr());
    }

    /**
     * table_ask: runs the ask `body` holds on the loaded table as `actorId`, whose
     * display name is `name`. `bot`: the row an add-bot seats. The edit's own result.
     */
    ask(body: Uint8Array, actorId: string, name: string, dealSeed: Uint8Array, bot: TableAskBot | null = null): number {
        if (dealSeed.length !== L.FOOLISH_SEED_LEN) throw new RangeError('table: a deal seed is 32 bytes');
        const [b, a, n, , i, k, r] = this.put(body, actorId, name, dealSeed, bot?.id ?? '', bot?.nickname ?? '', bot?.brain ?? '');
        return this.ex.wasm_table_ask(b, a, n, i, k, r);
    }

    /** ask_refusal_encode: the body of a reply that has no answer (ASK_WHY_*, and the kernel's codes behind it). */
    refusal(why: number, code = 0, detail = 0): Uint8Array {
        return this.must('refusal', this.ex.wasm_ask_refusal_encode(why, code, detail));
    }

    /** table_cast: one viewer's realtime message for the last operation, carrying `version`, or a refusal. */
    cast(gameId: string, viewer: number, version: number): Uint8Array | number {
        return this.out(this.ex.wasm_table_cast(...this.put(gameId), viewer, version));
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
    // Every commit asks, so the generated names are read once, not per call.
    statusLabels ??= new Map(Object.entries(L)
        .filter(([name, value]) => name.startsWith('GAME_STATUS_') && typeof value === 'number')
        .map(([name, value]) => [value as number, name.slice('GAME_STATUS_'.length).toLowerCase()]));
    const label = statusLabels.get(status);
    if (label === undefined) throw new RangeError(`table: ${status} is not a GAME_STATUS`);
    return label;
}
let statusLabels: Map<number, string> | null = null;

/** A table over a PRIVATE bots.wasm instance: nothing else shares its resident slot. */
export function createServerTable(): ServerTable {
    const inst = new WebAssembly.Instance(new WebAssembly.Module(loadWasmGz('bots') as BufferSource), {});
    const ex = inst.exports as unknown as BotsExports;
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
