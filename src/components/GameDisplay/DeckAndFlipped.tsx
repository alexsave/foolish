import { PersonalGame } from "../../common/types";
import { CardBack } from "./CardBack";
import { CardFace } from "./CardFace";
import { useServer } from "../../contexts/ServerContext";
import { useAnimation } from "../../contexts/AnimationContext";
import { SuitIcon } from "../SovietIcon";

export const DeckAndFlipped = () => {
    const game: PersonalGame = useServer().game as PersonalGame;
    const { inFlightFromDeck, inFlightToFlipped } = useAnimation();

    // Cards mid-flight FROM the deck pile drive the visible pile size.
    // Cards mid-flight TO the flipped slot are still in the deck system, so
    // they count toward the badge total even though they've left the pile.
    const displayedDeckLength = Math.max(0, game.deck_length - inFlightFromDeck);
    const badgeTotal = displayedDeckLength + (game.flipped ? 1 : 0) + inFlightToFlipped;

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
                    {badgeTotal}
                </p>
            </div>
        )}
        {/* Reserve the flipped slot in the layout from the start so the deck
            pile doesn't shift when the flipped card lands. */}
        <div
            data-location="flipped"
            style={{
                marginTop: displayedDeckLength > 0 ? '-30px' : '0px',
                width: '50px',
                height: '70px',
                zIndex: 0,
            }}
        >
            {game.flipped && <CardFace card={game.flipped} playerId="flipped" />}
        </div>
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