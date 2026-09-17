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

import { deckSizeFor } from '@api/core/constants.ts';
import {
    replayEventFrames, replayStepIndex, REPLAY_STEP, ReplayStepInfo,
} from '@sdk/ts/wasm/bots.ts';
import { clientTable } from '@sdk/ts/table/client_table.ts';
import { AnimationSequenceMessage, FeedAnimationEvent } from '../state/animationFeed';
import { pushToSequence, type ViewEvent } from '../state/pushSequence';
import { NO_CARD, type TableView, type ViewCard as Card } from '../state/view';

export { REPLAY_STEP } from '@sdk/ts/wasm/bots.ts';

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

/* A replay's frames name no one on the wire: the seats are named here, by the
 * table identity the kernel builds from the extras' names (or P1, P2...), with
 * player ids `seat-N`. Each board then carries the replay's own game id, which
 * is the code itself and longer than a table's id may be, so it is set on the
 * board after the read. A name the kernel refuses (too long for a roster name)
 * is set on the board the same way instead. Phase 7 retires the `seat-N` ids. */
interface Naming { identity: Uint8Array | null; ids: string[]; names: string[]; gameId: string }

const named = (v: TableView, naming: Naming): TableView => ({
    ...v,
    gameId: naming.gameId,
    seats: naming.identity ? v.seats : v.seats.map((s, i) => ({ ...s, id: naming.ids[i] ?? `seat-${i}`, name: naming.names[i] ?? '' })),
});

const readFrame = (bytes: Uint8Array, naming: Naming | null) => {
    const read = clientTable().readPush(bytes, { as3: false, identity: naming?.identity ?? 'none' });
    if (!read) return null;
    if (!naming) return pushToSequence(read);
    const seq = pushToSequence({
        steps: read.steps.map((s) => ({ event: s.event, view: named(s.view, naming) })),
        final: named(read.final, naming),
    });
    return seq;
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
    const ids = Array.from({ length: n }, (_, s) => `seat-${s}`);
    const seatNames = ids.map((_, s) => names?.[s] || `P${s + 1}`);
    const identity = clientTable().identityFromSeats('replay', '', ids.map((id, s) => ({ id, name: seatNames[s], isAi: false })));
    const seats: Naming = { identity, ids, names: seatNames, gameId };

    // One replay per seat, read for that seat's own hand. This is the reveal
    // eye's whole source of truth — see the header.
    const perSeat = Array.from({ length: n }, (_, s) => replayEventFrames(code, s));

    return main.map((bytes, i) => {
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
}

/**
 * The state the opening deal animates OUT of: a full face-down stock, empty
 * hands, nothing flipped. The board before the first frame lands — the lobby
 * shape a live client sits in while waiting to be dealt to.
 */
export function preDealGame(first: ReplayFrame): ReplayGameState {
    return {
        ...first.game,
        deckCount: deckSizeFor(first.game.seats.length),
        hasFlipped: false,
        flipped: NO_CARD,
        battles: [],
        seats: first.game.seats.map((p) => ({ ...p, handCount: 0 })),
        replay_hands: first.game.seats.map(() => []),
    };
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
                    player_id: fe.player_id,
                    cards: fe.cards,
                    from_location: 'table',
                    to_location: 'hand',
                    game_state: prev,
                };
                break;

            // table->hand on the way in => hand->table on the way out
            case 'pickup':
                event = {
                    type: 'attack_pass',
                    player_id: fe.player_id,
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
                    player_id: fe.player_id,
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
        if (TIMED_KINDS.includes(f.kind) && g < moveGaps.length) t += moveGaps[g++];
        return t;
    });
}
