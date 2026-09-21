/* =============================================================================
 * A replay, as the frames live play broadcasts (C_CORE_CONSOLIDATION.md A5)
 * =============================================================================
 * The kernel rebuilds the real Game a v6 code describes, replays it through the
 * real engine, and hands back the SAME packed evwire frames a live game sends —
 * one per step (the deal, then one per action). This module pulls them, reads
 * them with the client's LIVE reader (the kernel's client slot, into TableView
 * boards named by the replay's own seats, src/state/pushSequence.ts naming each
 * step's event), and shapes the handful of things a scrubber needs on top: what
 * each step is, and what each seat held.
 *
 * What used to be here instead: a TS fold over the decoded log stream that
 * rebuilt every board itself and RETRODICTED the hidden cards — assigning each
 * revealed card back to the oldest face-down slot that could have held it. It
 * was a second implementation of the rules, kept in step with the engine by
 * hand, and it was a guess. None of that survives: v6 is hidden-state-lossless,
 * so the kernel does not guess, and the boards are the ones the engine really
 * played.
 *
 * The two facts a frame cannot tell you, and where they come from instead:
 *
 *   - WHAT a step is. On the wire an attack and a pass are one event type,
 *     separated only by a reconstructed English sentence. Pattern-matching that
 *     prose would be a projection by the back door, so the kernel reports it
 *     (replayStepIndex -> c/src/replay_steps.c).
 *
 *   - WHAT EACH SEAT HELD, for the reveal-hands eye. A frame is masked for one
 *     viewer, so a spectator's frames show backs. Rather than deduce identities,
 *     replay the code once per seat and read each seat's own hand out of its own
 *     frames — exact, by construction. It is not expensive: a whole 3p game is
 *     ~7 KB of frames per viewer and four replays land in ~1 ms, because the
 *     arithmetic decode is the cost and it is tiny.
 * ========================================================================== */

import {
    replayEventFrames, replayStepIndex, REPLAY_STEP, ReplayStepInfo,
} from '@sdk/ts/wasm/bots.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import { AnimationSequenceMessage, FeedAnimationEvent } from '../state/animationFeed';
import { undealtBoard } from '../state/clientBoards';
import { pushToSequence, type ViewEvent } from '../state/pushSequence';
import { type TableView, type ViewCard as Card } from '../state/view';

export { REPLAY_STEP } from '@sdk/ts/wasm/bots.ts';

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
    /** REPLAY_STEP.* — what the kernel says this step played. */
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
    /** How many recorded MOVES this step is: 1, except a merged cover run.
     *  The extras' gaps are one per move, so the clock counts these, not steps. */
    moves: number;
    /** This frame's index in the KERNEL's step stream, which a merged cover run
     *  makes different from its index here. Every call that addresses the wire
     *  by step - replayStepMaskedState, replayStepLogs - takes this, not the
     *  array position. */
    step: number;
    /** The attack card being covered (COVER only). */
    target: Card | null;
    /** Cards moved but not shown individually (the discard count, hidden draws). */
    count: number;
    /** Ready to publish into the animation feed — the events + the committed board. */
    seq: AnimationSequenceMessage;
    /** This step's committed board, with every seat's hand attached. */
    game: ReplayGameState;
}

/* The event a step's status line describes. A step's frame carries the action
 * AND everything it caused (a cover that ends a bout brings the discard and the
 * refills with it, exactly as live play does), so the kernel's kind picks which
 * of those events is the one being narrated. */
const NARRATED_EVENT: Record<number, string> = {
    [REPLAY_STEP.DEAL]: 'flipped',        // the deal's news is the trump
    [REPLAY_STEP.ATTACK]: 'attack_pass',
    [REPLAY_STEP.PASS]: 'attack_pass',
    [REPLAY_STEP.COVER]: 'cover',
    [REPLAY_STEP.PICKUP]: 'pickup',
    [REPLAY_STEP.ROUND_END]: 'cards_to_trash',
    // GOOD moves no cards — nothing to narrate but the seat.
};

/* A replay's frames name no one on the wire, and a replay has no one signed in:
 * a code carries no player ids at all. So a frame is read with no identity, and
 * its seats keep the kernel's empty ids - the page names a seat by its index
 * (state/view.ts seatKey) - and take the names the code's extras give them (or
 * P1, P2...). The board carries the replay's own id, the code itself, which is
 * longer than a table's id may be. Both are the host's display strings, set on
 * the snapshot after the read; no field of the game is. */
interface Naming { names: string[]; gameId: string }

const named = (v: TableView, naming: Naming): TableView => ({
    ...v,
    gameId: naming.gameId,
    seats: v.seats.map((s, i) => ({ ...s, name: naming.names[i] ?? '' })),
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

export interface ReplayFramesOpts {
    /** Seat whose eyes the replay is watched through; -1 (default) = spectator.
     *  The shared-replay screen is a spectator; the tutorial sits in seat 0, so
     *  its boards mask exactly as they would for a real player in that seat. */
    viewer?: number;
    /** The loser's seat, marked in the closing board's name — the one thing the
     *  board itself does not carry. */
    fool?: number | null;
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

    const index: ReplayStepInfo[] = replayStepIndex(code);
    const main = replayEventFrames(code, viewer);
    if (main.length !== index.length) {
        throw new Error(`replay: ${main.length} frames for ${index.length} steps`);
    }

    // Player count comes from the frames, not from a header we would have to
    // trust separately: read step 0 once, then name the seats it has.
    const probe = readFrame(main[0], null);
    if (!probe) throw new Error('replay: the opening frame did not decode');
    const n = probe.game.seats.length;
    const seats: Naming = { names: Array.from({ length: n }, (_, s) => names?.[s] || `P${s + 1}`), gameId };

    // One replay per seat, read for that seat's own hand. This is the reveal
    // eye's whole source of truth — see the header.
    const perSeat = Array.from({ length: n }, (_, s) => replayEventFrames(code, s));

    const built: ReplayFrame[] = main.map((bytes, i): ReplayFrame => {
        const seq = readFrame(bytes, seats);
        if (!seq) throw new Error(`replay: step ${i} did not decode`);

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
            moves: 1,
            step: i,
            ...(info.kind === REPLAY_STEP.COVER && ev?.cards?.[0] && ev?.target_card
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
    return mergeCoverRuns(built);
}

// ONE MOVE IS ONE STEP. The replay coder groups an attack and a pass - both
// have a continuation loop - but codes a cover one pair at a time
// (c/src/replay.c atom_cover), so a defender who takes three attacks in one
// move comes back as three steps. Measured on a 65-step game: attacks arrive
// carrying 1, 2 or 3 cards, and all 21 covers arrive carrying exactly 1.
//
// The kernel reconstructs this rather than storing it, and iMessage has been
// reading it that way all along: it animates a TURN, found by walking back over
// consecutive steps of the same actor (c/ios/ios_api_replay.c). The same rule
// here, for covers only - the one kind the wire is known to split - so the
// board plays a double cover as one move, the scrubber counts it once, and the
// Oracle deliberates it once. Anything downstream that used to see two steps
// now sees one, which is what the game did.
function mergeCoverRuns(frames: ReplayFrame[]): ReplayFrame[] {
    const out: ReplayFrame[] = [];
    for (let i = 0; i < frames.length; i++) {
        const f = frames[i];
        let j = i;
        while (j + 1 < frames.length && frames[j + 1].kind === REPLAY_STEP.COVER
               && frames[j + 1].seat === f.seat && f.kind === REPLAY_STEP.COVER) j++;
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
            cards: run.flatMap((r) => r.cards.map((c) => ({ ...c }))),
            named: run.reduce((n, r) => n + r.named, 0),
            count: run.reduce((n, r) => n + r.count, 0),
            pairs: run.flatMap((r) => r.pairs ?? []),
            moves: run.reduce((n, r) => n + r.moves, 0),
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
 * engine cannot un-play a move. So a step back INVERTS the flight — cards that
 * flew hand->table on the way in fly table->hand on the way out — and lands on
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

/**
 * Absolute unix time (seconds) per step, from the extras blob: the deal is the
 * start time, each information-bearing step advances the clock by its recorded
 * gap, and everything else happens "at the same moment" as the action that
 * caused it.
 *
 * The gaps are recorded one per attack/cover/pass/pickup, and those are exactly
 * the step kinds below — one gap, one step. Goods carry no gap (v6 keeps only a
 * trailing one, and the rest are reconstructed), so they inherit the clock like
 * any derived step.
 */
const TIMED_KINDS: number[] = [
    REPLAY_STEP.ATTACK, REPLAY_STEP.COVER, REPLAY_STEP.PASS, REPLAY_STEP.PICKUP,
];

export function stepTimes(
    frames: ReplayFrame[],
    startTime: number | null,
    moveGaps: number[] | null,
): (number | null)[] {
    if (startTime === null || !moveGaps) return frames.map(() => null);
    let t = startTime;
    let g = 0;
    return frames.map((f) => {
        // A merged cover run is several recorded moves on one step, and the
        // extras hold one gap per move: spend them all or every later step drifts.
        if (TIMED_KINDS.includes(f.kind)) {
            for (let k = 0; k < f.moves && g < moveGaps.length; k++) t += moveGaps[g++];
        }
        return t;
    });
}
