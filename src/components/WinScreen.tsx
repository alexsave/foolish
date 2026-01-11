import React, { useEffect, useState, useMemo } from 'react';
import { useServer } from '../contexts/ServerContext';
import { useAuth } from '../contexts/AuthContext';
import { GAME_STATUS } from '../common/types';
import supabase from '../backend/Connector';
import { useWoodStyle, getWoodTextureStyle } from './WoodTexture';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { Text } from './Text';

interface PlayerResult {
    player_id: string;
    name: string;
    rank: number;
    old_elo: number;
    new_elo: number;
    elo_change: number;
    is_ai: boolean;
}

export const WinScreen: React.FC = () => {
    const { game, continueGame } = useServer();
    const { user_id } = useAuth();
    const [playerResults, setPlayerResults] = useState<Map<string, PlayerResult>>(new Map());
    const [loading, setLoading] = useState(true);

    // Wood styles
    const woodButtonBase = useWoodStyle(0.6);
    const woodButtonStyle = useMemo(() => ({
        ...woodButtonBase,
        border: '3px solid #5D3A1A',
        borderRadius: '0',
        fontWeight: 'bold' as const,
        cursor: 'pointer',
        transition: 'all 0.2s ease',
        boxShadow: `inset 0 1px 0 rgba(255,255,255,0.2), inset 0 -1px 0 rgba(0,0,0,0.3), 0 2px 4px rgba(0,0,0,0.4)`,
        position: 'relative' as const,
        overflow: 'hidden' as const,
        padding: '12px 32px',
        fontSize: '16px',
    }), [woodButtonBase]);

    const woodButtonHoverStyle = useMemo(() => ({
        ...woodButtonStyle,
        filter: 'brightness(1.1) contrast(1.1)',
        transform: 'translateY(-1px)',
        boxShadow: `inset 0 2px 0 rgba(255,255,255,0.3), inset 0 -2px 0 rgba(0,0,0,0.4), 0 4px 8px rgba(0,0,0,0.5)`,
    }), [woodButtonStyle]);

    useEffect(() => {
        if (!game || game.status !== GAME_STATUS.GAME_OVER) {
            return;
        }

        const loadEloData = async () => {
            try {
                // Separate player IDs by type
                const userIds = game.players.filter(p => !p.is_ai).map(p => p.player_id);
                const botIds = game.players.filter(p => p.is_ai).map(p => p.player_id);
                
                // Make bulk calls for ELO data
                const [userEloData, botEloData] = await Promise.all([
                    userIds.length > 0 ? supabase
                        .from('user_elo_ratings')
                        .select('user_id, elo_rating, previous_elo')
                        .in('user_id', userIds) : Promise.resolve({ data: [] }),
                    botIds.length > 0 ? supabase
                        .from('bots')
                        .select('id, elo_rating, previous_elo')
                        .in('id', botIds) : Promise.resolve({ data: [] })
                ]);
                
                // Create lookup maps
                const userEloMap = new Map();
                const botEloMap = new Map();
                
                if (userEloData.data) {
                    userEloData.data.forEach(user => {
                        userEloMap.set(user.user_id, {
                            elo_rating: user.elo_rating,
                            previous_elo: user.previous_elo
                        });
                    });
                }
                
                if (botEloData.data) {
                    botEloData.data.forEach(bot => {
                        botEloMap.set(bot.id, {
                            elo_rating: bot.elo_rating,
                            previous_elo: bot.previous_elo
                        });
                    });
                }
                
                // Calculate player results from elimination order
                const results = new Map<string, PlayerResult>();
                
                // Winners in order (elimination_order[0] = 1st place, etc.)
                // Deduplicate elimination_order to handle backend bugs
                const uniqueEliminationOrder = Array.from(new Set(game.elimination_order));
                for (let index = 0; index < uniqueEliminationOrder.length; index++) {
                    const playerId = uniqueEliminationOrder[index];
                    const player = game.players.find(p => p.player_id === playerId);
                    if (player) {
                        const eloData = player.is_ai 
                            ? botEloMap.get(playerId) || { elo_rating: 0, previous_elo: 0 }
                            : userEloMap.get(playerId) || { elo_rating: 0, previous_elo: 0 };

                        const playerRank = index + 1;
                        
                        results.set(playerId, {
                            player_id: playerId,
                            name: player.name,
                            rank: playerRank,
                            old_elo: eloData.previous_elo,
                            new_elo: eloData.elo_rating,
                            elo_change: eloData.elo_rating - eloData.previous_elo,
                            is_ai: player.is_ai
                        });
                    }
                }

                // Add the fool (last place) - only if not already in results
                const fool = game.players.find(p => !uniqueEliminationOrder.includes(p.player_id));
                
                if (fool && !results.has(fool.player_id)) {
                    const foolRank = game.players.length;
                    
                    const eloData = fool.is_ai 
                        ? botEloMap.get(fool.player_id) || { elo_rating: 0, previous_elo: 0 }
                        : userEloMap.get(fool.player_id) || { elo_rating: 0, previous_elo: 0 };

                    results.set(fool.player_id, {
                        player_id: fool.player_id,
                        name: fool.name,
                        rank: foolRank,
                        old_elo: eloData.previous_elo,
                        new_elo: eloData.elo_rating,
                        elo_change: eloData.elo_rating - eloData.previous_elo,
                        is_ai: fool.is_ai
                    });
                }

                setPlayerResults(results);
                setLoading(false);
            } catch (error) {
                console.error('Error loading ELO data:', error);
                setLoading(false);
            }
        };

        loadEloData();
    }, [game]);

    if (!game || game.status !== GAME_STATUS.GAME_OVER) {
        return <div style={{
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            minHeight: '100vh',
            backgroundColor: '#ad826e',
            color: 'white'
        }}><Text id="loading" /></div>;
    }

    if (loading) {
        return <div style={{
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            minHeight: '100vh',
            backgroundColor: '#ad826e',
            color: 'white'
        }}><Text id="loading" /></div>;
    }

    const handleContinue = async () => {
        try {
            await continueGame(game.id);
        } catch (error) {
            console.error('Error continuing game:', error);
        }
    };

    const getRankEmoji = (rank: number, totalPlayers: number) => {
        if (rank === totalPlayers) return '🃏'; // The fool takes precedence
        if (rank === 1) return '🥇';
        if (rank === 2) return '🥈';
        if (rank === 3) return '🥉';
        return `#${rank}`;
    };

    const sortedResults = Array.from(playerResults.values()).sort((a, b) => a.rank - b.rank);
    const totalPlayers = sortedResults.length;

    return (
        <div style={{
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            minHeight: '100vh',
            width: '100%',
            padding: '1rem',
            boxSizing: 'border-box',
            position: 'relative',
            backgroundColor: '#ad826e'
        }}>
            <WoolBackgroundLayer />
            
            {/* Title */}
            <h1 style={{
                color: 'white',
                fontSize: '2rem',
                fontWeight: 'bold',
                marginBottom: '0.5rem',
                textAlign: 'center',
                position: 'relative',
                zIndex: 10
            }}>
                🎉 <Text id="game_over" /> 🎉
            </h1>

            <h2 style={{
                color: 'white',
                fontSize: '1.2rem',
                fontWeight: 'normal',
                marginBottom: '1.5rem',
                textAlign: 'center',
                position: 'relative',
                zIndex: 10,
                opacity: 0.8
            }}>
                <Text id="final_rankings" />
            </h2>
            
            {/* Player Results */}
            <div style={{
                display: 'flex',
                flexDirection: 'column',
                alignItems: 'center',
                gap: '8px',
                width: '100%',
                maxWidth: '350px',
                position: 'relative',
                zIndex: 10,
                marginBottom: '1.5rem'
            }}>
                {sortedResults.map((result) => {
                    const isCurrentUser = result.player_id === user_id;
                    const playerSeed = (result.player_id.charCodeAt(0) + result.player_id.charCodeAt(1)) / 200;
                    const flip = (result.player_id.charCodeAt(3) || 0) % 2 === 0 ? 1 : -1;

                    return (
                        <div
                            key={result.player_id}
                            style={{
                                border: isCurrentUser ? '3px solid #4ADE80' : '2px solid #5D3A1A',
                                borderRadius: '0',
                                boxShadow: `
                                    inset 0 1px 0 rgba(255,255,255,0.2),
                                    inset 0 -1px 0 rgba(0,0,0,0.3),
                                    0 2px 4px rgba(0,0,0,0.4)`,
                                position: 'relative',
                                overflow: 'hidden',
                                width: '100%',
                                boxSizing: 'border-box',
                                padding: '12px 16px',
                                display: 'flex',
                                flexDirection: 'row',
                                justifyContent: 'space-between',
                                alignItems: 'center',
                                gap: '12px',
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
                                ...getWoodTextureStyle(playerSeed),
                                transform: `scaleX(${flip})`,
                                transformOrigin: 'center center'
                            }} />

                            {/* Left side: Rank + Name */}
                            <div style={{
                                display: 'flex',
                                alignItems: 'center',
                                gap: '12px',
                                flex: 1,
                                minWidth: 0
                            }}>
                                {/* Rank indicator */}
                                <div style={{
                                    fontSize: '1.5rem',
                                    minWidth: '36px',
                                    textAlign: 'center'
                                }}>
                                    {getRankEmoji(result.rank, totalPlayers)}
                                </div>

                                {/* Name */}
                                <div style={{
                                    display: 'flex',
                                    flexDirection: 'column',
                                    minWidth: 0
                                }}>
                                    <span style={{
                                        color: isCurrentUser ? '#4ADE80' : '#fff',
                                        fontWeight: 'bold',
                                        fontSize: '1rem',
                                        overflow: 'hidden',
                                        textOverflow: 'ellipsis',
                                        whiteSpace: 'nowrap',
                                        textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
                                    }}>
                                        {result.is_ai ? '🤖 ' : ''}{result.name}
                                    </span>
                                    {isCurrentUser && (
                                        <span style={{
                                            color: '#4ADE80',
                                            fontSize: '0.75rem',
                                            fontWeight: 'normal',
                                            textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
                                        }}>
                                            (<Text id="you" />)
                                        </span>
                                    )}
                                </div>
                            </div>

                            {/* Right side: ELO */}
                            <div style={{
                                display: 'flex',
                                flexDirection: 'column',
                                alignItems: 'flex-end',
                                flexShrink: 0
                            }}>
                                <span style={{
                                    color: '#fff',
                                    fontSize: '0.85rem',
                                    textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
                                }}>
                                    {result.old_elo} → {result.new_elo}
                                </span>
                                <span style={{
                                    fontWeight: 'bold',
                                    fontSize: '0.9rem',
                                    color: result.elo_change > 0 ? '#4ADE80' : 
                                           result.elo_change < 0 ? '#F87171' : 
                                           '#ccc',
                                    textShadow: '1px 1px 2px rgba(0,0,0,0.8)'
                                }}>
                                    {result.elo_change > 0 ? '+' : ''}{result.elo_change}
                                </span>
                            </div>
                        </div>
                    );
                })}
            </div>
            
            {/* Continue Button */}
            <button
                onClick={handleContinue}
                style={woodButtonStyle}
                onMouseEnter={(e) => {
                    Object.assign(e.currentTarget.style, woodButtonHoverStyle);
                }}
                onMouseLeave={(e) => {
                    Object.assign(e.currentTarget.style, woodButtonStyle);
                }}
            >
                <span style={{ color: '#000' }}>
                    <Text id="continue_to_lobby" />
                </span>
            </button>
        </div>
    );
};
