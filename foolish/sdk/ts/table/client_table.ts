// client_table.ts - the web client's slot (c/src/client_table.h), over the
// kernel link the runtime loads (web.wasm in a browser, bots.wasm on Node).
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

import type { WebExports } from '../gen/exports.web.ts';
import * as V from '../gen/view_layout.bots.ts';
import { __clientKernelExports } from '../wasm/bots.ts';

export type TableView = V.TableView_Snap;
export type PushEvent = V.PushEvent_Snap;
export type ViewSeat = V.ViewSeat_Snap;
export type ViewCard = V.Card_Snap;
export type ViewBattle = V.Battle_Snap;
export type ViewRules = V.ViewRules_Snap;
export type BoardEdit = V.BoardEdit_Snap;
export type ConflictQuestion = V.ClientConflict_Snap;
export type ClientPlay = V.ClientPlay_Snap;
export type ActionResponse = V.TableResponse_Snap;
export type HandMetrics = V.HandMetrics_Snap;
export type HandLayout = V.HandLayout_Snap;

/** One step of a push: what moved, and the board it left. */
export interface PushStep { event: PushEvent; view: TableView }

/** A whole push, read. */
export interface PushRead { steps: PushStep[]; final: TableView }

/** A seat of an identity built from parts. */
export interface IdentitySeat { id: string; name: string; isAi: boolean }

/** A refusal as the kernel read it (ask.h AskRefusal): `why` an ASK_WHY_*, `code` and `detail` the kernel's own. */
export type AskRefusal = V.AskRefusal_Snap;

/** An ask (ask.h TableAsk): its kind, the game it names, and whichever fields that kind carries. */
export type Ask = Pick<V.TableAsk_Snap, 'kind' | 'gid'> & Partial<V.TableAsk_Snap>;
const NO_ASK: V.TableAsk_Snap = { kind: 0, passing: 0, gid: '', bot: '', player: '', idx: [], order: [], title: '' };

/** A move's kinds by the names a caller says them with, numbered by the kernel (awire.h AWIRE_*). */
export const AWIRE_KIND = {
    attack: V.AWIRE_ATTACK,
    cover: V.AWIRE_COVER,
    pass: V.AWIRE_PASS,
    pickup: V.AWIRE_PICKUP,
    good: V.AWIRE_GOOD,
} as const;
export type AwireKindName = keyof typeof AWIRE_KIND;

export interface AwireMove {
    kind: AwireKindName;
    cards?: ViewCard[];
    attack_cards?: ViewCard[]; // cover only, positional with cards
}

/** How a push or cast names its seats: the identity kept for `gameId` ('kept', the default), explicit bytes, or 'none' (as a replay's are). */
export interface PushIdentity { gameId?: string; identity?: 'kept' | 'none' | Uint8Array }

const enc = new TextEncoder();

export class ClientTable {
    private readonly ex: WebExports;
    private mem: V.Mem;
    private readonly identities = new Map<string, Uint8Array>();
    private refusal = 0;

    constructor(ex: WebExports) {
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

    /** The `n` bytes a door left at the IO buffer, copied out. */
    private out(n: number): Uint8Array {
        const at = this.ex.wasm_io_ptr();
        return this.m().u8.slice(at, at + n);
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

    /** A realtime push (evwire as2, or as3 with its flags byte), or null when it does not read whole. A push that carries a roster names its seats itself. */
    readPush(bytes: Uint8Array, opts: PushIdentity & { as3: boolean; version?: number }): PushRead | null {
        return this.read(bytes, opts, (n) => this.ex.wasm_client_push_open(bytes.length, n, opts.as3 ? 1 : 0, opts.version ?? 0));
    }

    /**
     * A realtime message's payload whole (evwire.h, the cast: the committed version
     * and the as3 push in one run of bytes), or null when it does not read whole.
     * The boards carry the version the cast names.
     */
    readCast(bytes: Uint8Array, opts: PushIdentity = {}): PushRead | null {
        return this.read(bytes, opts, (n) => this.ex.wasm_client_cast_open(bytes.length, n));
    }

    /** The version a cast's header names, or null for bytes that have no such header: what to fetch at least, when the cast itself did not read. */
    castVersion(bytes: Uint8Array): number | null {
        if (!this.put(bytes)) return null;
        const v = this.ex.wasm_client_cast_version(bytes.length);
        return v < 0 ? null : v;
    }

    /** Every step and the committed board of the push or cast `open` opens from `bytes` and the identity's length. */
    private read(bytes: Uint8Array, opts: PushIdentity, open: (identityLen: number) => number): PushRead | null {
        const identity = opts.identity instanceof Uint8Array ? opts.identity
            : opts.identity === 'none' || !opts.gameId ? new Uint8Array(0)
            : this.identities.get(opts.gameId) ?? new Uint8Array(0);
        if (!this.put(bytes, identity)) return this.refused(V.CLIENT_E_PUSH);
        let rc = open(identity.length);
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

    // ---- an ask (ask.h): every request that is not a move ----

    /**
     * The request body for an ask, written by the kernel (ask_encode), or null for
     * an ask that is not one (no game id, a count past its array). A string too
     * long for its slot is the generated writer's RangeError.
     */
    askBody(ask: Ask): Uint8Array | null {
        V.writeTableAsk(this.m(), this.ex.wasm_client_ask_ptr(), { ...NO_ASK, ...ask });
        const n = this.ex.wasm_client_ask_encode();
        return n < 0 ? this.refused(n) : this.out(n);
    }

    /** A refusal body (ask.h AskRefusal), or null when the bytes are not one. */
    readRefusal(bytes: Uint8Array): AskRefusal | null {
        const rc = this.put(bytes) ? this.ex.wasm_client_refusal_decode(bytes.length) : V.ASK_E_WIRE;
        return rc !== V.ASK_OK ? this.refused(rc) : V.readAskRefusal(this.m(), this.ex.wasm_client_refusal_ptr());
    }

    /** Builds and keeps a table's identity from parts; its bytes, or null when the kernel refuses them. */
    identityFromSeats(gameId: string, title: string, seats: IdentitySeat[]): Uint8Array | null {
        const gid = enc.encode(gameId), text = enc.encode(title);
        let rc = this.put(gid, text) ? this.ex.wasm_client_identity_begin(gid.length, text.length) : V.CLIENT_E_IDENTITY;
        for (const s of seats) {
            if (rc !== V.CLIENT_OK) break;
            const id = enc.encode(s.id), name = enc.encode(s.name);
            rc = this.put(id, name) ? this.ex.wasm_client_identity_seat(id.length, name.length, s.isAi ? 1 : 0) : V.CLIENT_E_IDENTITY;
        }
        const n = rc === V.CLIENT_OK ? this.ex.wasm_client_identity() : rc;
        if (n <= 0) return this.refused(n);
        const identity = this.out(n);
        this.identities.set(gameId, identity);
        return identity;
    }

    /**
     * What `view` shows that is a rule of the game (c/src/client_table.h ViewRules):
     * the sword and shield seats, the viewer's Good, and the stock while
     * `fromDeck` cards fly out of it, `toFlipped` of them to the trump's slot,
     * and `trumpOut` when the view's trump is in the air on its way to a hand
     * (the plan frame's trump_flight). Any board a screen holds, the slot's or
     * one the host changed. A view is a value, so the answer is kept per view
     * object and flight.
     */
    rules(view: TableView, fromDeck = 0, toFlipped = 0, trumpOut = false): ViewRules {
        const key = (fromDeck * 65536 + toFlipped) * 2 + (trumpOut ? 1 : 0);
        let byFlight = this.ruled.get(view);
        const hit = byFlight?.get(key);
        if (hit) return hit;
        this.writeRulesView(view);
        const rc = this.ex.wasm_client_view_rules(fromDeck, toFlipped, trumpOut ? 1 : 0);
        if (rc !== V.CLIENT_OK) throw new Error(`client view rules: the view was refused (${rc})`);
        const r = V.readViewRules(this.m(), this.ex.wasm_client_rules_ptr());
        if (!byFlight) this.ruled.set(view, byFlight = new Map());
        byFlight.set(key, r);
        return r;
    }
    private readonly ruled = new WeakMap<TableView, Map<number, ViewRules>>();

    // The board the rules view holds: the rules and the gates only read it, so a
    // render that asks several of them of one board writes it once.
    private rulesBoard: TableView | null = null;
    private writeRulesView(view: TableView): void {
        if (this.rulesBoard === view) return;
        this.rulesBoard = null;
        V.writeTableView(this.m(), this.ex.wasm_client_rules_view_ptr(), view);
        this.rulesBoard = view;
    }

    // ---- the boards a client makes (c/src/client_table.h, Phase 6b) ----

    /**
     * The viewer's move, as its action wire, judged on `view` by the engine:
     * 0 legal, the ENGINE_REJECT_* that refuses it, CLIENT_E_MOVE for a wire that
     * is not a move, or the GAME_INVALID_* the board is refused for.
     */
    validate(view: TableView, wire: Uint8Array): number {
        this.writeRulesView(view);
        if (!this.put(wire)) return V.CLIENT_E_MOVE;
        return this.ex.wasm_client_validate(wire.length);
    }

    /** The board at the edit view, or null (and the refusal kept) when the door that edited it answered `rc`. */
    private edited(rc: number): TableView | null {
        return rc !== V.CLIENT_OK ? this.refused(rc) : V.readTableView(this.m(), this.ex.wasm_client_edit_view_ptr());
    }

    /** The board `view` becomes once the viewer's move (its action wire) stands on it, or null when the kernel refuses. */
    optimisticApply(view: TableView, wire: Uint8Array): TableView | null {
        V.writeTableView(this.m(), this.ex.wasm_client_edit_view_ptr(), view);
        return this.edited(this.put(wire) ? this.ex.wasm_client_optimistic_apply(wire.length) : V.CLIENT_E_MOVE);
    }

    /** The board after each edit in turn (CLIENT_EDIT_*), or null when the kernel refuses one. */
    edit(view: TableView, ...edits: BoardEdit[]): TableView | null {
        V.writeTableView(this.m(), this.ex.wasm_client_edit_view_ptr(), view);
        let rc: number = V.CLIENT_OK;
        for (const e of edits) {
            V.writeBoardEdit(this.m(), this.ex.wasm_client_board_edit_ptr(), e);
            if ((rc = this.ex.wasm_client_board_edit()) !== V.CLIENT_OK) break;
        }
        return this.edited(rc);
    }

    /**
     * The viewer's hand in the order `indices` gives: the board, or the refusal -
     * CLIENT_E_MISMATCH when there is no hand to order, CLIENT_E_MOVE when the
     * indices are not an order of it.
     */
    rearrangeHand(view: TableView, indices: readonly number[]): { rc: number; view: TableView | null } {
        // An index a byte cannot hold is no index of a hand: 255 is past every hand.
        const bytes = Uint8Array.from(indices, (i) => (Number.isInteger(i) && i >= 0 && i < 255 ? i : 255));
        V.writeTableView(this.m(), this.ex.wasm_client_edit_view_ptr(), view);
        const rc = this.put(bytes) ? this.ex.wasm_client_rearrange_hand(bytes.length) : V.CLIENT_E_MOVE;
        return { rc, view: this.edited(rc) };
    }

    /**
     * The conflict verdict (ANIM_CONFLICT_*) for each of the question's motions
     * against a push whose last board is `open` and whose final board is `final`.
     * Throws when the kernel refuses the question: a verdict nobody could read is
     * not a verdict to act on.
     */
    conflictVerdicts(open: TableView, final: TableView, q: ConflictQuestion): readonly number[] {
        const m = this.m();
        V.writeTableView(m, this.ex.wasm_client_edit_view_ptr(), open);
        V.writeTableView(m, this.ex.wasm_client_final_view_ptr(), final);
        V.writeClientConflict(m, this.ex.wasm_client_conflict_ptr(), q);
        const n = this.ex.wasm_client_conflict_verdicts();
        if (n < 0) throw new Error(`client conflict verdicts: refused (${n})`);
        return V.readConflictVerdicts(this.m(), this.ex.wasm_client_verdicts_ptr()).verdicts;
    }

    /**
     * The action wire of a move (awire.c's one encoder): the bytes `validate` and
     * `optimisticApply` read and the bytes the server applies. Throws on a move
     * the wire cannot carry - cards on a pickup, a cover whose pairs do not
     * match, a card that is not a card: a client bug, never a race.
     */
    moveWire(move: AwireMove): Uint8Array {
        const kind = AWIRE_KIND[move.kind];
        if (kind === undefined) throw new Error(`awire: unknown kind ${move.kind}`);
        V.writeClientMove(this.m(), this.ex.wasm_client_move_ptr(), { kind, cards: move.cards ?? [], attackCards: move.attack_cards ?? [] });
        const n = this.ex.wasm_client_move_wire();
        if (n <= 0) throw new Error(`client move: not a move the wire carries (${n})`);
        return this.out(n);
    }

    /**
     * The `action` endpoint's request body for a move's wire (table_request_encode).
     * `intent` is the games.version the move was composed against: the server
     * refuses a move whose intent predates the current round
     * (TABLE_REJECT_STALE_ROUND, docs/WEB_RACE_BUG_HANDOFF.md); undefined sends the
     * v1 body, which is not guarded. Throws when the kernel refuses: a game id
     * longer than its length byte can say.
     */
    actionRequest(gameId: string, wire: Uint8Array, intent?: number): Uint8Array {
        const gid = enc.encode(gameId);
        const n = this.put(gid, wire)
            ? this.ex.wasm_client_action_request(gid.length, wire.length, intent === undefined ? -1 : intent >>> 0)
            : -1;
        if (n <= 0) throw new Error(`action request: refused (${n}) for a ${gid.length}-byte game id and a ${wire.length}-byte wire`);
        return this.out(n);
    }

    /** The `action` endpoint's response body (table_response_decode: a TABLE_STATUS_*, the reject code, the version), or null when it is not one. */
    readActionResponse(body: Uint8Array): ActionResponse | null {
        if (!this.put(body) || this.ex.wasm_client_action_response(body.length) !== 0) return null;
        return V.readTableResponse(this.m(), this.ex.wasm_client_response_ptr());
    }

    /** Whether `defense` beats `attack` under `powerSuit` (the kernel's can_cover). */
    canCover(attack: ViewCard, defense: ViewCard, powerSuit: number): boolean {
        return this.ex.wasm_can_cover(attack.suit, attack.value, defense.suit, defense.value, powerSuit) === 1;
    }

    /**
     * What a gesture means on `view` (c/src/client_table.h client_play, over
     * legal.h's play_* rules): the move `cards` let go at `target` resolves to,
     * which battles they could cover, the battle the Cover button aims at, and
     * whether this seat may say Good yet, and `pills`: the PLAY_PILL_* bits of
     * the action pills to draw for `cards` as the selection (legal.h
     * play_pills), given the host's own PLAY_HOST_* bits in `host` - only its
     * transport and screen; every fact about the board is the kernel's.
     *
     * `target` is a battle index, PLAY_TARGET_TABLE for the open table,
     * PLAY_TARGET_HAND for a drop back in the hand (always a rearrange, never a
     * play), or CLIENT_PLAY_COVER_BUTTON for the button, which aims itself.
     * `moveType` is a MOVE_*, or -1 when the gesture names no legal move - an
     * answer, not a refusal. Throws when the kernel refuses the board.
     *
     * The answer is kept per board, selection and target: a drag asks this on
     * every frame while the finger sits still, and the menu behind it is a full
     * cover enumeration. The same memo `rules` above keeps, for the same reason.
     */
    play(view: TableView, cards: readonly ViewCard[], target: number, host = 0): ClientPlay {
        const key = `${target}|${host}|${cards.map((c) => `${c.suit},${c.value}`).join(' ')}`;
        let byGesture = this.plays.get(view);
        const hit = byGesture?.get(key);
        if (hit) return hit;
        this.writeRulesView(view);
        V.writeClientGesture(this.m(), this.ex.wasm_client_gesture_ptr(), { target, cards: [...cards], host });
        const rc = this.ex.wasm_client_play();
        if (rc !== V.CLIENT_OK) throw new Error(`client play: the gesture was refused (${rc})`);
        const p = V.readClientPlay(this.m(), this.ex.wasm_client_play_ptr());
        if (!byGesture) this.plays.set(view, byGesture = new Map());
        byGesture.set(key, p);
        return p;
    }
    private readonly plays = new WeakMap<TableView, Map<string, ClientPlay>>();

    /**
     * How `count` cards sit in `width` (c/src/hand_layout.h hand_layout): the
     * rows, one card width, the hand's height and every slot, top row first, in
     * the host's own units. Throws when the kernel refuses the count.
     */
    handLayout(metrics: HandMetrics, count: number, width: number): HandLayout {
        V.writeHandMetrics(this.m(), this.ex.wasm_hand_metrics_ptr(), metrics);
        const rc = this.ex.wasm_hand_layout(count, width);
        if (rc !== V.HAND_EOK) throw new Error(`hand layout: ${count} cards were refused (${rc})`);
        return V.readHandLayout(this.m(), this.ex.wasm_hand_layout_ptr());
    }
}

let shared: ClientTable | null = null;

/** The client's one slot, on the page's kernel (warm it first: ensureBotsAsync). */
export function clientTable(): ClientTable {
    return (shared ??= new ClientTable(__clientKernelExports() as unknown as WebExports));
}

/** A move's action wire, written by the client's one slot. */
export const encodeAction = (move: AwireMove): Uint8Array => clientTable().moveWire(move);
