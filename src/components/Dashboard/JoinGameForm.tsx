import { useState, useMemo } from "react";
import { useNavigate } from "react-router-dom";
import { useServer } from "../../contexts/ServerContext";
import { useWoodStyle } from "../WoodTexture";
import { Text } from "../Text";
import { useLocalization } from "../../contexts/LocalizationContext";

export const JoinGameForm: React.FC = () => {
    const [gameId, setGameId] = useState<string>('');
    const { joinGame } = useServer();
    const navigate = useNavigate();
    const { t } = useLocalization();

    const woodBase1 = useWoodStyle(0.1);
    const woodBase3 = useWoodStyle(0.3);

    const woodButtonStyle = useMemo(() => ({
        ...woodBase1,
        border: '3px solid #5D3A1A',
        borderRadius: '0',
        fontWeight: 'bold' as const,
        cursor: 'pointer',
        transition: 'all 0.2s ease',
        boxShadow: `inset 0 1px 0 rgba(255,255,255,0.2), inset 0 -1px 0 rgba(0,0,0,0.3), 0 2px 4px rgba(0,0,0,0.4)`,
        position: 'relative' as const,
        overflow: 'hidden' as const,
    }), [woodBase1]);

    const woodButtonHoverStyle = useMemo(() => ({
        ...woodButtonStyle,
        filter: 'brightness(1.1) contrast(1.1)',
        transform: 'translateY(-1px)',
        boxShadow: `inset 0 2px 0 rgba(255,255,255,0.3), inset 0 -2px 0 rgba(0,0,0,0.4), 0 4px 8px rgba(0,0,0,0.5)`,
    }), [woodButtonStyle]);

    const woodInputStyle = useMemo(() => ({
        ...woodBase3,
        border: '2px solid #5D3A1A',
        borderRadius: '0',
        color: '#ffffff',
        textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
        boxShadow: `inset 2px 2px 4px rgba(0,0,0,0.4), inset -1px -1px 2px rgba(255,255,255,0.2)`,
    }), [woodBase3]);

    const handleSubmit = (e: React.FormEvent) => {
        e.preventDefault();
        if (gameId.trim()) {
            joinGame(gameId.toLowerCase()).then(data => {
                navigate(`/${data.game_id}`);
            }).catch(error => {
                alert(error);
            });
        }
    };

    return (
        <form
            onSubmit={handleSubmit}
            style={{
                display: 'flex',
                alignItems: 'center',
                width: '100%',
                justifyContent: 'space-between'
            }}
        >
            <input
                type="text"
                value={gameId}
                onChange={(e) => setGameId(e.target.value)}
                placeholder={t('enter_game_id')}
                inputMode="text"
                style={{
                    ...woodInputStyle,
                    padding: '8px 12px',
                    fontSize: '16px',
                    textAlign: 'center',
                    width: '50%',
                }}
            />
            <button
                type="submit"
                disabled={!gameId.trim()}
                style={{
                    ...woodButtonStyle,
                    padding: '8px 16px',
                    fontSize: '14px',
                    width: '50%',
                    marginLeft: '0.5rem',
                    opacity: gameId.trim() ? 1 : 0.6,
                    cursor: gameId.trim() ? 'pointer' : 'not-allowed',
                }}
                onMouseEnter={(e) => {
                    if (gameId.trim()) {
                        Object.assign(e.currentTarget.style, woodButtonHoverStyle);
                    }
                }}
                onMouseLeave={(e) => {
                    if (gameId.trim()) {
                        Object.assign(e.currentTarget.style, woodButtonStyle);
                    }
                }}
            >
                <span style={{ color: '#000' }}>
                    <Text id="join" />
                </span>
            </button>
        </form>
    );
};
