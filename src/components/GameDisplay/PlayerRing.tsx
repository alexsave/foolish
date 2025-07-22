import { useEffect, useState } from "react";
import { PersonalGame, PublicPlayer } from "../../common/types";
import { useAuth } from "../../contexts/AuthContext";
import { useServer } from "../../contexts/ServerContext";
import { generateCardBackPattern } from "../../utils/cards";

const CardsVisual = ({ player, playerCardPatternDataUrl }: { player: PublicPlayer, playerCardPatternDataUrl: string }) => {

    return <div style={{
        display: 'flex',
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'center',
        position: 'relative',
        height: '20px',
        width: '100px'
    }} data-location="hand" data-player-id={player.player_id}>
        {Array.from({ length: player.hand_length }).map((_, cardIndex) => {
            const mid = (player.hand_length + 1) / 2;
            const halfCardWidth = 10 / 2; // Updated for 5:7 ratio
            const halfDivWidth = 100 / 2;
            const style: React.CSSProperties = {
                backgroundColor: '#DC143C', // Fallback red background
                width: '10px', // 5:7 ratio - width
                height: '14px', // 5:7 ratio - height  
                borderRadius: '2px',
                border: '1px solid #8B0000', // Same dark red border
                position: 'absolute',
                left: `${halfDivWidth + (cardIndex - mid) * 2 - halfCardWidth}px`,
                zIndex: cardIndex,
                boxShadow: '0 1px 2px rgba(0,0,0,0.3)',
                backgroundImage: playerCardPatternDataUrl ? `url(${playerCardPatternDataUrl})` : undefined,
                backgroundSize: '100% 100%',
                backgroundRepeat: 'no-repeat'

            }

            // Calculate proper centering: total span divided by 2, then offset each card


            return <div
                key={`player-${player.player_id}-card-${cardIndex}`}
                style={style}
            />;

        })}

        {/* Card count overlay */}
        <div style={{
            position: 'absolute',
            top: '50%',
            left: '50%',
            transform: 'translate(-50%, -50%)',
            color: 'white',
            fontSize: '10px',
            fontWeight: 'bold',
            zIndex: 15,
            pointerEvents: 'none',
            textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
        }}>
            {player.hand_length}
        </div>
    </div>
}

export const PlayerRing = () => {
    const [playerCardPatternDataUrl, setPlayerCardPatternDataUrl] = useState<string>('');
    useEffect(() => {
        // Generate the pattern for player cards once when component mounts
        generateCardBackPattern(12, 18).then(dataUrl => {
            setPlayerCardPatternDataUrl(dataUrl);
        });
    }, []);

    const game: PersonalGame = useServer().game as PersonalGame;
    const { user_id } = useAuth();
    const self_index = game.players.findIndex(p => p.player_id === user_id);
    return <> {
        game.players.map((player, index) => {

            const visual_index = (index - self_index + game.players.length) % game.players.length;
            const radians = (2) * Math.PI * visual_index / (game.players.length);
            const x = ((-1 * Math.sin(radians) * 35) + 50) + '%';
            const y = ((Math.cos(radians) * 35) + 50) + '%';

            return <div key={player.player_id} style={{
                position: 'absolute',
                top: y,
                left: x,
                display: 'flex',
                flexDirection: 'column',
                alignItems: 'center',
                width: '80px',
                height: '80px',
                transform: 'translate(-50%, -50%)' // Center the element relative to its position
            }}>


                {/* Sword area (top) - either sword or empty space */}
                {index === game.first_attacker && 
                 game.table_battles.length === 0 && 
                 !(game.deck_length > 0 && game.flipped === null) ? <div style={{
                    fontSize: '16px',
                    height: '20px',
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'center'
                }}>
                    ⚔️
                </div>
                    : <div style={{ height: '20px' }} />
                }

                {/* Player name (center) */}
                <p style={{
                    margin: 0,
                    fontSize: '12px',
                    color: 'white',
                    textAlign: 'center',
                    height: '20px',
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'center'
                }}>
                    {player.name}
                </p>

                {/* Cards area (bottom) */}
                {player.hand_length && player.hand_length > 0 ?


                    <CardsVisual player={player} playerCardPatternDataUrl={playerCardPatternDataUrl} />
                    : <div style={{ height: '20px' }} />
                }
            </div>
        })
    }
    </>
};