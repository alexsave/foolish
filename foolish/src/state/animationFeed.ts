import type { TableView, ViewCard } from './view';

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
    seat?: number;   // the acting seat
    cards?: ViewCard[]; // card backs are {suit:-1, value:-1}
    from_location?: 'deck' | 'hand' | 'table' | 'discard';
    to_location?: 'deck' | 'hand' | 'table' | 'discard' | 'flipped';
    target_card?: ViewCard;
    battle_index?: number;
    message?: string;
    /** the board to commit once this event's animation lands */
    game_state: TableView;
}

export interface AnimationSequenceMessage {
    type: 'animation_sequence';
    events: FeedAnimationEvent[];
    /** the final board, committed when the whole sequence has played */
    game: TableView;
    /** Committed games.version this sequence reflects. Present on live broadcasts
     *  (stamped by the server); absent for replay-synthesized sequences. The
     *  consumer uses it to drop sequences that arrive out of order under realtime
     *  latency, so the board never animates backwards. */
    version?: number;
}

/**
 * A live realtime message: the kernel's cast (c/src/evwire.h - the committed
 * version and the as3 push, one run of bytes), base64 because the realtime
 * API's envelope is JSON. Nothing rides beside it: the consumer
 * (AnimationProvider) hands the bytes to the kernel's client slot and gets the
 * sequence back. Replay/tutorial synthesis still publishes an
 * AnimationSequenceMessage directly.
 */
export interface CastMessage {
    /** base64(cast bytes) */
    b: string;
    /** Attached by the transport (RealtimeAnimationFeed): the game the topic
     *  belongs to, so the consumer can pick the identity to decode against. */
    game_id?: string;
}

type AnimationFeedMessage = AnimationSequenceMessage | CastMessage;

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
