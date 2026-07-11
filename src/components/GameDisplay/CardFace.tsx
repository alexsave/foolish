import React, { useRef, useEffect, useState } from 'react';
import { Card } from '@shared/types.ts';
import { CardBack } from './CardBack';
import { VALUE_MAP } from '../../utils/cards';
import { HEARTS, DIAMONDS } from '@shared/constants.ts';
import { useAnimation } from '../../contexts/AnimationContext';
import { useStyles } from '../../contexts/StyleContext';
import { SuitIcon } from '../SovietIcon';

export const CardFace = ({ card, onClick, style = {}, playerId, isAnimationOverlay = false, ...props }: {
    card: Card,
    onClick?: () => void,
    style?: React.CSSProperties,
    playerId?: string,
    isAnimationOverlay?: boolean
} & React.HTMLAttributes<HTMLDivElement>) => {
    const { getCardAnimationState } = useAnimation();
    const styles = useStyles();
    const cardRef = useRef<HTMLDivElement>(null);
    const [actualWidth, setActualWidth] = useState<number>(50);
    const [actualHeight, setActualHeight] = useState<number>(70);

    // Measure actual rendered dimensions using ResizeObserver
    // This ensures cards update when they're squished/unsquished due to layout changes
    useEffect(() => {
        if (!cardRef.current) return;

        const resizeObserver = new ResizeObserver((entries) => {
            for (const entry of entries) {
                const { width, height } = entry.contentRect;
                setActualWidth(width);
                setActualHeight(height);
            }
        });

        resizeObserver.observe(cardRef.current);

        return () => {
            resizeObserver.disconnect();
        };
    }, []); // Empty dependency array - ResizeObserver handles all size changes

    // Defense in depth: never crash the whole Game Page on a missing card. A
    // null/undefined slot should be impossible now that the hand reorder is
    // bounds-safe (see reorderHand / DragContext), but if one ever reaches here
    // — a sparse-array hole, a stale render — degrade to a face-down instead of
    // dereferencing `card.suit` on undefined (prod: "undefined is not an object
    // (evaluating 'e.suit')").
    if (!card) {
        return <CardBack deckSize={1} />;
    }

    // Check if this is a sanitized card (used for other players' cards in animations)
    const isSanitizedCard = card.suit === -1 && card.value === -1;

    // If it's a sanitized card, render a card back instead
    if (isSanitizedCard) {
        return <CardBack deckSize={1} />;
    }

    const animationState = getCardAnimationState(card, playerId);

    // Determine if suit is red (hearts/diamonds) or black (spades/clubs)
    const isRed = card.suit === HEARTS || card.suit === DIAMONDS; // hearts or diamonds
    const suitColor = isRed ? '#dc2626' : '#000000'; // red or black

    const valueSymbol = VALUE_MAP[card.value] || '?';
    
    // Render suit symbol - SVG icon for Soviet theme, emoji for default
    const suitEmoji = ['♠', '♥', '♣', '♦'][card.suit] || '?';
    const renderSuit = (size: number) => {
        if (!styles.icons.useEmojiIcons) {
            return <SuitIcon suit={card.suit} size={size} />;
        }
        return <span style={{ fontSize: size }}>{suitEmoji}</span>;
    };

    // Determine if this is a small card that needs simplified layout based on ACTUAL rendered size
    const isThinCard = actualWidth < 40 && actualHeight > 60;

    const defaultStyle: React.CSSProperties = {
        backgroundColor: 'var(--color-card-face)',
        width: '50px',
        height: '70px',
        borderRadius: styles.cardInHand.borderRadius,
        border: `2px solid ${styles.cardInHand.borderColor}`,
        boxShadow: styles.cardInHand.boxShadow,
        cursor: onClick ? 'pointer' : 'default',
        userSelect: 'none',
        WebkitUserSelect: 'none',
        WebkitTouchCallout: 'none',
        pointerEvents: onClick ? 'auto' : 'none',
        transition: 'transform 0.2s ease-in-out, opacity 0.2s ease-in-out, box-shadow 0.2s ease-in-out',
        position: 'relative',
        fontFamily: 'Georgia, serif',
        fontWeight: 'bold',
        color: suitColor,
        display: 'flex',
        flexDirection: 'column',
        fontSize: '20px',
        lineHeight: '15px',
    } as React.CSSProperties

    // Apply animation styles based on animation state
    const animationStyle: React.CSSProperties = {};

    if (animationState.isAnimating && !isAnimationOverlay) {
        // Hide the original card since the AnimationOverlay is showing the animated version
        // But don't hide cards that are being rendered inside the AnimationOverlay itself
        animationStyle.opacity = 0;
        animationStyle.pointerEvents = 'none';
    }

    // Base style for card indexes
    const baseIndexStyle: React.CSSProperties = {
        width: '12px',
        position: 'absolute',
        fontSize: '20px',
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        justifyContent: 'center',
        textAlign: 'center',
        userSelect: 'none',
        WebkitUserSelect: 'none',
        WebkitTouchCallout: 'none',
    } as React.CSSProperties;

    const mergedStyle = { ...defaultStyle, ...animationStyle, ...style };

    if (isThinCard) {
        return <div
            ref={cardRef}
            onClick={onClick}
            style={mergedStyle}
            {...props}>
            <div style={{ fontSize: '20px' }}>{valueSymbol}</div>
            <div>{renderSuit(20)}</div>
        </div>
    }

    return (
        <div
            ref={cardRef}
            onClick={onClick}
            style={mergedStyle}
            {...props}
        >
            {/* Top-left corner index */}
            <div style={{
                ...baseIndexStyle,
                left: '4px',
                top: '4px',
            }}>
                <div>{valueSymbol}</div>
                <div>{renderSuit(14)}</div>
            </div>

            {/* Bottom-right corner index (rotated) */}
            <div style={{
                ...baseIndexStyle,
                bottom: '4px',
                right: '4px',
                transform: 'rotate(180deg)',
            }}>
                <div>{valueSymbol}</div>
                <div>{renderSuit(14)}</div>
            </div>

            {/* Center suit symbol */}
            <div style={{
                position: 'absolute',
                top: '50%',
                left: '50%',
                transform: 'translate(-50%, -50%)',
                pointerEvents: 'none',
                userSelect: 'none',
                WebkitUserSelect: 'none',
                WebkitTouchCallout: 'none',
                textAlign: 'center',
                lineHeight: '1',
            }}>
                <div>{renderSuit(32)}</div>
            </div>
        </div>
    );
};
