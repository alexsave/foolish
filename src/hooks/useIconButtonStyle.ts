import { useMemo } from 'react';
import { useWoodStyle } from '../components/WoodTexture';

export const useIconButtonStyle = () => {
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

    const buttonBaseStyle = useMemo(() => ({
        position: 'absolute' as const,
        top: '10px',
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
    }), [woodButtonStyle]);

    const iconStyle = useMemo(() => ({
        color: '#000',
        fontSize: '28px',
        fontWeight: 900,
        lineHeight: 1,
    }), []);

    return {
        buttonBaseStyle,
        woodButtonStyle,
        woodButtonHoverStyle,
        iconStyle,
    };
};

