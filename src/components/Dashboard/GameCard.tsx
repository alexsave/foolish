import { useNavigate } from "react-router-dom";
import { useAuth } from "../../contexts/AuthContext";
import { SUIT_MAP } from "../../utils/cards";
import { getWoodTextureStyle } from "../WoodTexture";
import { Text } from "../Text";
import { PLAYER_STATUS, GAME_STATUS, PublicGame } from "../../common/types";

interface GameCardProps {
    game: PublicGame;
}

export const GameCard: React.FC<GameCardProps> = ({ game }) => {
    const navigate = useNavigate();
    const { username } = useAuth();

    const isGameOver = game.status === GAME_STATUS.GAME_OVER;
    const isWaiting = game.status === GAME_STATUS.WAITING;
    const isPlaying = game.status === GAME_STATUS.PLAYING;

    const readyPlayers = game.players.filter(p => p.status === PLAYER_STATUS.READY).length;
    const totalPlayers = game.players.length;

    // Generate wood texture based on game id
    const woodSeed = (game.id.charCodeAt(0) + game.id.charCodeAt(1)) / 200;
    const flip = (game.id.charCodeAt(3) || 0) % 2 === 0 ? 1 : -1;

    // Rustic status badge colors
    const getStatusStyle = () => {
        if (isWaiting) return { backgroundColor: '#B8860B', color: '#fff' }; // Dark goldenrod
        if (isPlaying) return { backgroundColor: '#2E5A1C', color: '#fff' }; // Dark forest green
        if (isGameOver) return { backgroundColor: '#5D3A1A', color: '#ccc' }; // Wood brown
        return { backgroundColor: '#8B4513', color: '#fff' }; // Saddle brown fallback
    };

    return (
        <div
            style={{
                border: '2px solid #5D3A1A',
                borderRadius: '0',
                boxShadow: `
                    inset 0 1px 0 rgba(255,255,255,0.2),
                    inset 0 -1px 0 rgba(0,0,0,0.3),
                    0 3px 6px rgba(0,0,0,0.4)`,
                position: 'relative',
                overflow: 'hidden',
                cursor: 'pointer',
                padding: '12px 16px',
                color: 'white',
                width: '100%',
                maxWidth: '95vw',
                minHeight: '90px',
                display: 'flex',
                flexDirection: 'column',
                justifyContent: 'space-between',
                boxSizing: 'border-box',
                transition: 'all 0.2s ease',
            }}
            onClick={() => navigate(`/${game.id}`)}
            onMouseEnter={(e) => {
                e.currentTarget.style.transform = 'translateY(-2px)';
                e.currentTarget.style.boxShadow = `
                    inset 0 1px 0 rgba(255,255,255,0.15),
                    inset 0 -1px 0 rgba(0,0,0,0.25),
                    0 5px 10px rgba(0,0,0,0.4)`;
            }}
            onMouseLeave={(e) => {
                e.currentTarget.style.transform = 'translateY(0)';
                e.currentTarget.style.boxShadow = `
                    inset 0 1px 0 rgba(255,255,255,0.2),
                    inset 0 -1px 0 rgba(0,0,0,0.3),
                    0 3px 6px rgba(0,0,0,0.4)`;
            }}
        >
            {/* Wood texture background */}
            <div style={{
                position: 'absolute',
                top: 0,
                left: 0,
                right: 0,
                bottom: 0,
                zIndex: -1,
                ...getWoodTextureStyle(woodSeed),
                transform: `scaleX(${flip})`,
                transformOrigin: 'center center'
            }} />

            {/* Header row: Game name + status badge */}
            <div style={{
                display: 'flex',
                justifyContent: 'space-between',
                alignItems: 'center',
                marginBottom: '4px'
            }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: '12px', flex: '1' }}>
                    <h3 style={{
                        margin: '0',
                        fontSize: '1.1rem',
                        fontWeight: 'bold',
                        textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
                    }}>
                        {game.name}
                    </h3>

                    {/* Game-specific info */}
                    {isWaiting && (
                        <span style={{
                            fontSize: '0.8rem',
                            textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
                        }}>
                            <Text id="ready" />: {readyPlayers}/{totalPlayers}
                        </span>
                    )}

                    {isPlaying && (
                        <span style={{
                            fontSize: '0.8rem',
                            textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
                        }}>
                            <Text id="deck_cards" />: {game.deck_length + (game.flipped ? 1 : 0)} {SUIT_MAP[game.power_suit]}
                        </span>
                    )}
                </div>

                {/* Status badge */}
                <span style={{
                    padding: '4px 10px',
                    borderRadius: '4px',
                    fontSize: '0.8rem',
                    fontWeight: 'bold',
                    border: '1px solid rgba(0,0,0,0.3)',
                    textShadow: '1px 1px 1px rgba(0,0,0,0.5)',
                    ...getStatusStyle()
                }}>
                    {isWaiting && <Text id="waiting" />}
                    {isPlaying && <Text id="playing" />}
                    {isGameOver && <Text id="game_over" />}
                </span>
            </div>

            {/* Players row */}
            <div style={{
                margin: '4px 0',
                fontSize: '0.85rem',
            }}>
                <div style={{ display: 'flex', flexWrap: 'wrap', gap: '8px', alignItems: 'center' }}>
                    {game.players.map((player, idx) => {
                        const isDefender = isPlaying && game.defender === idx;
                        const isFirstAttacker = isPlaying && game.first_attacker === idx;
                        const isCurrentUser = player.name === username;

                        return (
                            <span key={idx} style={{
                                display: 'flex',
                                alignItems: 'center',
                                gap: '4px',
                                textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
                            }}>
                                {player.is_ai ? '🤖' : '👤'}
                                <span style={{
                                    color: isCurrentUser ? '#4ADE80' : '#fff',
                                    fontWeight: isCurrentUser ? 'bold' : 'normal'
                                }}>
                                    {player.name}
                                </span>

                                {/* Status icons based on game state */}
                                {isWaiting && (
                                    <span style={{ fontSize: '0.8rem' }}>
                                        {player.status === PLAYER_STATUS.READY ? '🟢' : '🔴'}
                                    </span>
                                )}

                                {isPlaying && (
                                    <span style={{ display: 'flex', alignItems: 'center', gap: '2px' }}>
                                        {isDefender && <span title="Defender">🛡️</span>}
                                        {isFirstAttacker && <span title="First Attacker">⚔️</span>}
                                        {player.hand_length > 0 && <span style={{ fontSize: '0.75rem' }}> ({player.hand_length}) </span>}
                                    </span>
                                )}

                                {isGameOver && player.hand_length === 0 && (
                                    <span style={{ fontSize: '0.8rem' }} title="Winner">👑</span>
                                )}
                            </span>
                        );
                    })}
                </div>
            </div>
        </div>
    );
};
