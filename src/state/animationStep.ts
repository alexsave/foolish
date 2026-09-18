// animationStep.ts - one step the web's animation pipeline plays.
//
// pushSequence.ts names a step the KERNEL read out of a push (ViewEvent). This
// file names the step the pipeline actually queues, which is that plus the
// fields only a client has: the revert flag a doomed prediction carries, and the
// two thunks a prediction's board is committed through. A push's step never
// carries either - a push's board is truth the moment it arrives - so the extra
// fields are exactly the difference between "the server moved" and "I moved and
// nobody has answered yet".
//
// It lives beside pushSequence.ts rather than inside AnimationContext.tsx
// because three modules now name it: the provider that queues the steps, the
// frame loop it is generic over (useAnimationRun.ts), and the conflict
// resolution that builds the revert steps (optimisticResolve.ts).

import type { TableView, ViewCard as Card } from './view';

export interface ClientAnimationEvent {
    type: 'magic_transition' | 'deal' | 'flipped' | 'defender_move' | 'attack_pass' | 'cover' | 'pickup' | 'discard' | 'out' | 'refill' | 'cards_to_trash' | 'revert';
    seat?: number;   // the acting seat
    cards?: readonly Card[];
    from_location?: 'deck' | 'hand' | 'table' | 'discard';
    to_location?: 'deck' | 'hand' | 'table' | 'discard' | 'flipped';
    target_card?: Card;
    target_cards?: readonly Card[]; // For multi-card cover animations
    battle_index?: number;
    message?: string;
    game_state?: TableView; // the board after this event
    is_revert?: boolean; // CLIENT-ONLY: flag for reverted optimistic animations
    // CLIENT-ONLY: whether this step's board is still worth committing when its
    // flight lands. A predicted move's board rides its own flight (there is no
    // second timer for it any more), and a refusal that arrives mid-flight must
    // stop it landing - otherwise the board appears and the revert takes it away
    // one frame later. Only a prediction carries one; a push's board is truth.
    commit_if?: () => boolean;
    // CLIENT-ONLY: a board this step only knows at its LANDING. A prediction's
    // board is the kernel's edit of whatever is on screen when its flight lands,
    // not of what was on screen when the card was tapped: a broadcast can commit
    // fresher state inside that window, and a board derived at tap time would
    // write the stale table and hand back over it.
    commit_board?: () => TableView | null;
}
