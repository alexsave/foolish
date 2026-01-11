import { PersonalGame } from "../../common/types";
import { CardBack } from "./CardBack";
import { useServer } from "../../contexts/ServerContext";

export const DiscardPile = () => {
    const game: PersonalGame = useServer().game as PersonalGame;
    
    return (
        <div style={{ 
            display: 'flex', 
            position: 'absolute', 
            top: '0px', 
            right: '0px', 
            flexDirection: 'column', 
            justifyContent: 'center', 
            alignItems: 'center', 
            height: '240px', 
            width: '100px' 
        }}>
            <div style={{ position: 'relative' }} data-location="discard">
                {/* Always render the discard pile area for animations, but only show cards if there are any */}
                {game.discard_pile_length > 0 && (
                    <>
                        <CardBack deckSize={game.discard_pile_length} enableRandomRotation />
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
                            {game.discard_pile_length}
                        </p>
                    </>
                )}
            </div>
        </div>
    );
}; 