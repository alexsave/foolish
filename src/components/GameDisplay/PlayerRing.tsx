import { useServer } from "../../contexts/ServerContext";
import { PLAYER_STATUS, rulesOf, seatKey, type TableView, type ViewSeat } from "../../state/view";
import { useFernFractal } from "../../utils/fernFractal";
import { useStyles } from "../../contexts/StyleContext";
import { useLocalization } from "../../contexts/LocalizationContext";
import { useState, useEffect, useRef } from "react";
import { RoleMarkSize, RoleMarkView, type RoleMarkKind } from "../RoleMark";
import { SovietCardBack } from "./SovietCardBack";
import { botDisplayName } from "../../common/botName";

// Mini stacked card back — same SVG as the full CardBack, absolutely positioned
// to fill the stacked ring slot.
const MiniSovietCardBack = () => (
    <SovietCardBack style={{ position: 'absolute', top: 0, left: 0 }} />
);

const CardsVisual = ({ player, handKey, selfHandLength, isSelf }: { player: ViewSeat, handKey: string, selfHandLength?: number, isSelf: boolean }) => {
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
        : { 'data-location': 'hand', 'data-player-id': handKey };

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
                    <div key={`player-${handKey}-card-${cardIndex}`} style={style}>
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

/** THE ONE MARK a seat wears, from the kernel's answers about it.
 *
 *  NEVER TWO, and that is the point of ranking them here rather than letting two
 *  components each decide. The website used to draw the sword in this ring and
 *  the shield in a component of its own, so nothing stopped one seat wearing
 *  both - and on a finished game it did: e2e/fixtures/ui_dom/replay_named_end
 *  recorded Ada with the sword in her own slot and the shield floating beside it,
 *  because at game over the kernel's two badges can name the same seat. iMessage
 *  has the rule as a single function (`RoleMarkKind.worn`, ios/FoolishKit/Boards/
 *  FRoleMotion.swift: "a seat is never two of these at once"), which is a HOST
 *  function there too, so this is that function and not a second opinion.
 *
 *  Said good beats the shield beats the sword, and an out seat wears nothing
 *  (FSeatBadge turns an out seat's badge edge-on; selfRoleMark is `isOut ? nil`).
 *
 *  WHAT IS MISSING, and it needs the kernel: a THROW-IN attacker's plain sword.
 *  `first_attacker_badge` answers "who leads the NEXT bout" and goes to -1 the
 *  moment the table opens, so mid-bout this draws no sword at all, while the
 *  iMessage board gives every eligible attacker one and tints only the opener's.
 *  The rule is the kernel's `turn_may_act` (c/src/game.h) and it is not reachable
 *  from a board: it wants a per-seat mark on ViewRules. Deriving it here instead
 *  would be a TypeScript copy of a C rule, so it is not derived here. */
const markOf = (game: TableView, seat: number, player: ViewSeat): RoleMarkKind | null => {
    if (player.status === PLAYER_STATUS.OUT) return null;
    const rules = rulesOf(game);
    if ((game.goodMask >> seat) & 1) return 'check';
    if (rules.defenderBadge === seat) return 'shield';
    if (rules.firstAttackerBadge === seat) return 'leadSword';
    return null;
};

export const PlayerRing = () => {
    const { t } = useLocalization();
    const game = useServer().view as TableView;
    const { chatMessages } = useServer();
    const styles = useStyles();
    const self_index = game.mySeat;
    // The marks a seat can wear, named the way FoolishKit names them.
    const markLabel: Record<RoleMarkKind, string> = {
        shield: t('ios.a11y.defending'),
        sword: t('ios.a11y.attacking'),
        leadSword: t('ios.a11y.attackfirst'),
        check: t('ios.a11y.saidgood'),
    };

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
                const key = seatKey(game, index);
                const bubble = chatBubbles[key];
                const mark = markOf(game, index, player);

                return (
                    <div key={key} style={{
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
                        {/* THE SEAT'S ROLE ROW (FSeatBadge.roleRow): a CONSTANT box,
                            always present whether or not this seat wears a mark, so
                            the name and the mini hand below it have nothing to
                            re-lay-out when a mark arrives or leaves. Tall enough for
                            the largest glyph in the family, or it clips the sword's
                            corners (RoleMarkSize.rowHeight).

                            WHERE IT SITS is the one thing not taken from iMessage:
                            FSeatBadge stacks name, mini fan, then the role row, and
                            this ring keeps the slot the website already had, above
                            the name. The seat box here is 80px with the mini hand
                            overflowing it, so moving the row under the fan is a
                            layout change with its own screens to check (the self
                            seat sits just above the action bar); the glyphs are the
                            spec, the stacking order is deliberately left alone.

                            TODO(ios-parity): iMessage board shifts the defender shield
                            the moment a pass is staged (before defender_move lands) -
                            that immediacy feels good; consider mirroring. And the
                            marks do not MOVE here yet: on iMessage a mark turns like a
                            coin when it changes and FLIES to the seat that takes it
                            over (ios/FoolishKit/Boards/FRoleMotion.swift). */}
                        <div style={{
                            height: `${RoleMarkSize.rowHeight}px`,
                            display: 'flex',
                            alignItems: 'center',
                            justifyContent: 'center'
                        }}>
                            {mark && <RoleMarkView kind={mark} label={markLabel[mark]} />}
                        </div>

                        <div style={{
                            margin: 0,
                            fontSize: '12px',
                            color: 'var(--color-text-primary)',
                            textShadow: 'var(--text-shadow-label)',
                            // One line, centred on the seat even when wider than it:
                            // a wrapped second line fell under the seat's mini hand.
                            whiteSpace: 'nowrap',
                            textAlign: 'center',
                            height: '30px',
                            display: 'flex',
                            alignItems: 'center',
                            justifyContent: 'center',
                            position: 'relative'
                        }}>
                            {botDisplayName(player.name)}

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
                            handKey={key}
                            selfHandLength={index === self_index ? game.myHand.length : undefined}
                            isSelf={index === self_index}
                        />
                    </div>
                );
            })}
        </>
    );
};
