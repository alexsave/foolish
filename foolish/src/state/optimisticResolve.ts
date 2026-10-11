// optimisticResolve.ts - MY MOVES THE SERVER HAS NOT ANSWERED, and what an
// arriving push does to them.
//
// A screen shows a card on the table the moment its owner taps it, long before
// any server says so. What an arriving push does to such a card - it is the
// card's own confirmation, it sweeps the card off, it leaves the card waiting,
// it dooms it - is ONE question with one kernel answer (c/src/client_table.h
// client_push_settle), and so are the board a doomed card flies home over
// (client_revert_board) and the boards a waiting card stays on
// (client_keep_board). This file holds the list the question is asked about and
// does what the answer says.
//
// Nothing here is React. Asserted natively (c/tests/tests.c
// test_client_push_settle) and end-to-end through the real provider, frame by
// frame, by e2e/ui_animation_trace.test.ts.

import * as V from '@sdk/ts/gen/view_layout.bots.ts';
import { ANIM_LOC_NONE } from '@sdk/ts/gen/anim.bots.ts';
import { ANIM_LOC, animEventTypeCode } from '@sdk/ts/wasm/bots.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import { optimisticBoard } from './clientBoards';
import { NO_CARD, sameCard, type TableView, type ViewCard as Card } from './view';
import type { ClientAnimationEvent } from './animationStep';

// The board's own seat, if the board has one: the seat an event of mine is keyed by.
export const seatOf = (v: TableView | null | undefined): number | undefined =>
    v && v.mySeat >= 0 ? v.mySeat : undefined;

/** A MOVE OF MINE THE SERVER HAS NOT CONFIRMED, one card of it, as the kernel
 *  reads it (client_table.h ConflictMotion), and when it was predicted. */
type PendingMotion = { -readonly [K in keyof V.ConflictMotion_Snap]: V.ConflictMotion_Snap[K] } & { at: number };

/** Everything the client knows about its own unanswered moves. */
export interface OptimisticState {
    motions: PendingMotion[];
    /** My unconfirmed pass, as the action wire I sent: it stands while a motion is still marked as its card. */
    pass: Uint8Array | null;
}

/** Remember a predicted move's cards as pending. `targets` are the attacks a cover's cards cover, by position. */
export const rememberPending = (
    s: OptimisticState, cards: readonly Card[], dest: number, at: number, targets?: readonly Card[], pass?: Uint8Array,
): void => {
    if (pass) {
        for (const m of s.motions) m.isPass = false;
        s.pass = pass;
    }
    for (const [i, card] of cards.entries()) {
        s.motions.push({ card, dest, isCover: !!targets, isPass: !!pass, target: targets?.[i] ?? NO_CARD, at });
    }
};

/** Drop predictions nobody ever answered: older than `maxAge` at `now`. */
export const sweepStaleMotions = (s: OptimisticState, now: number, maxAge: number): void => {
    s.motions = s.motions.filter((m) => now - m.at <= maxAge);
};

/** The server refused a move of mine: its cards that were still pending, which are pending no longer. */
export const forgetRefused = (s: OptimisticState, cards: readonly Card[], dest: number, isCover: boolean): Card[] => {
    const mine = (m: PendingMotion) => m.dest === dest && m.isCover === isCover && cards.some((c) => sameCard(c, m.card));
    const refused = s.motions.filter(mine).map((m) => m.card);
    s.motions = s.motions.filter((m) => !mine(m));
    return refused;
};

/**
 * My pending table cards, for the REST load path: a reconnect resync re-applies
 * them instead of momentarily wiping them (the "vanish then reappear" glitch).
 */
export const pendingOverlayCards = (s: OptimisticState): { card: Card; target?: Card | null }[] =>
    s.motions.filter((m) => m.dest === V.ANIM_DEST_TABLE).map((m) => ({ card: m.card, target: m.isCover ? m.target : null }));

/** My pass's wire while it is pending: while a motion is still one of its cards. */
const pendingPass = (s: OptimisticState): Uint8Array | null => {
    if (s.pass && !s.motions.some((m) => m.isPass)) s.pass = null;
    return s.pass;
};

/** `board` with my still-unconfirmed pass standing on it, asked of the kernel;
 *  null when no pass is pending or the kernel refuses to change the board. */
export const passedBoard = (s: OptimisticState, board: TableView | null | undefined): TableView | null => {
    const pass = board ? pendingPass(s) : null;
    return pass ? optimisticBoard(board!, pass) : null;
};

/** A push as this file reads it. `events` is replaced and the boards are
 *  replaced in place when pending cards are kept on them. */
interface IncomingPush {
    events: any[];
    game: TableView;
}

const locationCode = (loc: string | undefined): number => (loc && loc in ANIM_LOC ? ANIM_LOC[loc] : ANIM_LOC_NONE);

/**
 * Judge an arriving push against my unanswered moves, and do what the kernel says.
 *
 * `push.events` loses the events this client already played as predictions;
 * every pending motion the push answers loses its tracking; the motions it keeps
 * are laid on every board it carries; and the flights that carry the doomed ones
 * home come back, in the order they fly, each with the board it is drawn over.
 * `versioned`: a live cast, whose final board is authoritative.
 */
export function settlePush(s: OptimisticState, push: IncomingPush, versioned: boolean): ClientAnimationEvent[] {
    if (s.motions.length === 0) return [];
    const table = clientTable();
    const all = push.events, mine = s.motions;
    const events = all.map((e) => ({
        type: animEventTypeCode(e.type), masked: false, seat: e.seat ?? -1,
        from: locationCode(e.from_location), to: locationCode(e.to_location), cards: e.cards ?? [],
    }));
    const ask = (openEvent: number) => table.settle(
        openEvent === V.CLIENT_EVENT_NONE ? null : all[openEvent].game_state, push.game,
        { versioned, openEvent, events, motions: mine }, pendingPass(s));
    let a = ask(V.CLIENT_EVENT_NONE);
    if (a.openEvent !== V.CLIENT_EVENT_NONE) a = ask(a.openEvent);

    s.motions = mine.filter((_, i) => a.fates[i] === V.CLIENT_FATE_PENDING || a.fates[i] === V.CLIENT_FATE_KEEP);
    push.events = all.filter((_, i) => !a.played[i]);
    if (a.fates.includes(V.CLIENT_FATE_KEEP)) {
        const keep = (board: TableView): TableView => table.keptBoard(board, pendingPass(s)) ?? board;
        for (const e of push.events) if (e.game_state) e.game_state = keep(e.game_state);
        push.game = keep(push.game);
    }
    if (a.flightCount.length === 0) return [];

    const home = table.revertBoard(
        all[a.homeEvent].game_state, a.magicEvent === V.CLIENT_EVENT_NONE ? null : all[a.magicEvent].game_state) ?? undefined;
    let at = 0;
    return a.flightCount.map((n): ClientAnimationEvent => {
        const flying = a.order.slice(at, at += n).map((i) => mine[i]);
        const toHand = flying[0].dest === V.ANIM_DEST_TABLE;
        return {
            type: 'revert', cards: flying.map((m) => m.card), is_revert: true, seat: seatOf(push.game), game_state: home,
            from_location: toHand ? 'table' : 'hand', to_location: toHand ? 'hand' : 'table',
        };
    });
}
