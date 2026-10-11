/* =============================================================================
 * A replay, as the frames live play broadcasts
 * =============================================================================
 * The kernel rebuilds the real Game a v6 code describes, replays it through the
 * real engine, and hands back the SAME packed evwire frames a live game sends -
 * one per step (the deal, then one per action). This module pulls them, reads
 * them with the client's LIVE reader (the kernel's client slot, into TableView
 * boards, src/state/pushSequence.ts naming each step's event), and shapes what a
 * scrubber draws on top.
 *
 * What a frame cannot say, the kernel says (c/src/replay_steps.h ReplayPlayStep):
 * what a step IS - an attack and a pass are one event type on the wire - which
 * steps were one move, where a bout opens and when each step happened.
 *
 * WHAT EACH SEAT HELD, for the reveal-hands eye: a frame is masked for one
 * viewer, so the code is replayed once per seat and each seat's own hand read
 * out of its own frames - exact, by construction, and ~1 ms for a whole game.
 * ========================================================================== */

import { replayEventFrames, replayPlaySteps, type ReplayPlayStep } from '@sdk/ts/wasm/bots.ts';
import { REPLAY_ATOM_DEAL, REPLAY_ATOM_ATTACK, REPLAY_ATOM_COVER, REPLAY_ATOM_PASS, REPLAY_ATOM_PICKUP, REPLAY_ATOM_ROUND_END } from '@sdk/ts/gen/kernel_consts.bots.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import { AnimationSequenceMessage, FeedAnimationEvent } from '../state/animationFeed';
import { undealtBoard } from '../state/clientBoards';
import { pushToSequence, type ViewEvent } from '../state/pushSequence';
import { type TableView, type ViewCard as Card } from '../state/view';

/** The key a replay's boards are held under: the frames' game id, and the one
 *  ReplayServerProvider keeps them in. A board's game id is a table's, and a
 *  share link - its moves and its extras - is longer than a table's id may be,
 *  so the kernel's board writer refuses the link itself. */
export const REPLAY_KEY = 'replay';

/** A step's board plus every seat's exact hand, for the reveal-hands overlay. */
export type ReplayGameState = TableView & {
    readonly replay_hands: (Card | null)[][];
};

export interface ReplayFrame {
    /** REPLAY_ATOM_* - what the kernel says this step played. */
    kind: number;
    /** The acting seat, or null (the deal; a round end nobody in particular closed). */
    seat: number | null;
    /** The cards this step moved, for the status line. */
    cards: Card[];
    /** How many of `cards` the MOVE NAMED, as the kernel counts them: the cards
     *  the seat CHOSE. 0 for a pickup, whose step carries the pile it swept.
     *  A reader that treats this step as a decision slices `cards` to this. */
    named: number;
    /** A COVER's (card, attack) pairs - one normally, several where the wire
     *  split one multi-cover into consecutive steps and this frame merged them
     *  back. Absent for every other kind. */
    pairs?: { card: Card; target: Card }[];
    /** When it happened, unix seconds, or null for a replay cut without times. */
    time: number | null;
    /** An attack onto an empty table: where a bout begins. */
    opensBout: boolean;
    /** This frame's index in the KERNEL's step stream, which a merged cover run
     *  makes different from its index here. Every call that addresses the wire
     *  by step - replayStepMaskedState, replayStepLogs - takes this, not the
     *  array position. */
    step: number;
    /** The attack card being covered (COVER only). */
    target: Card | null;
    /** Cards moved but not shown individually (the discard count, hidden draws). */
    count: number;
    /** Ready to publish into the animation feed - the events + the committed board. */
    seq: AnimationSequenceMessage;
    /** This step's committed board, with every seat's hand attached. */
    game: ReplayGameState;
}

/* The event a step's status line describes. A step's frame carries the action
 * AND everything it caused (a cover that ends a bout brings the discard and the
 * refills with it, exactly as live play does), so the kernel's kind picks which
 * of those events is the one being narrated. */
const NARRATED_EVENT: Record<number, string> = {
    [REPLAY_ATOM_DEAL]: 'flipped',        // the deal's news is the trump
    [REPLAY_ATOM_ATTACK]: 'attack_pass',
    [REPLAY_ATOM_PASS]: 'attack_pass',
    [REPLAY_ATOM_COVER]: 'cover',
    [REPLAY_ATOM_PICKUP]: 'pickup',
    [REPLAY_ATOM_ROUND_END]: 'cards_to_trash',
    // GOOD moves no cards - nothing to narrate but the seat.
};

/* A replay's frames name no one on the wire, and a replay has no one signed in:
 * a code carries no player ids at all. So a frame is read with no identity, and
 * its seats keep the kernel's empty ids - the page names a seat by its index
 * (state/view.ts seatKey) - and take the names the code's extras give them (or
 * P1, P2...). Both are the host's display strings, set on the snapshot after
 * the read; no field of the game is. */
interface Naming { names?: (string | null)[] | null; gameId: string }

const named = (v: TableView, naming: Naming): TableView => ({
    ...v,
    gameId: naming.gameId,
    seats: v.seats.map((s, i) => ({ ...s, name: naming.names?.[i] || `P${i + 1}` })),
});

const readFrame = (bytes: Uint8Array, naming: Naming | null) => {
    const read = clientTable().readPush(bytes, { as3: false, identity: 'none' });
    if (!read) return null;
    if (!naming) return pushToSequence(read);
    return pushToSequence({
        steps: read.steps.map((s) => ({ event: s.event, view: named(s.view, naming) })),
        final: named(read.final, naming),
    });
};

interface ReplayFramesOpts {
    /** Seat whose eyes the replay is watched through; -1 (default) = spectator.
     *  The shared-replay screen is a spectator; the tutorial sits in seat 0, so
     *  its boards mask exactly as they would for a real player in that seat. */
    viewer?: number;
    /** The loser's seat, marked in the closing board's name - the one thing the
     *  board itself does not carry. */
    fool?: number | null;
    /** The extras' clock: the first move's unix time and the per-move gaps, in
     *  seconds. The gaps are what tells one multi-cover apart from several single
     *  ones; absent on a replay cut without times, and then no cover run is merged. */
    startTime?: number | null;
    moveGaps?: number[] | null;
}

/** Every step of a v6 code, as the frames live play broadcasts. */
export function buildReplayFrames(
    code: Uint8Array,
    gameId: string,
    names?: (string | null)[] | null,
    opts: ReplayFramesOpts = {},
): ReplayFrame[] {
    const viewer = opts.viewer ?? -1;
    const fool = opts.fool ?? null;

    const moveGaps = opts.moveGaps ?? null, startTime = opts.startTime ?? null;
    const index = replayPlaySteps(code, startTime, moveGaps);
    const main = replayEventFrames(code, viewer);
    if (main.length !== index.length) {
        throw new Error(`replay: ${main.length} frames for ${index.length} steps`);
    }
    const seats: Naming = { names, gameId };
    const seqs = main.map((bytes, i) => {
        const seq = readFrame(bytes, seats);
        if (!seq) throw new Error(`replay: step ${i} did not decode`);
        return seq;
    });

    // One replay per seat, read for that seat's own hand: the reveal eye's
    // whole source of truth.
    const perSeat = seqs[0].game.seats.map((_, s) => replayEventFrames(code, s));

    const built: ReplayFrame[] = seqs.map((seq, i): ReplayFrame => {
        const hands: (Card | null)[][] = perSeat.map((frames, s) => {
            const own = readFrame(frames[i], null);
            // A seat's own frame always reveals its own hand; fall back to backs
            // rather than crash if a future masking change ever breaks that.
            return own && own.game.mySeat === s ? own.game.myHand.map((c) => ({ ...c }) as Card | null)
                        : Array.from({ length: seq.game.seats[s]?.handCount ?? 0 }, () => null);
        });

        const info = index[i];
        const atEnd = i === main.length - 1;
        // A spectator holds nothing and acts on nothing (mySeat -1); a seated
        // viewer's hand is the kernel's own, masked exactly as it would be live.
        const game: ReplayGameState = {
            ...seq.game,
            seats: seq.game.seats.map((p, s) => ({
                ...p,
                name: `${p.name}${atEnd && fool === s ? ' 🃏' : ''}`,
            })),
            replay_hands: hands,
        };

        const narrated = NARRATED_EVENT[info.kind];
        const ev = narrated ? seq.events.find((e) => e.type === narrated) : undefined;

        return {
            kind: info.kind,
            seat: info.seat < 0 ? null : info.seat,
            cards: ev?.cards?.map((c) => ({ ...c })) ?? [],
            named: info.named,
            time: startTime === null || !moveGaps ? null : info.at,
            opensBout: info.opensBout,
            step: i,
            ...(info.kind === REPLAY_ATOM_COVER && ev?.cards?.[0] && ev?.target_card
                ? { pairs: [{ card: { ...ev.cards[0] }, target: { ...ev.target_card } }] }
                : null),
            target: ev?.target_card ? { ...ev.target_card } : null,
            count: ev?.cards?.length ?? 0,
            seq: {
                type: 'animation_sequence',
                sequence_id: '',
                timestamp: 0,
                events: seq.events.map((e) => ({ ...e }) as unknown as FeedAnimationEvent),
                game,
            },
            game,
        };
    });
    return mergeCoverRuns(built, index);
}

// ONE MOVE IS ONE STEP. The replay coder codes a cover one pair at a time, so
// a defender who takes three attacks in one move comes back as three steps; the
// kernel says which steps were one move (ReplayPlayStep.joins) and this folds
// them, so the board plays a double cover as one move, the scrubber counts it
// once, and the Oracle deliberates it once.
function mergeCoverRuns(frames: ReplayFrame[], index: readonly ReplayPlayStep[]): ReplayFrame[] {
    const out: ReplayFrame[] = [];
    for (let i = 0; i < frames.length; i++) {
        const f = frames[i];
        let j = i;
        while (j + 1 < frames.length && index[j + 1].joins) j++;
        if (j === i) { out.push(f); continue; }
        const run = frames.slice(i, j + 1);
        const last = run[run.length - 1];
        // The board is the one the LAST pair left; the events are every pair's,
        // in play order, so both cards fly on the one step.
        out.push({
            ...last,
            // The board is the last pair's, but the STEP is the first pair's:
            // that is the one the whole move was decided on, and it is what the
            // kernel's step-addressed calls have to be given.
            step: run[0].step,
            // `target` pairs with cards[0], so it is the FIRST pair's - spreading
            // `last` would leave a reader comparing one pair's card against
            // another pair's attack. Anything that wants the whole move reads
            // `pairs`; this keeps the single-pair shape honest for what does not.
            target: run[0].target ? { ...run[0].target } : null,
            cards: run.flatMap((r) => r.cards.map((c) => ({ ...c }))),
            named: run.reduce((n, r) => n + r.named, 0),
            count: run.reduce((n, r) => n + r.count, 0),
            pairs: run.flatMap((r) => r.pairs ?? []),
            seq: { ...last.seq, events: run.flatMap((r) => r.seq.events) },
        });
        i = j;
    }
    return out;
}

/**
 * The board the opening deal animates onto: the whole stock face down, no trump
 * turned, an empty table and no card in any hand - the watching seat's own
 * included, though its first frame already shows the hand it was dealt. The
 * kernel makes it from the first frame's board (client_board_edit UNDEAL).
 */
export function preDealGame(first: ReplayFrame): ReplayGameState {
    const board = undealtBoard(first.game);
    if (!board) throw new Error('replay: the board before the deal was refused');
    return { ...board, replay_hands: first.game.seats.map(() => []) };
}

/* =============================================================================
 * Stepping backwards
 * =============================================================================
 * The forward stream is the engine's own; there is no backward one, because the
 * engine cannot un-play a move. So a step back INVERTS the flight - cards that
 * flew hand->table on the way in fly table->hand on the way out - and lands on
 * the previous step's board, which the previous frame already carries. The
 * inversion is presentation only: nothing about the game is being recomputed,
 * and the state committed at the end is the kernel's, not a rewind of it.
 *
 * Motions with no honest inverse (drawing from a face-down stock, the opening
 * deal, a rotation) collapse to a magic_transition: the prior board is committed
 * without a flight. Seeking skips this entirely and commits directly.
 * ========================================================================== */
export function buildReverseFrames(frames: ReplayFrame[]): (AnimationSequenceMessage | null)[] {
    // Nothing precedes the deal: stepping back from step 0 clamps.
    const reverse: (AnimationSequenceMessage | null)[] = [null];

    for (let i = 1; i < frames.length; i++) {
        const prev = frames[i - 1].game;
        // The action is the frame's first event; the ones after it are what the
        // action caused, and they land back on `prev` for free by being dropped.
        const fe = frames[i].seq.events[0] as ViewEvent & FeedAnimationEvent;
        let event: FeedAnimationEvent;

        switch (fe.type) {
            // hand->table on the way in => table->hand on the way out
            case 'attack_pass':
            case 'cover':
                event = {
                    type: 'pickup',
                    seat: fe.seat,
                    // A merged cover run played several cards on this one step,
                    // and every one of them has to come back.
                    cards: fe.type === 'cover' ? frames[i].cards : fe.cards,
                    from_location: 'table',
                    to_location: 'hand',
                    game_state: prev,
                };
                break;

            // table->hand on the way in => hand->table on the way out
            case 'pickup':
                event = {
                    type: 'attack_pass',
                    seat: fe.seat,
                    cards: fe.cards,
                    from_location: 'hand',
                    to_location: 'table',
                    game_state: prev,
                };
                break;

            // table->discard on the way in => discard->table on the way out
            case 'cards_to_trash':
                event = {
                    type: 'pickup',
                    cards: fe.cards,
                    from_location: 'discard',
                    to_location: 'table',
                    game_state: prev,
                };
                break;

            default:
                event = {
                    type: 'magic_transition',
                    seat: fe.seat,
                    game_state: prev,
                };
                break;
        }

        reverse.push({
            type: 'animation_sequence',
            sequence_id: '',
            timestamp: 0,
            events: [event],
            game: prev,
        });
    }

    return reverse;
}
