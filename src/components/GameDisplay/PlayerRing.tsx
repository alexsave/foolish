import { PersonalGame, PublicPlayer } from "../../common/types";
import { useAuth } from "../../contexts/AuthContext";
import { useServer } from "../../contexts/ServerContext";
import { useFernFractal } from "../../utils/fernFractal";
import { useState, useEffect, useRef } from "react";

const CardsVisual = ({ player, selfHandLength }: { player: PublicPlayer, selfHandLength?: number }) => {
    const { fernPattern } = useFernFractal();

    const cardWidth = 25;
    const cardHeight = cardWidth * 1.4;

    // Use selfHandLength for current user (optimistic), otherwise use server's hand_length
    const displayHandLength = selfHandLength !== undefined ? selfHandLength : player.hand_length;

    return <div style={{
        display: 'flex',
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'center',
        position: 'relative',
        height: '20px',
        width: '100px',
    }} data-location="hand" data-player-id={player.player_id}>
        {Array.from({ length: displayHandLength }).map((_, cardIndex) => {
            const mid = (displayHandLength - 1) / 2;
            const halfCardWidth = cardWidth / 2; // Updated for 5:7 ratio
            const halfDivWidth = 100 / 2;
            const style: React.CSSProperties = {
                //display: 'block',
                boxSizing: 'border-box',
                backgroundColor: fernPattern ? '#000000' : `rgb(180, 14, 9)`, // Dark red while loading, black when pattern loaded
                width: cardWidth + 'px', // 5:7 ratio - width
                height: cardHeight + 'px', // 5:7 ratio - height  
                borderRadius: '3px',
                border: '1px solid #8B0000', // Same dark red border
                position: 'absolute',
                left: `${halfDivWidth + (cardIndex - mid) * 10 - halfCardWidth}px`,
                zIndex: cardIndex,
                boxShadow: '0 1px 2px rgba(0,0,0,0.3)',
                backgroundImage: fernPattern ? `url(${fernPattern})` : undefined,
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
            {displayHandLength}
        </div>
    </div>
}

export const PlayerRing = () => {
    const game: PersonalGame = useServer().game as PersonalGame;
    const { chatMessages } = useServer();
    const { user_id } = useAuth();
    const self_index = game.players.findIndex(p => p.player_id === user_id);

    // Track active chat bubbles: player_id -> {message, timestamp}
    const [chatBubbles, setChatBubbles] = useState<{ [playerId: string]: { message: string; timestamp: number } }>({});
    const lastMessageIdRef = useRef<number | null>(null);

    // Listen for new chat messages and create bubbles
    useEffect(() => {
        if (!chatMessages || chatMessages.length === 0) {
            return;
        }

        const latestMessage = chatMessages[chatMessages.length - 1];

        // Check if this is a new message we haven't processed yet
        if (latestMessage.id && latestMessage.id !== lastMessageIdRef.current) {
            lastMessageIdRef.current = latestMessage.id;

            const senderId = latestMessage.user_id;
            const message = latestMessage.message;

            // Add the bubble
            setChatBubbles(prev => ({
                ...prev,
                [senderId]: {
                    message,
                    timestamp: Date.now()
                }
            }));

            // Remove it after 8 seconds
            setTimeout(() => {
                setChatBubbles(prev => {
                    const newBubbles = { ...prev };
                    // Only remove if it's the same message (in case a new one was added)
                    if (newBubbles[senderId]?.message === message) {
                        delete newBubbles[senderId];
                    }
                    return newBubbles;
                });
            }, 8000);
        }
    }, [chatMessages]);

    return <> {
        game.players.map((player, index) => {

            const visual_index = (index - self_index + game.players.length) % game.players.length;
            const radians = (2) * Math.PI * visual_index / (game.players.length);
            const x = ((-1 * Math.sin(radians) * 35) + 50) + '%';
            const y = ((Math.cos(radians) * 35) + 50) + '%';

            // Check if this player has an active chat bubble
            const bubble = chatBubbles[player.player_id];

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
                    height: '30px',
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'center',
                    position: 'relative'
                }}>
                    {player.name}

                    {/* Chat bubble */}
                    {bubble && (
                        <div style={{
                            position: 'absolute',
                            bottom: '100%',
                            left: '50%',
                            transform: 'translateX(-50%)',
                            backgroundColor: 'rgba(0, 0, 0, 0.85)',
                            color: 'white',
                            padding: '6px 10px',
                            borderRadius: '12px',
                            fontSize: '11px',
                            whiteSpace: 'nowrap',
                            maxWidth: '150px',
                            overflow: 'hidden',
                            textOverflow: 'ellipsis',
                            marginBottom: '5px',
                            border: '1px solid rgba(255, 255, 255, 0.3)',
                            boxShadow: '0 2px 8px rgba(0, 0, 0, 0.4)',
                            zIndex: 1000,
                            animation: 'fadeIn 0.3s ease-in'
                        }}>
                            {bubble.message}
                            {/* Speech bubble pointer */}
                            <div style={{
                                position: 'absolute',
                                bottom: '-6px',
                                left: '50%',
                                transform: 'translateX(-50%)',
                                width: '0',
                                height: '0',
                                borderLeft: '6px solid transparent',
                                borderRight: '6px solid transparent',
                                borderTop: '6px solid rgba(0, 0, 0, 0.85)'
                            }} />
                        </div>
                    )}
                </p>

                {/* Cards area (bottom) */}
                {player.hand_length && player.hand_length > 0 ?

                    <CardsVisual
                        player={player}
                        selfHandLength={player.player_id === user_id ? game.self?.hand.length : undefined}
                    />
                    : <div style={{ height: '20px' }} />
                }
            </div>
        })
    }
    </>
};