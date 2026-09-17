import { useServer } from "../../contexts/ServerContext";
import { rulesOf, type TableView, type ViewSeat } from "../../state/view";
import { useFernFractal } from "../../utils/fernFractal";
import { useStyles } from "../../contexts/StyleContext";
import { useState, useEffect, useRef } from "react";
import { SovietIcon } from "../SovietIcon";
import { SovietCardBack } from "./SovietCardBack";

// Mini stacked card back — same SVG as the full CardBack, absolutely positioned
// to fill the stacked ring slot.
const MiniSovietCardBack = () => (
    <SovietCardBack style={{ position: 'absolute', top: 0, left: 0 }} />
);

const CardsVisual = ({ player, selfHandLength, isSelf }: { player: ViewSeat, selfHandLength?: number, isSelf: boolean }) => {
    const styles = useStyles();
    const { fernPattern } = useFernFractal();

    const hasPattern = styles.miniCard.usePattern && !!fernPattern;
    const cardWidth = 25;
    const cardHeight = cardWidth * 1.4;
    const displayHandLength = selfHandLength !== undefined ? selfHandLength : player.handCount;

    // Mark this container as the deal/refill destination for *other* players only.
    // Self's real hand is rendered by ActionButtons with the same data-* attrs;
    // tagging this mini-hand too would let querySelector pick the wrong target.
    const handAttrs = isSelf
        ? {}
        : { 'data-location': 'hand', 'data-player-id': player.id };

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
                    <div key={`player-${player.id}-card-${cardIndex}`} style={style}>
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
                    zIndex: 100,
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
    const game = useServer().view as TableView;
    const { chatMessages } = useServer();
    const styles = useStyles();
    const self_index = game.mySeat;
    // The sword's seat is the kernel's (client_view_rules): the next bout's lead,
    // on an empty table, once the deal has turned the trump.
    const swordSeat = rulesOf(game).firstAttackerBadge;

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
            {game.seats.map((player, index) => {
                const visual_index = (index - self_index + game.seats.length) % game.seats.length;
                const radians = 2 * Math.PI * visual_index / game.seats.length;
                const x = ((-1 * Math.sin(radians) * 35) + 50) + '%';
                const y = ((Math.cos(radians) * 35) + 50) + '%';
                const bubble = chatBubbles[player.id];

                return (
                    <div key={player.id} style={{
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
                        {/* TODO(ios-parity): iMessage board shifts the defender shield
                            the moment a pass is staged (before defender_move lands) —
                            that immediacy feels good; consider mirroring. (This ring
                            only draws the first-attacker sword above; the web
                            defender shield itself is the kernel's defender badge in
                            DefenderShield.tsx, rendered as a sibling in GameBoard.) */}
                        {index === swordSeat ? (
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
                            selfHandLength={index === self_index ? game.myHand.length : undefined}
                            isSelf={index === self_index}
                        />
                    </div>
                );
            })}
        </>
    );
};
