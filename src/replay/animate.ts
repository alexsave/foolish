/* =============================================================================
 * Replay animation sequences (client-only presentation helper)
 * =============================================================================
 * Converts the decoded replay's step stream into the SAME animation-sequence
 * messages the server broadcasts during a live game (see
 * supabase/functions/_shared/actions/* events and utils.ts
 * broadcastAnimationEvents). Published into src/state/animationFeed, they
 * drive the real AnimationProvider — flying cards, deck drain, the lot.
 *
 * sequences[i] animates the transition INTO steps[i]'s state:
 *   - sequences[0] is the opening deal (deal each hand, flip the trump,
 *     place the defender), ending in the GAME_START state;
 *   - sequences[i>0] is one game event (attack/cover/draw/...), ending in
 *     steps[i]'s state.
 * Seeking skips this module entirely: the player commits stepToGame(i)
 * straight into the game state.
 * ========================================================================== */

import { Card, LOG_TYPE } from '@shared/types.ts';
import {
    AnimationSequenceMessage,
    FeedAnimationEvent,
} from '../state/animationFeed';
import { DecodedReplay } from '@shared/replay/core.ts';
import { ReplayStep, ReplayGameState, stepToGame } from './view';

const HIDDEN: Card = { suit: -1, value: -1 };

const seatId = (seat: number) => `seat-${seat}`;

function battleCards(step: ReplayStep): Card[] {
    const out: Card[] = [];
    for (const b of step.battles) {
        out.push(b.attack);
        if (b.defense) out.push(b.defense);
    }
    return out;
}

/** The pre-deal state sequences[0] starts from: full face-down stock, empty
 *  hands, no trump flipped yet (the live client's "initialization" shape). */
export function preDealGame(
    d: DecodedReplay,
    step0: ReplayStep,
    gameId: string,
    names?: string[] | null,
): ReplayGameState {
    const deckSize = d.playerCount > 4 ? 52 : 36;
    const base = stepToGame(d, step0, gameId, names);
    return {
        ...base,
        deck_length: deckSize,
        flipped: null,
        players: base.players.map((p) => ({ ...p, hand_length: 0 })),
        table_battles: [],
        replay_hands: base.players.map(() => []),
    };
}

export function buildReplaySequences(
    d: DecodedReplay,
    steps: ReplayStep[],
    gameId: string,
    names?: string[] | null,
): AnimationSequenceMessage[] {
    const n = d.playerCount;
    const deckSize = n > 4 ? 52 : 36;
    const games = steps.map((s) => stepToGame(d, s, gameId, names));

    const sequences: AnimationSequenceMessage[] = [];

    /* ---- sequences[0]: the opening deal, mirroring start_game() ---- */
    {
        const step0 = steps[0];
        const final = games[0];
        const events: FeedAnimationEvent[] = [];

        // deal 6 to each seat in order; each snapshot drains the deck like the
        // live DEAL events do
        for (let s = 0; s < n; s++) {
            const state: ReplayGameState = {
                ...final,
                deck_length: deckSize - 6 * (s + 1),
                flipped: null,
                table_battles: [],
                players: final.players.map((p, idx) => ({
                    ...p,
                    hand_length: idx <= s ? 6 : 0,
                })),
                replay_hands: final.replay_hands.map((h, idx) =>
                    idx <= s ? h : [],
                ),
            };
            events.push({
                type: 'deal',
                player_id: seatId(s),
                cards: Array.from({ length: 6 }, () => ({ ...HIDDEN })),
                from_location: 'deck',
                to_location: 'hand',
                game_state: state,
            });
        }
        events.push({
            type: 'flipped',
            cards: [d.trumpCard],
            from_location: 'deck',
            to_location: 'flipped',
            game_state: { ...final, table_battles: [] },
        });
        const defenderSeat = step0.players.findIndex((p) => p.isDefender);
        events.push({
            type: 'defender_move',
            player_id: defenderSeat >= 0 ? seatId(defenderSeat) : undefined,
            game_state: final,
        });

        sequences.push({
            type: 'animation_sequence',
            sequence_id: '',
            timestamp: 0,
            events,
            game: final,
        });
    }

    /* ---- sequences[i>0]: one event each ---- */
    for (let i = 1; i < steps.length; i++) {
        const step = steps[i];
        const prev = steps[i - 1];
        const state = games[i];
        let event: FeedAnimationEvent;

        switch (step.kind) {
            case LOG_TYPE.ATTACK:
            case LOG_TYPE.PASS:
                event = {
                    type: 'attack_pass',
                    player_id: seatId(step.seat!),
                    cards: step.cards,
                    from_location: 'hand',
                    to_location: 'table',
                    game_state: state,
                };
                break;

            case LOG_TYPE.COVER: {
                const battleIndex = step.battles.findIndex(
                    (b) =>
                        b.attack.suit === step.target!.suit &&
                        b.attack.value === step.target!.value,
                );
                event = {
                    type: 'cover',
                    player_id: seatId(step.seat!),
                    cards: step.cards,
                    target_card: step.target!,
                    battle_index: battleIndex,
                    from_location: 'hand',
                    to_location: 'table',
                    game_state: state,
                };
                break;
            }

            case LOG_TYPE.PICKUP:
                event = {
                    type: 'pickup',
                    player_id: seatId(step.seat!),
                    cards: battleCards(prev),
                    from_location: 'table',
                    to_location: 'hand',
                    game_state: state,
                };
                break;

            case LOG_TYPE.DISCARD:
                event = {
                    type: 'cards_to_trash',
                    cards: battleCards(prev),
                    from_location: 'table',
                    to_location: 'discard',
                    game_state: state,
                };
                break;

            case LOG_TYPE.DRAW: {
                // hidden cards first, then the identified trump (the stock's
                // final card) — the order the engine logs them
                const cards: Card[] = [
                    ...Array.from({ length: step.count }, () => ({ ...HIDDEN })),
                    ...step.cards,
                ];
                event = {
                    type: 'refill',
                    player_id: seatId(step.seat!),
                    cards,
                    from_location: 'deck',
                    to_location: 'hand',
                    game_state: state,
                };
                break;
            }

            case LOG_TYPE.PLAYER_OUT:
                event = {
                    type: 'out',
                    player_id: seatId(step.seat!),
                    game_state: state,
                };
                break;

            case LOG_TYPE.DEFENDER_CHANGE: {
                const def = step.players.findIndex((p) => p.isDefender);
                event = {
                    type: 'defender_move',
                    player_id: def >= 0 ? seatId(def) : undefined,
                    game_state: state,
                };
                break;
            }

            // GOOD, GAME_START (never at i>0) and the synthetic end step are
            // pure state commits — no cards move
            default:
                event = {
                    type: 'magic_transition',
                    player_id: step.seat !== null ? seatId(step.seat) : undefined,
                    game_state: state,
                };
                break;
        }

        sequences.push({
            type: 'animation_sequence',
            sequence_id: '',
            timestamp: 0,
            events: [event],
            game: state,
        });
    }

    return sequences;
}

/* =============================================================================
 * Reverse sequences (one-step-back)
 * =============================================================================
 * reverse[i] animates the transition OUT of steps[i] back into steps[i-1] — the
 * mirror image of the forward sequence: cards that flew hand→table on the way
 * in fly table→hand on the way out, and the sequence commits steps[i-1]'s state
 * when it lands. reverse[0] is null (there's nothing before the opening deal,
 * and stepping back from step 0 just clamps).
 *
 * Card motions that have no clean visual inverse (drawing from a face-down
 * stock, the opening deal, pure rotations) collapse to a magic_transition: the
 * prior state is committed without a flight. Only seeking skips this entirely.
 * ========================================================================== */
export function buildReverseSequences(
    d: DecodedReplay,
    steps: ReplayStep[],
    gameId: string,
    names: string[] | null | undefined,
    forward: AnimationSequenceMessage[],
): (AnimationSequenceMessage | null)[] {
    const games = steps.map((s) => stepToGame(d, s, gameId, names));
    const reverse: (AnimationSequenceMessage | null)[] = [null];

    for (let i = 1; i < steps.length; i++) {
        const prevState = games[i - 1];
        // forward sequences[i>0] always carry exactly one event
        const fe = forward[i].events[forward[i].events.length - 1];
        let event: FeedAnimationEvent;

        switch (fe.type) {
            // hand→table on the way in ⇒ table→hand on the way out
            case 'attack_pass':
            case 'cover':
                event = {
                    type: 'pickup',
                    player_id: fe.player_id,
                    cards: fe.cards,
                    from_location: 'table',
                    to_location: 'hand',
                    game_state: prevState,
                };
                break;

            // table→hand on the way in ⇒ hand→table on the way out
            case 'pickup':
                event = {
                    type: 'attack_pass',
                    player_id: fe.player_id,
                    cards: fe.cards,
                    from_location: 'hand',
                    to_location: 'table',
                    game_state: prevState,
                };
                break;

            // table→discard on the way in ⇒ discard→table on the way out
            case 'cards_to_trash':
                event = {
                    type: 'pickup',
                    cards: fe.cards,
                    from_location: 'discard',
                    to_location: 'table',
                    game_state: prevState,
                };
                break;

            // draws (deck→hand, face-down), the opening deal, and pure rotations
            // have no honest reverse flight — just restore the prior state
            default:
                event = {
                    type: 'magic_transition',
                    player_id: fe.player_id,
                    game_state: prevState,
                };
                break;
        }

        reverse.push({
            type: 'animation_sequence',
            sequence_id: '',
            timestamp: 0,
            events: [event],
            game: prevState,
        });
    }

    return reverse;
}
