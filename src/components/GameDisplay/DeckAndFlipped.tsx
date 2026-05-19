import { PersonalGame } from "../../common/types";
import { CardBack } from "./CardBack";
import { CardFace } from "./CardFace";
import { useServer } from "../../contexts/ServerContext";
import { useAnimation } from "../../contexts/AnimationContext";
import { SuitIcon } from "../SovietIcon";

export const DeckAndFlipped = () => {
    const game: PersonalGame = useServer().game as PersonalGame;
    const { currentAnimation, getCardAnimationState } = useAnimation();

    // Cards mid-flight FROM the deck haven't yet had their snapshot applied
    // (game_state commits at animation end). Subtract them so the deck visual
    // and badge stay consistent with what's flying.
    //
    // Use getCardAnimationState to detect whether the animation is actively
    // running — currentAnimation persists through the ~100ms gap *after*
    // game_state commits and before the next animation starts, and during
    // that window subtracting would double-deduct (state is already correct).
    const firstCard = currentAnimation?.cards?.[0];
    const isLive = firstCard
        ? getCardAnimationState(firstCard, currentAnimation?.player_id).isAnimating
        : false;
    const inFlightFromDeck =
        isLive && currentAnimation?.from_location === 'deck' && currentAnimation.cards
            ? currentAnimation.cards.length
            : 0;
    const displayedDeckLength = Math.max(0, game.deck_length - inFlightFromDeck);

    return <div style={{ display: 'flex', position: 'absolute', top: '0px', left: '0px', flexDirection: 'column', justifyContent: 'center', alignItems: 'center', height: '240px', width: '100px' }}>
        {displayedDeckLength > 0 && (
            <div style={{ position: 'relative' }} data-location="deck">
                <CardBack deckSize={displayedDeckLength} />
                <p style={{
                    position: 'absolute',
                    top: '50%',
                    left: '50%',
                    transform: 'translate(-50%, -50%)',
                    margin: 0,
                    color: 'var(--color-text-primary)',
                    fontSize: '16px',
                    fontWeight: 'bold',
                    textShadow: '1px 1px 2px rgba(0,0,0,0.8)',
                    pointerEvents: 'none',
                    zIndex: 1003
                }}>
                    {displayedDeckLength + (game.flipped ? 1 : 0)}
                </p>
            </div>
        )}
        {game.flipped && (
            <div style={{ marginTop: game.deck_length > 0 ? '-30px' : '0px', zIndex: 0 }} data-location="flipped">
                <CardFace card={game.flipped} playerId="flipped" />
            </div>
        )}
        {/* Trump indicator appears when deck and flipped card are gone */}
        {game.deck_length === 0 && !game.flipped && (
            <div style={{
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center'
            }}>
                <SuitIcon suit={game.power_suit} size={64} />
            </div>
        )}
    </div>
};