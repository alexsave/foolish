import React, { useRef, useEffect, useState } from 'react';
import { Card } from '../../common/types';
import { CardBack } from './CardBack';
import { SUIT_MAP, VALUE_MAP } from '../../utils/cards';
import { HEARTS, DIAMONDS } from '../../common/constants';
import { useAnimation } from '../../contexts/AnimationContext';

export const CardFace = ({ card, onClick, style = {}, playerId, isAnimationOverlay = false, ...props }: {
    card: Card,
    onClick?: () => void,
    style?: React.CSSProperties,
    playerId?: string,
    isAnimationOverlay?: boolean
} & React.HTMLAttributes<HTMLDivElement>) => {
    const { getCardAnimationState } = useAnimation();
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

    // Get clean suit symbols (without emoji modifiers for better display)
    // Voodoo here
    const suitSymbol = SUIT_MAP[card.suit]?.replace('️', '') || '?';
    const valueSymbol = VALUE_MAP[card.value] || '?';

    // Determine if this is a small card that needs simplified layout based on ACTUAL rendered size
    const isThinCard = actualWidth < 40 && actualHeight > 60;

    const defaultStyle: React.CSSProperties = {
        backgroundColor: 'white',
        width: '50px',
        height: '70px',
        borderRadius: '5px',
        border: '2px solid black',
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

    if (isThinCard) {
        return <div
            ref={cardRef}
            onClick={onClick}
            style={{ ...defaultStyle, ...animationStyle, ...style }}
            {...props}>
            <div style={{ fontSize: '20px' }}>{valueSymbol}</div>
            <div style={{ fontSize: '24px' }}>{suitSymbol}</div>
        </div>
    }

    return (
        <div
            ref={cardRef}
            onClick={onClick}
            style={{ ...defaultStyle, ...animationStyle, ...style }}
            {...props}
        >
            {/* Top-left corner index */}
            <div style={{
                ...baseIndexStyle,
                left: '4px',
                top: '4px',
            }}>
                <div>{valueSymbol}</div>
                <div>{suitSymbol}</div>
            </div>

            {/* Bottom-right corner index (rotated) */}
            <div style={{
                ...baseIndexStyle,
                bottom: '4px',
                right: '4px',
                transform: 'rotate(180deg)',
            }}>
                <div>{valueSymbol}</div>
                <div>{suitSymbol}</div>
            </div>

            {/* Center suit symbol */}
            <div style={{
                position: 'absolute',
                top: '50%',
                left: '50%',
                transform: 'translate(-50%, -50%)',
                fontSize: '36px',
                pointerEvents: 'none',
                userSelect: 'none',
                WebkitUserSelect: 'none',
                WebkitTouchCallout: 'none',
                textAlign: 'center',
                lineHeight: '1',
            }}>
                <div>{suitSymbol}</div>
            </div>
        </div>
    );
};
