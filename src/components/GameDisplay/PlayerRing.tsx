import { PersonalGame, PublicPlayer } from "../../common/types";
import { useAuth } from "../../contexts/AuthContext";
import { useServer } from "../../contexts/ServerContext";
import { useFernFractal } from "../../utils/fernFractal";
import { useStyles } from "../../contexts/StyleContext";
import { useState, useEffect, useRef } from "react";
import { SovietIcon } from "../SovietIcon";

// Soviet card back - exact same as CardBack.tsx, SVG scales naturally
const MiniSovietCardBack = () => (
    <svg width="100%" height="100%" viewBox="0 0 50 70" preserveAspectRatio="none" style={{ display: 'block', position: 'absolute', top: 0, left: 0 }}>
        <rect x="0" y="0" width="50" height="70" fill="#B32929" />
        <rect x="2" y="2" width="46" height="66" fill="none" stroke="#E79743" strokeWidth="3" />
        <polygon 
            points="25,18 28.5,29 40,29 31,36.5 34.5,48 25,41 15.5,48 19,36.5 10,29 21.5,29" 
            fill="none" 
            stroke="#0A0A0A" 
            strokeWidth="3"
        />
        <polygon 
            points="25,18 28.5,29 40,29 31,36.5 34.5,48 25,41 15.5,48 19,36.5 10,29 21.5,29" 
            fill="none" 
            stroke="#F5E6C8" 
            strokeWidth="1.5"
        />
        <polygon 
            points="25,18 28.5,29 40,29 31,36.5 34.5,48 25,41 15.5,48 19,36.5 10,29 21.5,29" 
            fill="#E79743" 
        />
    </svg>
);

const CardsVisual = ({ player, selfHandLength, isSelf }: { player: PublicPlayer, selfHandLength?: number, isSelf: boolean }) => {
    const styles = useStyles();
    const { fernPattern } = useFernFractal();

    const hasPattern = styles.miniCard.usePattern && !!fernPattern;
    const cardWidth = 25;
    const cardHeight = cardWidth * 1.4;
    const displayHandLength = selfHandLength !== undefined ? selfHandLength : player.hand_length;

    // Mark this container as the deal/refill destination for *other* players only.
    // Self's real hand is rendered by ActionButtons with the same data-* attrs;
    // tagging this mini-hand too would let querySelector pick the wrong target.
    const handAttrs = isSelf
        ? {}
        : { 'data-location': 'hand', 'data-player-id': player.player_id };

    return (
        <div style={{
            display: 'flex',
            flexDirection: 'row',
            alignItems: 'center',
            justifyContent: 'center',
            position: 'relative',
            height: '20px',
            width: '100px',
        }} {...handAttrs}>
            {Array.from({ length: displayHandLength }).map((_, cardIndex) => {
                const mid = (displayHandLength - 1) / 2;
                const halfCardWidth = cardWidth / 2;
                const halfDivWidth = 100 / 2;
                const style: React.CSSProperties = {
                    boxSizing: 'border-box',
                    backgroundColor: hasPattern ? '#000000' : styles.miniCard.backgroundColor,
                    width: cardWidth + 'px',
                    height: cardHeight + 'px',
                    borderRadius: styles.miniCard.borderRadius,
                    border: styles.miniCard.border,
                    position: 'absolute',
                    left: `${halfDivWidth + (cardIndex - mid) * 10 - halfCardWidth}px`,
                    zIndex: cardIndex,
                    boxShadow: styles.miniCard.boxShadow,
                    backgroundImage: hasPattern ? `url(${fernPattern})` : undefined,
                    backgroundSize: '100% 100%',
                    backgroundRepeat: 'no-repeat',
                    overflow: 'hidden'
                };

                return (
                    <div key={`player-${player.player_id}-card-${cardIndex}`} style={style}>
                        {styles.miniCard.useSvgCardBack && <MiniSovietCardBack />}
                    </div>
                );
            })}

            {displayHandLength > 0 && (
                <div style={{
                    position: 'absolute',
                    top: '50%',
                    left: '50%',
                    transform: 'translate(-50%, -50%)',
                    color: 'var(--color-text-primary)',
                    fontSize: '10px',
                    fontWeight: 'bold',
                    zIndex: 15,
                    pointerEvents: 'none',
                    textShadow: styles.text.cardCountTextShadow
                }}>
                    {displayHandLength}
                </div>
            )}
        </div>
    );
};

export const PlayerRing = () => {
    const game: PersonalGame = useServer().game as PersonalGame;
    const { chatMessages } = useServer();
    const { user_id } = useAuth();
    const styles = useStyles();
    const self_index = game.players.findIndex(p => p.player_id === user_id);

    const [chatBubbles, setChatBubbles] = useState<{ [playerId: string]: { message: string; timestamp: number } }>({});
    const lastMessageIdRef = useRef<number | null>(null);

    useEffect(() => {
        if (!chatMessages || chatMessages.length === 0) return;

        const latestMessage = chatMessages[chatMessages.length - 1];

        if (latestMessage.id && latestMessage.id !== lastMessageIdRef.current) {
            lastMessageIdRef.current = latestMessage.id;

            // Only surface bubbles for live messages. On initial load / reconnect,
            // chatMessages is hydrated from history and the newest entry can be
            // hours old — without this guard it would pop up as if just sent.
            const ageMs = latestMessage.created_at
                ? Date.now() - new Date(latestMessage.created_at).getTime()
                : 0;
            if (ageMs > 10_000) return;

            const senderId = latestMessage.user_id;
            const message = latestMessage.message;

            setChatBubbles(prev => ({
                ...prev,
                [senderId]: { message, timestamp: Date.now() }
            }));

            setTimeout(() => {
                setChatBubbles(prev => {
                    const newBubbles = { ...prev };
                    if (newBubbles[senderId]?.message === message) {
                        delete newBubbles[senderId];
                    }
                    return newBubbles;
                });
            }, 8000);
        }
    }, [chatMessages]);

    return (
        <>
            {game.players.map((player, index) => {
                const visual_index = (index - self_index + game.players.length) % game.players.length;
                const radians = 2 * Math.PI * visual_index / game.players.length;
                const x = ((-1 * Math.sin(radians) * 35) + 50) + '%';
                const y = ((Math.cos(radians) * 35) + 50) + '%';
                const bubble = chatBubbles[player.player_id];

                return (
                    <div key={player.player_id} style={{
                        position: 'absolute',
                        top: y,
                        left: x,
                        display: 'flex',
                        flexDirection: 'column',
                        alignItems: 'center',
                        width: '80px',
                        height: '80px',
                        transform: 'translate(-50%, -50%)'
                    }}>
                        {index === game.first_attacker &&
                            game.table_battles.length === 0 &&
                            !(game.deck_length > 0 && game.flipped === null) ? (
                            <div style={{
                                fontSize: '16px',
                                height: '20px',
                                display: 'flex',
                                alignItems: 'center',
                                justifyContent: 'center'
                            }}>
                                <SovietIcon name="sword" size={16} />
                            </div>
                        ) : (
                            <div style={{ height: '20px' }} />
                        )}

                        <div style={{
                            margin: 0,
                            fontSize: '12px',
                            color: 'var(--color-text-primary)',
                            textAlign: 'center',
                            height: '30px',
                            display: 'flex',
                            alignItems: 'center',
                            justifyContent: 'center',
                            position: 'relative'
                        }}>
                            {player.name}

                            {bubble && (
                                <div className={`chat-bubble ${!styles.icons.useEmojiIcons ? 'chat-bubble--soviet' : ''}`}>
                                    {bubble.message}
                                    <div className="chat-bubble__pointer" />
                                </div>
                            )}
                        </div>

                        {/* Always render so DEAL/REFILL animations have a destination element to target, even when hand_length === 0 */}
                        <CardsVisual
                            player={player}
                            selfHandLength={player.player_id === user_id ? game.self?.hand.length : undefined}
                            isSelf={player.player_id === user_id}
                        />
                    </div>
                );
            })}
        </>
    );
};
