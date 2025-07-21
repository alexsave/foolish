import { useState } from "react";
import supabase from '../backend/Connector';
import { useServer } from "../contexts/ServerContext";
import { useNavigate } from "react-router-dom";
import { useAuth } from "../contexts/AuthContext";

export const Dashboard = () => {
    const [gameId, setGameId] = useState<string>('');
    const { username } = useAuth();
    const { joinGame, games } = useServer();
    const navigate = useNavigate();

    return <div style={{
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        height: '100%',
        width: '100%',
        padding: '0.25rem',
        boxSizing: 'border-box'
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
                    justifyContent: 'center'
                }}
            >
                <input 
                    type="text" 
                    value={gameId} 
                    onChange={(e) => setGameId(e.target.value)}
                    placeholder="Enter existing game ID"
                    inputMode="text"
                    style={{
                        padding: '6px 10px',
                        fontSize: '16px',
                        borderRadius: '4px',
                        border: '1px solid white',
                        backgroundColor: 'rgba(255, 255, 255, 0.1)',
                        color: 'white',
                        width: '150px',
                        textAlign: 'center'
                    }}
                />
                <button 
                    type="submit"
                    disabled={!gameId.trim()}
                    style={{
                        padding: '6px 12px',
                        backgroundColor: gameId.trim() ? '#2196F3' : 'rgba(33, 150, 243, 0.5)',
                        color: 'white',
                        border: 'none',
                        borderRadius: '4px',
                        cursor: gameId.trim() ? 'pointer' : 'not-allowed',
                        fontSize: '14px',
                        fontWeight: 'bold'
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
                    padding: '6px 16px',
                    backgroundColor: '#4CAF50',
                    color: 'white',
                    border: 'none',
                    borderRadius: '4px',
                    cursor: 'pointer',
                    fontSize: '14px',
                    fontWeight: 'bold',
                    width: '240px'
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
                            border: isGameOver ? '1px solid rgba(255, 255, 255, 0.3)' : '1px solid white',
                            borderRadius: '6px',
                            cursor: 'pointer',
                            padding: '8px 12px',
                            backgroundColor: isGameOver ? 'rgba(255, 255, 255, 0.02)' : 'rgba(255, 255, 255, 0.05)',
                            color: isGameOver ? 'rgba(255, 255, 255, 0.6)' : 'white',
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
                                e.currentTarget.style.backgroundColor = 'rgba(255, 255, 255, 0.1)';
                            }
                        }}
                        onMouseLeave={(e) => {
                            e.currentTarget.style.backgroundColor = isGameOver ? 'rgba(255, 255, 255, 0.02)' : 'rgba(255, 255, 255, 0.05)';
                        }}
                        >
                            <div style={{ 
                                                            display: 'flex', 
                            justifyContent: 'space-between', 
                            alignItems: 'center',
                            marginBottom: '4px' 
                            }}>
                                <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
                                    <h3 style={{ 
                                        margin: '0', 
                                        fontSize: '1.1rem', 
                                        fontWeight: 'bold' 
                                    }}>
                                        {game.name}
                                    </h3>

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
                            
                            {/* Game-specific info */}
                            {isWaiting && (
                                <p style={{ 
                                    margin: '4px 0 0 0', 
                                    fontSize: '0.8rem', 
                                    opacity: '0.6' 
                                }}>
                                    Ready: {readyPlayers}/{totalPlayers} players
                                </p>
                            )}
                            
                            {isPlaying && games[game.id] && (
                                <p style={{ 
                                    margin: '4px 0 0 0', 
                                    fontSize: '0.8rem', 
                                    opacity: '0.6' 
                                }}>
                                    Cards left: {games[game.id].deck_length} • 
                                    Battles: {games[game.id].table_battles?.length || 0}
                                </p>
                            )}

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