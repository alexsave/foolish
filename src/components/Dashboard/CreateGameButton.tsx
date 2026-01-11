import { useMemo } from "react";
import { useNavigate } from "react-router-dom";
import supabase from "../../backend/Connector";
import { useWoodStyle } from "../WoodTexture";
import { Text } from "../Text";

export const CreateGameButton: React.FC = () => {
    const navigate = useNavigate();

    const woodBase2 = useWoodStyle(0.7);

    const woodButtonStyle = useMemo(() => ({
        ...woodBase2,
        border: '3px solid #5D3A1A',
        borderRadius: '0',
        fontWeight: 'bold' as const,
        cursor: 'pointer',
        transition: 'all 0.2s ease',
        boxShadow: `inset 0 1px 0 rgba(255,255,255,0.2), inset 0 -1px 0 rgba(0,0,0,0.3), 0 2px 4px rgba(0,0,0,0.4)`,
        position: 'relative' as const,
        overflow: 'hidden' as const,
    }), [woodBase2]);

    const woodButtonHoverStyle = useMemo(() => ({
        ...woodButtonStyle,
        filter: 'brightness(1.1) contrast(1.1)',
        transform: 'translateY(-1px)',
        boxShadow: `inset 0 2px 0 rgba(255,255,255,0.3), inset 0 -2px 0 rgba(0,0,0,0.4), 0 4px 8px rgba(0,0,0,0.5)`,
    }), [woodButtonStyle]);

    const handleCreate = () => {
        supabase.functions.invoke('create')
            .then(data => {
                navigate(`/${data.data.id}`);
            }).catch(error => {
                alert(error);
            });
    };

    return (
        <button
            onClick={handleCreate}
            style={{
                ...woodButtonStyle,
                padding: '8px 16px',
                fontSize: '14px',
                width: '100%'
            }}
            onMouseEnter={(e) => {
                Object.assign(e.currentTarget.style, woodButtonHoverStyle);
            }}
            onMouseLeave={(e) => {
                Object.assign(e.currentTarget.style, woodButtonStyle);
            }}
        >
            <span style={{ color: '#000' }}>
                <Text id="create_new_game" />
            </span>
        </button>
    );
};
