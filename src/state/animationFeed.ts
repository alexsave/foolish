import { Card, PersonalGame, PublicGame } from '@shared/core/types.ts';

/**
 * The animation feed: a tiny pub/sub carrying "animation sequence" messages —
 * the unit of game-state change the UI animates through. AnimationProvider
 * (AnimationContext.tsx) is the single consumer; producers are:
 *
 *   - RealtimeAnimationFeed: the live game's supabase broadcast channel
 *     (mounted inside ProtectedRoute, where auth + game id exist), and
 *   - the replay player (src/replay/), which synthesizes the same message
 *     shape from a decoded replay integer.
 *
 * Keeping this module free of React and supabase is the point: anything that
 * can produce these messages drives the full game UI, animations included.
 */

/** One animated event, shaped like the server's PublicAnimationEvent. */
export interface FeedAnimationEvent {
    type:
        | 'magic_transition'
        | 'deal'
        | 'flipped'
        | 'defender_move'
        | 'attack_pass'
        | 'cover'
        | 'pickup'
        | 'discard'
        | 'out'
        | 'refill'
        | 'cards_to_trash';
    player_id?: string;
    cards?: Card[]; // card backs are {suit:-1, value:-1}
    from_location?: 'deck' | 'hand' | 'table' | 'discard';
    to_location?: 'deck' | 'hand' | 'table' | 'discard' | 'flipped';
    target_card?: Card;
    battle_index?: number;
    message?: string;
    /** game state to commit once this event's animation lands */
    game_state: PersonalGame | PublicGame;
}

export interface AnimationSequenceMessage {
    type: 'animation_sequence';
    sequence_id: string;
    timestamp: number;
    events: FeedAnimationEvent[];
    /** final game state, committed when the whole sequence has played */
    game: PersonalGame | PublicGame;
    /** Committed games.version this sequence reflects. Present on live broadcasts
     *  (stamped by the server); absent for replay-synthesized sequences. The
     *  consumer uses it to drop sequences that arrive out of order under realtime
     *  latency, so the board never animates backwards. */
    version?: number;
}

/**
 * Packed broadcast envelope (docs/PACKED_WIRE_CUTOVER.md): the server now
 * ships the whole personalized animation sequence as ONE kernel-format byte
 * buffer, base64 inside the JSON payload the realtime API requires. The
 * consumer (AnimationProvider) decodes it into the legacy sequence shape at
 * the render boundary; replay/tutorial synthesis still publishes the legacy
 * AnimationSequenceMessage directly.
 */
export interface PackedSequenceEnvelope {
    t: 'as2';
    /** sequence id (dedup key) */
    s: string;
    /** committed games.version (the monotonic reorder-drop token) */
    v: number;
    /** base64(evwire bytes) */
    b: string;
    /** Attached by the transport (RealtimeAnimationFeed): the game this
     *  envelope belongs to — the packed payload itself carries no JS state,
     *  so the consumer needs it to pick the decode roster. */
    game_id?: string;
}

export type AnimationFeedMessage = AnimationSequenceMessage | PackedSequenceEnvelope;

type FeedListener = (message: AnimationFeedMessage) => void;

class AnimationFeedBus {
    private listeners = new Set<FeedListener>();

    subscribe(listener: FeedListener): () => void {
        this.listeners.add(listener);
        return () => {
            this.listeners.delete(listener);
        };
    }

    publish(message: AnimationFeedMessage): void {
        for (const listener of this.listeners) {
            try {
                listener(message);
            } catch (e) {
                console.error('animationFeed listener failed:', e);
            }
        }
    }
}

export const animationFeed = new AnimationFeedBus();
