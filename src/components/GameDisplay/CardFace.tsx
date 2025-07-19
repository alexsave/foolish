import { SUIT_MAP, VALUE_MAP } from "../../utils/cards";
import { Card } from "../../common/types";
import { useAnimation } from "../../contexts/AnimationContext";

export const CardFace = ({ card, onClick, style = {}, playerId, isAnimationOverlay = false, ...props }: { 
    card: Card, 
    onClick?: () => void, 
    style?: React.CSSProperties,
    playerId?: string,
    isAnimationOverlay?: boolean
} & React.HTMLAttributes<HTMLDivElement>) => {
    const { getCardAnimationState } = useAnimation();
    const animationState = getCardAnimationState(card, playerId);

    const defaultStyle: React.CSSProperties = {
        backgroundColor: 'white',
        width: '50px',
        height: '70px',
        borderRadius: '5px',
        border: '2px solid black',
        display: 'flex',
        justifyContent: 'center',
        alignItems: 'center',
        cursor: onClick ? 'pointer' : 'default',
        userSelect: 'none',
        pointerEvents: onClick ? 'auto' : 'none', // Don't block pointer events unless there's an onClick
        fontSize: '20px',
        transition: 'transform 0.2s ease-in-out, opacity 0.2s ease-in-out',
    }

    // Apply animation styles based on animation state
    const animationStyle: React.CSSProperties = {};
    
    if (animationState.isAnimating && !isAnimationOverlay) {
        // Hide the original card since the AnimationOverlay is showing the animated version
        // But don't hide cards that are being rendered inside the AnimationOverlay itself
        animationStyle.opacity = 0;
        animationStyle.pointerEvents = 'none';
    }

    return (
        <div onClick={onClick} style={{ ...defaultStyle, ...animationStyle, ...style }} {...props}>
            <p style={{
                pointerEvents: 'none',
                userSelect: 'none',
                textAlign: 'center',
                margin: '1px'
            }}>
                {VALUE_MAP[card.value]}
                <br />
                {SUIT_MAP[card.suit]}
            </p>
        </div>
    )
}
