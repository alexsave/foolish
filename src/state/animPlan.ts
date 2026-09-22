// animPlan.ts - the web's animation steps, as the KERNEL's plan.
//
// docs/C_GAME_SHAPE_MIGRATION.md Phase 9. AnimationContext used to decide its
// own timing: one setTimeout per step, ANIMATION_TIME for every duration, and
// nothing anywhere holding "where does this sequence stand at time T". This
// file is the marshal that lets it ask C instead - the steps in, the kernel's
// AnimPlan out, and `frameAt` sampling it at a clock the host owns.
//
// NOTHING HERE DECIDES ANYTHING. Every number a plan carries - the duration, the
// inter-step gap, the start offset, which badges are frozen, which cards are
// veiled, how many of a step's cards left the deck - is the kernel's
// (c/src/anim_plan.h). What is here is the shape change: a ClientAnimationEvent
// names its type and its places with strings and carries a whole TableView; the
// kernel takes ANIM_EVT_*/ANIM_LOC_* and the four numbers off that board.

import {
    ANIM_EVT, ANIM_LOC, animBuildPlan, animPlanAt,
    type AnimCountsSnap, type AnimFrameSnap, type AnimPlanEventIn, type AnimPlanSnap,
} from '@sdk/ts/wasm/bots.ts';
import { covered, sameCard, NO_CARD, type TableView, type ViewBattle, type ViewCard } from './view';

/** anim_plan.h ANIM_TABLE_NONE: a battle with no cover on it. */
const TABLE_NONE = 0xfe;
/** anim_plan.h ANIM_TABLE_UNKNOWN: a card that is there and cannot be named. */
const TABLE_UNKNOWN = 0xff;

/** One step, as the pipeline holds it: the fields the plan reads, and nothing else. */
export interface AnimStep {
    type: string;
    seat?: number;
    cards?: readonly ViewCard[];
    from_location?: string;
    to_location?: string;
    /** The board this step leaves, when it has one of its own. */
    game_state?: TableView | null;
}

const idOf = (c: ViewCard): number =>
    c.suit < 0 || c.value < 1 ? TABLE_UNKNOWN : c.suit * 13 + (c.value - 1);

/** The row a board holds, in the one 2-bytes-per-battle layout every table in
 *  this codebase uses (the attack, then its cover or ANIM_TABLE_NONE). */
export const rowOf = (view: TableView): number[] => {
    const row: number[] = [];
    for (const b of view.battles) {
        row.push(idOf(b.attack));
        row.push(covered(b) ? idOf(b.defense) : TABLE_NONE);
    }
    return row;
};

/** The same row, cell for cell. `null` is "no row of my own", so two nulls are
 *  the same answer and a row is never equal to one. */
export const sameRow = (a: readonly ViewBattle[] | null, b: readonly ViewBattle[] | null): boolean =>
    a === b || (!!a && !!b && a.length === b.length
        && a.every((x, i) => sameCard(x.attack, b[i].attack) && sameCard(x.defense, b[i].defense)));

/** No pile is being held back - the answer for every frame with nothing in the
 *  air, handed out as one object so a consumer can compare it by identity. */
export const NO_HELD: ReadonlySet<number> = new Set<number>();

/** The same set, member for member. */
export const sameHeld = (a: ReadonlySet<number>, b: ReadonlySet<number>): boolean =>
    a === b || (a.size === b.size && [...a].every((x) => b.has(x)));

/**
 * THE PILES THE GRID MUST NOT MAKE ROOM FOR YET: the cards this run is still
 * carrying to the table, minus any the row already held when the stream opened.
 *
 * The grid centres its cells, so a board that ALREADY holds the pile now in
 * flight draws every other pile half a slot plus its gap to the side - and on a
 * COLD OPEN nothing ever moves it back, because there is no previous layout to
 * interpolate away from. c/src/anim_plan.h's AnimCounts is written about that
 * exact defect ("a card 36pt to the side of where it belonged on the very first
 * painted frame, and never moving"), and iMessage holds the row for it
 * (ShownLedger.battles, seeded from `AnimPlan.pre.battles` and advanced one
 * landing at a time). This client advances the row the same way - it commits a
 * step's board at that step's landing - so what is left to answer is only the
 * board that runs AHEAD of its own flight: a confirmation that beat the card it
 * confirms, an optimistic board laid before the run reaches it.
 *
 * TWO ANSWERS DECIDE A CELL, and both are the kernel's: the run's own steps
 * name the cards they are putting on the table, and `AnimCounts.battles` - the
 * plan's pre-stream row, `anim_pre_stream_table` - names the piles that were
 * already down when the stream opened. A pile is held back only when the first
 * says its card is still in the air AND the second does not vouch for it having
 * been there before.
 *
 * THE SECOND TEST IS WHAT KEEPS THIS OFF MY OWN PENDING CARDS. A board on this
 * client is the server's plus whatever I have played and not had confirmed
 * (src/state/optimisticOverlay.ts). Those piles are on the table because
 * nothing is flying them; taking their slot away for the length of somebody
 * else's flight would be the flicker the optimistic overlay exists to prevent.
 */
export function heldPiles(pre: AnimCountsSnap, flying: readonly AnimStep[]): ReadonlySet<number> {
    const air = new Set<number>();
    for (const s of flying) {
        if (s.to_location !== 'table') continue;
        for (const c of s.cards ?? []) air.add(idOf(c));
    }
    if (air.size === 0) return NO_HELD;
    // An unpaired row is the flat one-cell-per-card reading of a pickup and
    // vouches for nothing (AnimPreTable.paired).
    if (pre.paired) for (const id of pre.battles) air.delete(id);
    return air.size === 0 ? NO_HELD : air;
}

/**
 * THE ROW THE GRID DRAWS: the board's, with the cells of `held` taken out (and
 * a cover that is still in the air taken off the attack it has not reached).
 * The board's own array comes back untouched when nothing is held, which is
 * every frame but the ones `heldPiles` is about.
 */
export function shownRow(
    battles: readonly ViewBattle[], held: ReadonlySet<number>,
): readonly ViewBattle[] {
    if (held.size === 0 || battles.length === 0) return battles;
    const row: ViewBattle[] = [];
    for (const b of battles) {
        if (held.has(idOf(b.attack))) continue;               // no slot until it lands
        row.push(covered(b) && held.has(idOf(b.defense)) ? { attack: b.attack, defense: NO_CARD } : b);
    }
    return sameRow(row, battles) ? battles : row;
}

const countsOf = (view: TableView) => ({
    deck: view.deckCount,
    discard: view.discardPileLength,
    flipped: view.hasFlipped ? view.flipped : null,
    hand: view.seats.map((s) => s.handCount),
});

const planEvent = (step: AnimStep): AnimPlanEventIn => {
    const view = step.game_state ?? undefined;
    const cards = step.cards ?? [];
    return {
        type: ANIM_EVT[step.type] ?? ANIM_EVT.magic_transition,
        seat: step.seat,
        from: step.from_location === undefined ? undefined : ANIM_LOC[step.from_location],
        to: step.to_location === undefined ? undefined : ANIM_LOC[step.to_location],
        cards,
        // A viewer-masked back has no identity, so it is excluded from the veil
        // and animates as a back - another player's draw, and nothing else.
        maskCards: cards.length > 0 && cards.every((c) => c.suit < 0),
        counts: view ? countsOf(view) : undefined,
        battles: view ? rowOf(view) : undefined,
    };
};

/**
 * THE TIMED PLAN for a run of steps. `board` is what the page is holding now -
 * the plan's boardless fallback reads it, and nothing else does, because every
 * step that carries a board of its own is described by that board.
 *
 * A run with no board at all (every step optimistic, before any push) still
 * plans: the kernel's timing does not depend on a board, only its freeze does.
 */
export function planFor(steps: readonly AnimStep[], board: TableView | undefined): AnimPlanSnap {
    const nPlayers = board?.seats.length
        ?? steps.reduce((n, s) => Math.max(n, s.game_state?.seats.length ?? 0), 0);
    const final = board
        ? countsOf(board)
        : { deck: 0, discard: 0, flipped: null, hand: new Array<number>(nPlayers).fill(0) };
    return animBuildPlan(steps.map(planEvent), nPlayers, final);
}

/** WHERE THE LAST-BUILT PLAN STANDS at `nowMs` from its start (anim_plan_at). */
export function frameAt(nowMs: number): AnimFrameSnap {
    return animPlanAt(nowMs);
}
