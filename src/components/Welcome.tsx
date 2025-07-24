import { useNavigate } from 'react-router-dom';
import { getWoodTextureStyle } from './WoodTexture';

export const Welcome = () => {
    const navigate = useNavigate();

    const woodButtonStyle: React.CSSProperties = {
        ...getWoodTextureStyle(0.5), // Random position seed 0.5
        border: '3px solid #5D3A1A', // Darker wood border color
        borderRadius: '0', // Sharp 90-degree corners
        color: '#ffffff',
        fontWeight: 'bold',
        cursor: 'pointer',
        transition: 'all 0.2s ease',
        textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
        boxShadow: `
            inset 0 1px 0 rgba(255,255,255,0.2),
            inset 0 -1px 0 rgba(0,0,0,0.3),
            0 2px 4px rgba(0,0,0,0.4)`,
        position: 'relative' as const,
        overflow: 'hidden' as const,
        padding: '10px 20px',
        fontSize: '16px',
    };

    const woodButtonHoverStyle: React.CSSProperties = {
        ...getWoodTextureStyle(0.5), // Match button seed
        filter: 'brightness(1.1) contrast(1.1)',
        transform: 'translateY(-1px)',
        boxShadow: `
            inset 0 2px 0 rgba(255,255,255,0.3),
            inset 0 -2px 0 rgba(0,0,0,0.4),
            0 4px 8px rgba(0,0,0,0.5)`,
    };

    return (
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', height: '100%', width: '100%' }}>

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
                Start
            </button>
        </div>
    );
};