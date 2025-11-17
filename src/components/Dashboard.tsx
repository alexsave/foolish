import { useState } from "react";
import supabase from '../backend/Connector';
import { useServer } from "../contexts/ServerContext";
import { useNavigate } from "react-router-dom";
import { useAuth } from "../contexts/AuthContext";
import { SUIT_MAP } from "../utils/cards";
import { useWoodStyle, getWoodTextureStyle } from "./WoodTexture";

export const Dashboard = () => {
    const [gameId, setGameId] = useState<string>('');
    const { username } = useAuth();
    const { joinGame, games } = useServer();
    const navigate = useNavigate();
    
    // Get wood styles with different seeds for variety
    const woodButtonStyle = {
        ...useWoodStyle(0.1),
        border: '3px solid #5D3A1A',
        borderRadius: '0',
        color: '#ffffff',
        fontWeight: 'bold',
        cursor: 'pointer',
        transition: 'all 0.2s ease',
        textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
        boxShadow: `inset 0 1px 0 rgba(255,255,255,0.2), inset 0 -1px 0 rgba(0,0,0,0.3), 0 2px 4px rgba(0,0,0,0.4)`,
        position: 'relative' as const,
        overflow: 'hidden' as const,
    };
    const woodButtonHoverStyle = {
        ...woodButtonStyle,
        filter: 'brightness(1.1) contrast(1.1)',
        transform: 'translateY(-1px)',
        boxShadow: `inset 0 2px 0 rgba(255,255,255,0.3), inset 0 -2px 0 rgba(0,0,0,0.4), 0 4px 8px rgba(0,0,0,0.5)`
    };
    const woodButtonStyle2 = {
        ...useWoodStyle(0.7),
        border: '3px solid #5D3A1A',
        borderRadius: '0',
        color: '#ffffff',
        fontWeight: 'bold',
        cursor: 'pointer',
        transition: 'all 0.2s ease',
        textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
        boxShadow: `inset 0 1px 0 rgba(255,255,255,0.2), inset 0 -1px 0 rgba(0,0,0,0.3), 0 2px 4px rgba(0,0,0,0.4)`,
        position: 'relative' as const,
        overflow: 'hidden' as const,
    };
    const woodButtonHoverStyle2 = {
        ...woodButtonStyle2,
        filter: 'brightness(1.1) contrast(1.1)',
        transform: 'translateY(-1px)',
        boxShadow: `inset 0 2px 0 rgba(255,255,255,0.3), inset 0 -2px 0 rgba(0,0,0,0.4), 0 4px 8px rgba(0,0,0,0.5)`
    };
    const woodInputStyle = {
        ...useWoodStyle(0.3),
        border: '2px solid #5D3A1A',
        borderRadius: '0',
        color: '#ffffff',
        textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
        boxShadow: `inset 2px 2px 4px rgba(0,0,0,0.4), inset -1px -1px 2px rgba(255,255,255,0.2)`,
    };

    return <div style={{
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        minHeight: '100vh',
        width: '100%',
        padding: '0.25rem',
        boxSizing: 'border-box',
        overflowY: 'auto',
        overflowX: 'hidden'
    }}>
        <h1 style={{ 
            color: 'white', 
            fontSize: '1.3rem', 
            fontWeight: 'bold', 
            marginBottom: '0.75rem',
            textAlign: 'center'
        }}>
            {username}'s Dashboard
        </h1>
        
        <div style={{ 
            display: 'flex', 
            flexDirection: 'column', 
            gap: '0.5rem', 
            alignItems: 'center',
            marginBottom: '0.75rem',
            width: '300px'
        }}>
            {/* Join Game Row */}
            <form 
                onSubmit={(e) => {
                    e.preventDefault();
                    if (gameId.trim()) {
                        joinGame(gameId.toLowerCase()).then(data => {
                            console.log(data);
                            navigate(`/${data.game_id}`);
                        }).catch(error => {
                            alert(error);
                        });
                    }
                }}
                style={{
                    display: 'flex',
                    alignItems: 'center',
                    //gap: '0.5rem',
                    width: '100%',
                    justifyContent: 'space-between'
                }}
            >
                <input 
                    type="text" 
                    value={gameId} 
                    onChange={(e) => setGameId(e.target.value)}
                    placeholder="Enter existing game ID"
                    inputMode="text"
                    style={{
                        ...woodInputStyle,
                        padding: '8px 12px',
                        fontSize: '16px',
                        textAlign: 'center',
                        flex: 1,
                    }}
                />
                <button 
                    type="submit"
                    disabled={!gameId.trim()}
                    style={{
                        ...woodButtonStyle,
                        padding: '8px 16px',
                        fontSize: '14px',
                        flex: '1',
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
                    Join
                </button>
            </form>
            
            {/* Create Game Button */}
            <button 
                onClick={() => {
                    // Just call it directly for now, i dont care
                    supabase.functions.invoke('create')
                        .then(data => {
                            console.log(data);
                            navigate(`/${data.data.id}`);
                        }).catch(error => {
                            alert(error);
                        });
                }}
                style={{
                    ...woodButtonStyle2, // Use secondary texture for variety
                    padding: '8px 16px',
                    fontSize: '14px',
                    flex: '1',
                    width: '100%'
                }}
                onMouseEnter={(e) => {
                    Object.assign(e.currentTarget.style, woodButtonHoverStyle2);
                }}
                onMouseLeave={(e) => {
                    Object.assign(e.currentTarget.style, woodButtonStyle2);
                }}
            >
                Create New Game
            </button>
        </div>

        <div style={{ width: '100%', maxWidth: '95vw', flex: '1' }}>
            
            <div style={{ 
                display: 'flex', 
                flexDirection: 'column', 
                gap: '6px', 
                alignItems: 'center' 
            }}>
                {Object.values(games).map((game) => {
                    const isGameOver = game.status === 'game_over';
                    const isWaiting = game.status === 'waiting';
                    const isPlaying = game.status === 'playing';
                    
                    // Find current user in the game
                    const currentUser = game.players.find(p => p.name === username);
                    
                    // Calculate readiness for waiting games
                    const readyPlayers = game.players.filter(p => p.status === 'ready').length;
                    const totalPlayers = game.players.length;
                    
                    return (
                        <div 
                            key={game.id} 
                            style={{ 
                            border: '2px solid #5D3A1A',
                            borderRadius: '0',
                            boxShadow: `
                                inset 0 1px 0 rgba(255,255,255,0.2),
                                inset 0 -1px 0 rgba(0,0,0,0.3),
                                0 3px 6px rgba(0,0,0,0.4)`,
                            position: 'relative' as const,
                            overflow: 'hidden' as const,
                            cursor: 'pointer',
                            padding: '12px 16px',
                            color: isGameOver ? 'rgba(255, 255, 255, 0.7)' : 'white',
                            width: '100%',
                            maxWidth: '95vw',
                            height: '90px',
                            display: 'flex',
                            flexDirection: 'column',
                            justifyContent: 'space-between',
                            boxSizing: 'border-box',
                            transition: 'all 0.2s ease',
                            opacity: isGameOver ? 0.7 : 1
                        }} 
                            onClick={() => {
                                navigate(`/${game.id}`);
                            }}
                            onMouseEnter={(e) => {
                            if (!isGameOver) {
                                e.currentTarget.style.transform = 'translateY(-2px)';
                                e.currentTarget.style.boxShadow = `
                                    inset 0 1px 0 rgba(255,255,255,0.15),
                                    inset 0 -1px 0 rgba(0,0,0,0.25),
                                    0 5px 10px rgba(0,0,0,0.4)`;
                            }
                        }}
                        onMouseLeave={(e) => {
                            if (!isGameOver) {
                                e.currentTarget.style.transform = 'translateY(0)';
                                e.currentTarget.style.boxShadow = `
                                    inset 0 1px 0 rgba(255,255,255,0.1),
                                    inset 0 -1px 0 rgba(0,0,0,0.2),
                                    0 3px 6px rgba(0,0,0,0.3)`;
                            }
                        }}
                        >
                            {/* Wood texture background layer - can be transformed independently */}
                            <div style={{
                                position: 'absolute',
                                top: 0,
                                left: 0,
                                right: 0,
                                bottom: 0,
                                zIndex: -1,
                                ...getWoodTextureStyle((game.id.charCodeAt(0) + game.id.charCodeAt(1)) / 200),
                                transform: `scaleX(${(game.id.charCodeAt(3) || 0) % 2 === 0 ? 1 : -1})`,
                                transformOrigin: 'center center'
                            }} />
                            
                                                        <div style={{ 
                                display: 'flex', 
                                justifyContent: 'space-between', 
                                alignItems: 'center',
                                marginBottom: '4px' 
                            }}>
                                <div style={{ display: 'flex', alignItems: 'center', gap: '8px', flex: '1' }}>
                                    <h3 style={{ 
                                        margin: '0', 
                                        fontSize: '1.1rem', 
                                        fontWeight: 'bold' 
                                    }}>
                                        {game.name}
                                    </h3>
                                    
                                    {/* Game-specific info inline */}
                                    {isWaiting && (
                                        <span style={{ 
                                            fontSize: '0.75rem', 
                                            opacity: '0.6',
                                            marginLeft: '8px'
                                        }}>
                                            Ready: {readyPlayers}/{totalPlayers}
                                        </span>
                                    )}
                                    
                                    {isPlaying && games[game.id] && (
                                        <span style={{ 
                                            fontSize: '0.75rem', 
                                            opacity: '0.6',
                                            marginLeft: '8px'
                                        }}>
                                            Deck cards: {games[game.id].deck_length + (games[game.id].flipped ? 1 : 0)} {SUIT_MAP[games[game.id].power_suit]}
                                        </span>
                                    )}
                                </div>
                                <span style={{ 
                                    padding: '4px 8px', 
                                    borderRadius: '12px', 
                                    fontSize: '0.8rem',
                                    backgroundColor: isWaiting ? '#FFC107' : 
                                                   isPlaying ? '#4CAF50' : 
                                                   isGameOver ? '#757575' : '#f44336',
                                    color: 'white'
                                }}>
                                    {game.status.replace('_', ' ')}
                                </span>
                            </div>
                            
                            {/* Players with status indicators */}
                            <div style={{ 
                                margin: '4px 0', 
                                fontSize: '0.85rem', 
                                opacity: '0.8' 
                            }}>
                                <div style={{ display: 'flex', flexWrap: 'wrap', gap: '8px', alignItems: 'center' }}>
                                    {game.players.map((player, idx) => {
                                        const isDefender = isPlaying && game.defender === idx;
                                        const isFirstAttacker = isPlaying && game.first_attacker === idx;
                                        
                                        return (
                                            <span key={idx} style={{ display: 'flex', alignItems: 'center', gap: '4px' }}>
                                                {player.is_ai ? '🤖' : '👤'}
                                                <span style={{ 
                                                    color: player.name === username ? '#4CAF50' : 'inherit',
                                                    fontWeight: player.name === username ? 'bold' : 'normal'
                                                }}>
                                                    {player.name}
                                                </span>
                                                
                                                {/* Status icons based on game state */}
                                                {isWaiting && (
                                                    <span style={{ fontSize: '0.8rem' }}>
                                                        {player.status === 'ready' ? '🟢' : '🔴'}
                                                    </span>
                                                )}
                                                
                                                {isPlaying && (
                                                    <span style={{ display: 'flex', alignItems: 'center', gap: '2px' }}>
                                                        {isDefender && <span title="Defender">🛡️</span>}
                                                        {isFirstAttacker && <span title="First Attacker">⚔️</span>}
                                                        {player.hand_length > 0 && (
                                                            <span style={{ fontSize: '0.7rem', opacity: '0.6' }}>
                                                                ({player.hand_length})
                                                            </span>
                                                        )}
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
                })}
                
                {Object.keys(games).length === 0 && (
                    <div style={{
                        color: 'white',
                        opacity: '0.6',
                        textAlign: 'center',
                        padding: '2rem',
                        fontSize: '1.1rem'
                    }}>
                        No games available. Create a new game to get started!
                    </div>
                )}
            </div>
        </div>
    </div>;
};