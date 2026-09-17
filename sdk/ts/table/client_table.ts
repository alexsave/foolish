// client_table.ts - the web client's slot (c/src/client_table.h), over bots.wasm.
//
// A thin, synchronous call-and-copy wrapper, like server_table.ts on the other
// side of the wire. It knows the export names and the snapshot readers
// generated from the C structs (sdk/ts/gen/view_layout.bots.ts), and no byte of
// an envelope, a push, a board or a roster: every input is bytes a server wrote,
// every output a plain snapshot object copied out of the module.
//
// ONE SECTION PER CALL. Each method writes its input into the module, has the
// kernel read it, and copies the answer out before it returns; nothing returned
// points into wasm memory, and nothing here survives an await.
//
// IDENTITY. A move's push names no one (a move cannot change who sits where), so
// the table's identity - opaque bytes the kernel writes - is kept per game id
// from every envelope and roster-carrying push this reads, and handed back to
// read the next push for that game.

import * as V from '../gen/view_layout.bots.ts';
import { __clientKernelExports } from '../wasm/bots.ts';

export type TableView = V.TableView_Snap;
export type PushEvent = V.PushEvent_Snap;
export type ViewSeat = V.ViewSeat_Snap;
export type ViewCard = V.Card_Snap;

/** One step of a push: what moved, and the board it left. */
export interface PushStep { event: PushEvent; view: TableView }

/** A whole push, read. */
export interface PushRead { steps: PushStep[]; final: TableView }

/** The bots.wasm exports this wrapper calls (c/wasm/wasm_table_api.c). */
export interface ClientExports {
    memory: WebAssembly.Memory;
    wasm_io_ptr(): number;
    wasm_io_cap(): number;
    wasm_client_view_ptr(): number;
    wasm_client_event_ptr(): number;
    wasm_client_detail(): number;
    wasm_client_adopt_envelope(len: number): number;
    wasm_client_adopt_resident(viewer: number): number;
    wasm_client_push_open(len: number, identityLen: number, as3: number, version: number): number;
    wasm_client_push_next(): number;
    wasm_client_push_final(): number;
    wasm_client_identity(): number;
    wasm_client_identity_at(): number;
    wasm_client_identity_begin(gidLen: number, titleLen: number): number;
    wasm_client_identity_seat(idLen: number, nameLen: number, isAi: number): number;
}

/** A seat of an identity built from parts (a roster that arrived as JSON). */
export interface IdentitySeat { id: string; name: string; isAi: boolean }

const enc = new TextEncoder();

export class ClientTable {
    private readonly ex: ClientExports;
    private mem: V.Mem;
    private readonly identities = new Map<string, Uint8Array>();
    private refusal = 0;

    constructor(ex: ClientExports) {
        this.ex = ex;
        this.mem = V.memOf(ex.memory.buffer);
    }

    private m(): V.Mem {
        if (this.mem.u8.buffer !== this.ex.memory.buffer) this.mem = V.memOf(this.ex.memory.buffer);
        return this.mem;
    }

    /** Writes byte strings back to back at the IO buffer; false when they do not fit. */
    private put(...parts: Uint8Array[]): boolean {
        const total = parts.reduce((n, p) => n + p.length, 0);
        if (total > this.ex.wasm_io_cap()) return false;
        const u8 = this.m().u8;
        let at = this.ex.wasm_io_ptr();
        for (const p of parts) { u8.set(p, at); at += p.length; }
        return true;
    }

    private refused(rc: number): null {
        this.refusal = rc;
        return null;
    }

    /** Keeps the identity the last read took from its input: the roster trailer, already in those bytes. */
    private keepIdentity(gameId: string, input: Uint8Array): void {
        const at = this.ex.wasm_client_identity_at();
        if (gameId && at >= 0) this.identities.set(gameId, input.slice(at));
    }

    /** The CLIENT_E_* of the last refusal, and the kernel's detail behind it. */
    lastRefusal(): { code: number; detail: number } {
        return { code: this.refusal, detail: this.ex.wasm_client_detail() };
    }

    /** A response envelope (a player_views / spectator_views row, a create or meta response), or null when it does not read whole. */
    adoptEnvelope(bytes: Uint8Array): TableView | null {
        if (!this.put(bytes)) return this.refused(V.CLIENT_E_FORMAT);
        const rc = this.ex.wasm_client_adopt_envelope(bytes.length);
        if (rc !== V.CLIENT_OK) return this.refused(rc);
        const view = V.readTableView(this.m(), this.ex.wasm_client_view_ptr());
        this.keepIdentity(view.gameId, bytes);
        return view;
    }

    /** The game the module holds (an FMSG decode's), as `viewer` sees it; it names no one. */
    adoptResident(viewer: number): TableView | null {
        const rc = this.ex.wasm_client_adopt_resident(viewer);
        if (rc !== V.CLIENT_OK) return this.refused(rc);
        return V.readTableView(this.m(), this.ex.wasm_client_view_ptr());
    }

    /**
     * A realtime push (evwire as2, or as3 with its flags byte), or null when it does
     * not read whole. `identity`: the table's kept identity for `gameId` ('kept', the
     * default), explicit bytes, or 'none' (seats unnamed, as a replay's are). A push
     * that carries a roster names its seats itself.
     */
    readPush(bytes: Uint8Array, opts: { as3: boolean; gameId?: string; version?: number; identity?: 'kept' | 'none' | Uint8Array }): PushRead | null {
        const identity = opts.identity instanceof Uint8Array ? opts.identity
            : opts.identity === 'none' || !opts.gameId ? new Uint8Array(0)
            : this.identities.get(opts.gameId) ?? new Uint8Array(0);
        if (!this.put(bytes, identity)) return this.refused(V.CLIENT_E_PUSH);
        let rc = this.ex.wasm_client_push_open(bytes.length, identity.length, opts.as3 ? 1 : 0, opts.version ?? 0);
        if (rc !== V.CLIENT_OK) return this.refused(rc);
        const m = this.m();
        const viewAt = this.ex.wasm_client_view_ptr(), eventAt = this.ex.wasm_client_event_ptr();
        const steps: PushStep[] = [];
        while ((rc = this.ex.wasm_client_push_next()) === 1) {
            steps.push({ event: V.readPushEvent(m, eventAt), view: V.readTableView(m, viewAt) });
        }
        if (rc !== 0) return this.refused(rc);
        rc = this.ex.wasm_client_push_final();
        if (rc !== V.CLIENT_OK) return this.refused(rc);
        const final = V.readTableView(m, viewAt);
        this.keepIdentity(final.gameId, bytes);
        return { steps, final };
    }

    /** Builds and keeps a table's identity from parts; its bytes, or null when the kernel refuses them. */
    identityFromSeats(gameId: string, title: string, seats: IdentitySeat[]): Uint8Array | null {
        const gid = enc.encode(gameId), text = enc.encode(title);
        if (!this.put(gid, text)) return this.refused(V.CLIENT_E_IDENTITY);
        let rc = this.ex.wasm_client_identity_begin(gid.length, text.length);
        if (rc !== V.CLIENT_OK) return this.refused(rc);
        for (const s of seats) {
            const id = enc.encode(s.id), name = enc.encode(s.name);
            if (!this.put(id, name)) return this.refused(V.CLIENT_E_IDENTITY);
            if ((rc = this.ex.wasm_client_identity_seat(id.length, name.length, s.isAi ? 1 : 0)) !== V.CLIENT_OK) return this.refused(rc);
        }
        const n = this.ex.wasm_client_identity();
        if (n <= 0) return this.refused(n);
        const at = this.ex.wasm_io_ptr();
        const identity = this.m().u8.slice(at, at + n);
        this.identities.set(gameId, identity);
        return identity;
    }

    /** The identity kept for a game, if any. */
    identity(gameId: string): Uint8Array | undefined {
        return this.identities.get(gameId);
    }
}

let shared: ClientTable | null = null;

/** The client's one slot, on the page's bots.wasm (warm it first: ensureBotsAsync). */
export function clientTable(): ClientTable {
    return (shared ??= new ClientTable(__clientKernelExports() as unknown as ClientExports));
}
