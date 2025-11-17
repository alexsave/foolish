import { useNavigate } from "react-router-dom";
import { useWoodStyle } from "./WoodTexture";

export const Tutorial = () => { 
    const navigate = useNavigate();
    const woodStyle = useWoodStyle(0.3);

    const woodButtonStyle: React.CSSProperties = {
        ...woodStyle, // Random position seed 0.3
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
        ...woodStyle, // Match button seed
        filter: 'brightness(1.1) contrast(1.1)',
        transform: 'translateY(-1px)',
        boxShadow: `
            inset 0 2px 0 rgba(255,255,255,0.3),
            inset 0 -2px 0 rgba(0,0,0,0.4),
            0 4px 8px rgba(0,0,0,0.5)`,
    };

    return <div>
        <h1>Tutorial</h1>
        <p>Insert a cool tutorial here with a sample game and everything</p>
        <button 
            style={woodButtonStyle}
            onMouseEnter={(e) => {
                Object.assign(e.currentTarget.style, woodButtonHoverStyle);
            }}
            onMouseLeave={(e) => {
                Object.assign(e.currentTarget.style, woodButtonStyle);
            }}
            onClick={() => navigate('/dashboard')}
        >
            Go to Game
        </button>
    </div>;
};