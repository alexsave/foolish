import { useServer } from "../../contexts/ServerContext";
import { seatKey, type TableView, type ViewSeat } from "../../state/view";
import { useFernFractal } from "../../utils/fernFractal";
import { useStyles } from "../../contexts/StyleContext";
import { useLocalization } from "../../contexts/LocalizationContext";
import { useState, useEffect, useRef } from "react";
import { RoleMarkSize, type RoleMarkKind } from "../RoleMark";
import { RoleCoin } from "../RoleCoin";
import { useAnimation } from "../../contexts/AnimationContext";
import { markWorn, shownBoardOf } from "../../state/roleLedger";
import { SovietCardBack } from "./SovietCardBack";
import { botDisplayName } from "../../common/botName";
import { useHandShape } from "./handShape";

// Mini stacked card back — same SVG as the full CardBack, absolutely positioned
// to fill the stacked ring slot.
const MiniSovietCardBack = () => (
    <SovietCardBack style={{ position: 'absolute', top: 0, left: 0 }} />
);

/** THE SEAT BADGE'S THREE ROWS, top to bottom: the role coin, the name, the
 *  mini hand. Each row is exactly as tall as what it draws and none of them
 *  shrinks, and the seat box is their sum, so nothing overflows into a
 *  neighbour.
 *
 *  It used to be a fixed 80px box holding 40 + 30 + 20 = 90px of rows. Flexbox
 *  answered the overflow by shrinking every row, and the mini hand's row
 *  (declared 20px, squeezed to 16) held 35px cards centred on it, so they hung
 *  ~10px up into the name and covered the lower half of its letters. The squeeze
 *  also differed with the coin's face (an empty coin gave up 4.5px, a sword
 *  none), so a name moved when a mark arrived, which the coin's constant box
 *  exists to prevent. */
const MINI_CARD_WIDTH = 25;
const MINI_CARD_HEIGHT = MINI_CARD_WIDTH * 1.4;
/** One 12px line, with its descenders, and nothing to spare. */
const NAME_ROW_HEIGHT = 16;
export const SeatBadgeSize = {
    roleRow: RoleMarkSize.rowHeight,
    nameRow: NAME_ROW_HEIGHT,
    handRow: MINI_CARD_HEIGHT,
    seat: RoleMarkSize.rowHeight + NAME_ROW_HEIGHT + MINI_CARD_HEIGHT,
} as const;

const CardsVisual = ({ player, handKey, selfHandLength, isSelf }: { player: ViewSeat, handKey: string, selfHandLength?: number, isSelf: boolean }) => {
    const styles = useStyles();
    const { fernPattern } = useFernFractal();

    const hasPattern = styles.miniCard.usePattern && !!fernPattern;
    const cardWidth = MINI_CARD_WIDTH;
    const cardHeight = MINI_CARD_HEIGHT;
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
            height: `${SeatBadgeSize.handRow}px`,
            width: '100px',
            flexShrink: 0,
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
                    top: 0,
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

/** THE ONE MARK a seat wears is src/state/roleLedger.ts's `markWorn`, and it is
 *  asked of the LEDGER, not of the board.
 *
 *  NEVER TWO, and that is why the ranking is one function rather than two
 *  components each deciding. The website used to draw the sword in this ring and
 *  the shield in a component of its own, so nothing stopped one seat wearing
 *  both - and on a finished game it did: e2e/fixtures/ui_dom/replay_named_end
 *  recorded Ada with the sword in her own slot and the shield floating beside
 *  it, because at game over the kernel's two badges can name the same seat.
 *  iMessage has the rule as a single function (`RoleMarkKind.worn`), which is a
 *  HOST function there too.
 *
 *  WHY THE LEDGER AND NOT `rulesOf(game)`. `first_attacker_badge` answers "who
 *  leads the NEXT bout" and goes to -1 the moment the table opens, so this ring
 *  drew NO sword at all for the whole of every open bout - measured in a real
 *  browser over a four-move bout, 32 frames of sword out of 648. The rule
 *  iMessage draws the sword by is `showsSword` (MessageTableView+Roles.swift),
 *  which is a host rule there and is NOT the kernel's `turn_may_act`: it keeps
 *  an attacker's sword until THEY say good, and it asks the roles the board is
 *  SHOWING rather than the ones the kernel has moved on to. Neither of those is
 *  a fact about a board, which is why the answer is not a field of one.
 *
 *  A seat that is not in the ledger yet (a first paint) wears whatever the live
 *  board says, so the very first frame is not blank. */
export const PlayerRing = () => {
    const { t } = useLocalization();
    const game = useServer().view as TableView;
    const { chatMessages } = useServer();
    const styles = useStyles();
    const self_index = game.mySeat;
    const { lift } = useHandShape();
    // The marks a seat can wear, named the way FoolishKit names them.
    const markLabel: Record<RoleMarkKind, string> = {
        shield: t('ios.a11y.defending'),
        sword: t('ios.a11y.attacking'),
        leadSword: t('ios.a11y.attackfirst'),
        check: t('ios.a11y.saidgood'),
    };

    // The marks lag the board: what they are WEARING is the ledger's, walked
    // forward beat by beat by the sequence that earns each change
    // (src/state/useRoleMotion.ts). `shownBoardOf(game)` is the seed for a screen
    // whose ledger has not been written yet.
    const { shownRoles, roleHandOff, publishRolePad } = useAnimation();
    const shownBoard = shownRoles ?? shownBoardOf(game);

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
                const mark = markWorn(shownBoard, index, player);

                return (
                    <div key={key} style={{
                        position: 'absolute',
                        top: y,
                        left: x,
                        display: 'flex',
                        flexDirection: 'column',
                        alignItems: 'center',
                        width: '80px',
                        height: `${SeatBadgeSize.seat}px`,
                        // My seat sits just above my hand, so it rises with
                        // the hand's second row (handShape.tsx lift).
                        transform: visual_index === 0 && lift > 0
                            ? `translate(-50%, calc(-50% - ${lift}px))` : 'translate(-50%, -50%)'
                    }}>
                        {/* THE SEAT'S ROLE ROW (FSeatBadge.roleRow), which is a
                            COIN: it turns when the mark changes and it blanks
                            while its mark is in the air as a flight ghost
                            (src/components/RoleCoin.tsx). A CONSTANT box, always
                            present whether or not this seat wears a mark, so the
                            name and the mini hand below it have nothing to
                            re-lay-out when a mark arrives or leaves - and so this
                            seat always has a landing pad to publish, which is
                            what lets a mark fly to a seat that is wearing
                            nothing.

                            WHERE IT SITS is the one thing not taken from
                            iMessage: FSeatBadge stacks name, mini fan, then the
                            role row, and this ring keeps the slot the website
                            already had, above the name. The seat box here is 80px
                            with the mini hand overflowing it, so moving the row
                            under the fan is a layout change with its own screens
                            to check (the self seat sits just above the action
                            bar); the glyphs and the motion are the spec, the
                            stacking order is deliberately left alone.

                            TODO(ios-parity): iMessage shifts the defender shield
                            the moment a pass is STAGED (before defender_move
                            lands) - that immediacy feels good; here a pass I have
                            played flies its shield at the prediction's closing
                            beat instead.

                            THE WEB HAS NO STAGING - a Messages extension stages a
                            move into a bubble and sends it as a separate act, and
                            that split is the platform's, not a design. The web's
                            equivalent moment is the OPTIMISTIC SUBMIT, which is
                            where a good now flips its own check
                            (AnimationContext's `good`). A pass cannot follow it
                            there yet for a reason that is not about where the
                            call goes: `anim_goods_opening` needs only a mask this
                            client already knows (its own seat's bit), while
                            `anim_pass_hand_off` needs the FINAL board's defender,
                            and an optimistic step carries no final board. Who
                            defends next is the kernel's to say, so this waits on
                            asking it rather than on deriving it here. */}
                        <RoleCoin
                            seat={index}
                            kind={mark}
                            departing={roleHandOff.departing.has(index)}
                            arriving={roleHandOff.arriving.has(index)}
                            labelOf={(k) => markLabel[k]}
                            publishPad={publishRolePad}
                        />

                        <div style={{
                            margin: 0,
                            fontSize: '12px',
                            color: 'var(--color-text-primary)',
                            textShadow: 'var(--text-shadow-label)',
                            // One line, centred on the seat even when wider than it:
                            // a wrapped second line fell under the seat's mini hand.
                            whiteSpace: 'nowrap',
                            textAlign: 'center',
                            height: `${SeatBadgeSize.nameRow}px`,
                            lineHeight: `${SeatBadgeSize.nameRow}px`,
                            flexShrink: 0,
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
