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
    type AnimFrameSnap, type AnimPlanEventIn, type AnimPlanSnap,
} from '@sdk/ts/wasm/bots.ts';
import { covered, type TableView, type ViewCard } from './view';

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
