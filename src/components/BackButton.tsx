import { useNavigate } from 'react-router-dom';
import { useWoodStyle } from './WoodTexture';
import { useMemo } from 'react';

export const BackButton = () => {
    const navigate = useNavigate();

    // Get wood styles with seeds - memoized to prevent new object creation
    const woodButtonBaseStyle = useWoodStyle(0.2);
    const woodButtonStyle = useMemo(() => ({
        ...woodButtonBaseStyle,
        mixBlendMode: 'normal' as const,
    }), [woodButtonBaseStyle]);
    const woodButtonHoverStyle = useMemo(() => ({ 
        ...woodButtonBaseStyle, 
        filter: 'brightness(1.1) contrast(1.1)', 
        transform: 'translateY(-1px)',
        boxShadow: `inset 0 2px 0 rgba(255,255,255,0.3), inset 0 -2px 0 rgba(0,0,0,0.4), 0 4px 8px rgba(0,0,0,0.5)`,
        mixBlendMode: 'normal' as const,
    }), [woodButtonBaseStyle]);

    return (
        <button
            onClick={() => navigate('/dashboard')}
            style={{
                position: 'absolute',
                top: '10px',
                left: '10px',
                zIndex: 100,
                ...woodButtonStyle,
                width: '44px',
                height: '44px',
                padding: '0',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                border: '2px solid #5D3A1A',
                borderRadius: '0',
                cursor: 'pointer',
                transition: 'all 0.2s ease',
                boxShadow: `inset 0 1px 0 rgba(255,255,255,0.2), inset 0 -1px 0 rgba(0,0,0,0.3), 0 2px 4px rgba(0,0,0,0.4)`,
                overflow: 'hidden',
            }}
            onMouseEnter={(e) => {
                Object.assign(e.currentTarget.style, woodButtonHoverStyle);
            }}
            onMouseLeave={(e) => {
                Object.assign(e.currentTarget.style, woodButtonStyle);
            }}
        >
            <span style={{
                color: 'rgba(70, 35, 20, 0.8)',
                mixBlendMode: 'color-burn',
                filter: 'contrast(1.2) brightness(0.9)',
                fontSize: '28px',
                fontWeight: 900,
                lineHeight: 1,
            }}>{'<'}</span>
        </button>
    );
};

