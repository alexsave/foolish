import { PersonalGame } from "../../common/types";
import { CardBack } from "./CardBack";
import { CardFace } from "./CardFace";
import { useServer } from "../../contexts/ServerContext";
import { SuitIcon } from "../SovietIcon";

export const DeckAndFlipped = () => {
    const game: PersonalGame = useServer().game as PersonalGame;
    return <div style={{ display: 'flex', position: 'absolute', top: '0px', left: '0px', flexDirection: 'column', justifyContent: 'center', alignItems: 'center', height: '240px', width: '100px' }}>
        {game.deck_length > 0 && (
            <div style={{ position: 'relative' }} data-location="deck">
                <CardBack deckSize={game.deck_length} />
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
                    {game.deck_length + (game.flipped ? 1 : 0)}
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