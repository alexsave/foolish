import { Card, AnimationEvent, ANIMATION_EVENT_TYPE } from './types.ts';

// Animation event manager for collecting events during operations
export class AnimationEventManager {
    private events: AnimationEvent[] = [];
    
    clear() {
        this.events = [];
    }
    
    getEvents(): AnimationEvent[] {
        return [...this.events];
    }
    
    addEvent(event: AnimationEvent) {
        this.events.push(event);
    }
    
    addAttackEvent(player_id: string, cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
            player_id,
            cards,
            from_location: 'hand',
            to_location: 'table'
        });
    }
    
    addPassEvent(player_id: string, cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.ATTACK_PASS,
            player_id,
            cards,
            from_location: 'hand',
            to_location: 'table'
        });
    }
    
    addCoverEvent(player_id: string, cover_card: Card, attack_card: Card, battle_index: number) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.COVER,
            player_id,
            cards: [cover_card],
            target_card: attack_card,
            battle_index,
            from_location: 'hand',
            to_location: 'table'
        });
    }
    
    addPickupEvent(player_id: string, cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.PICKUP,
            player_id,
            cards,
            from_location: 'table',
            to_location: 'hand'
        });
    }
    
    addMagicTransitionEvent(message: string) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.MAGIC_TRANSITION,
            message
        });
    }
    
    addDealEvent(player_id: string, cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.DEAL,
            player_id,
            cards,
            from_location: 'deck',
            to_location: 'hand'
        });
    }
    
    addFlippedEvent(card: Card) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.FLIPPED,
            cards: [card],
            from_location: 'deck',
            to_location: 'flipped'
        });
    }
    
    addDefenderMoveEvent(player_id: string) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.DEFENDER_MOVE,
            player_id
        });
    }
    
    addOutEvent(player_id: string) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.OUT,
            player_id
        });
    }
    
    addRefillEvent(player_id: string, cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.REFILL,
            player_id,
            cards,
            from_location: 'deck',
            to_location: 'hand'
        });
    }
    
    addCardsToTrashEvent(cards: Card[]) {
        this.addEvent({
            type: ANIMATION_EVENT_TYPE.CARDS_TO_TRASH,
            cards,
            from_location: 'table',
            to_location: 'discard'
        });
    }
} 