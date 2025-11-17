import { useNavigate } from 'react-router-dom';
import { useMemo } from 'react';
import { useWoodStyle } from './WoodTexture';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';

export const Welcome = () => {
    const navigate = useNavigate();
    const woodStyleBase = useWoodStyle(0.5);

    const woodButtonStyle: React.CSSProperties = useMemo(() => ({
        ...woodStyleBase,
        border: '3px solid #5D3A1A',
        borderRadius: '0',
        fontWeight: 'bold',
        cursor: 'pointer',
        transition: 'all 0.2s ease',
        boxShadow: `
            inset 0 1px 0 rgba(255,255,255,0.2),
            inset 0 -1px 0 rgba(0,0,0,0.3),
            0 2px 4px rgba(0,0,0,0.4)`,
        position: 'relative' as const,
        overflow: 'hidden' as const,
        padding: '10px 20px',
        fontSize: '16px',
        mixBlendMode: 'normal' as const,
    }), [woodStyleBase]);

    const woodButtonHoverStyle: React.CSSProperties = useMemo(() => ({
        ...woodStyleBase,
        filter: 'brightness(1.1) contrast(1.1)',
        transform: 'translateY(-1px)',
        boxShadow: `
            inset 0 2px 0 rgba(255,255,255,0.3),
            inset 0 -2px 0 rgba(0,0,0,0.4),
            0 4px 8px rgba(0,0,0,0.5)`,
        mixBlendMode: 'normal' as const,
    }), [woodStyleBase]);

    return (
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', height: '100%', width: '100%' }}>
            <WoolBackgroundLayer />
            <p style={{ fontSize: '48px', fontWeight: 'bold', color: 'rgb(239, 151, 28)' }}>FOOLISH</p>
            <button 
                style={woodButtonStyle}
                onMouseEnter={(e) => {
                    Object.assign(e.currentTarget.style, woodButtonHoverStyle);
                }}
                onMouseLeave={(e) => {
                    Object.assign(e.currentTarget.style, woodButtonStyle);
                }}
                onClick={() => { navigate('/login'); }}
            >
                <span style={{
                    color: 'rgba(70, 35, 20, 0.8)',
                    mixBlendMode: 'color-burn',
                    filter: 'contrast(1.2) brightness(0.9) blur(.3px)',
                }}>Start</span>
            </button>
        </div>
    );
};