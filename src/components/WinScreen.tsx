import React, { useEffect, useState } from 'react';
import { useServer } from '../contexts/ServerContext';
import { useAuth } from '../contexts/AuthContext';
import { GAME_STATUS } from '../common/types';
import supabase from '../backend/Connector';

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
        return <div>Loading...</div>;
    }

    if (loading) {
        return <div>Loading ELO data...</div>;
    }

    const handleContinue = async () => {
        try {
            await continueGame(game.id);
        } catch (error) {
            console.error('Error continuing game:', error);
        }
    };

    return (
        <div className="min-h-screen bg-gradient-to-br from-red-900 via-red-800 to-red-900 p-4">
            <div className="max-w-4xl mx-auto">
                <div className="bg-black/80 backdrop-blur-sm rounded-xl p-8 shadow-2xl border border-red-500/30">
                    <h1 className="text-4xl font-bold text-center text-red-100 mb-8">
                        🎉 Game Over! 🎉
                    </h1>
                    
                    <div className="bg-red-900/50 rounded-lg p-6 mb-8">
                        <h2 className="text-2xl font-semibold text-red-100 mb-4 text-center">
                            Final Rankings
                        </h2>
                        
                        <div className="space-y-3">
                            {Array.from(playerResults.values()).sort((a, b) => a.rank - b.rank).map((result) => (
                                <div 
                                    key={result.player_id}
                                    className={`flex items-center justify-between p-4 rounded-lg border-2 ${
                                        result.player_id === user_id 
                                            ? 'bg-red-700/50 border-red-400' 
                                            : 'bg-red-800/30 border-red-600/30'
                                    }`}
                                >
                                    <div className="flex items-center space-x-4">
                                        <div className={`w-8 h-8 rounded-full flex items-center justify-center font-bold ${
                                            result.rank === 1 ? 'bg-yellow-500 text-yellow-900' :
                                            result.rank === 2 ? 'bg-gray-400 text-gray-900' :
                                            result.rank === 3 ? 'bg-orange-600 text-orange-100' :
                                            'bg-red-600 text-red-100'
                                        }`}>
                                            {result.rank}
                                        </div>
                                        <div>
                                            <span className="text-lg font-semibold text-red-100">
                                                {result.name}
                                                {result.is_ai && <span className="text-red-400 ml-1">(Bot)</span>}
                                                {result.player_id === user_id && <span className="text-red-300 ml-1">(You)</span>}
                                            </span>
                                        </div>
                                    </div>
                                    
                                    <div className="text-right">
                                        <div className="text-red-100">
                                            ELO: {result.old_elo} → {result.new_elo}
                                        </div>
                                        <div className={`font-semibold ${
                                            result.elo_change > 0 ? 'text-green-400' : 
                                            result.elo_change < 0 ? 'text-red-400' : 
                                            'text-gray-400'
                                        }`}>
                                            {result.elo_change > 0 ? '+' : ''}{result.elo_change}
                                        </div>
                                    </div>
                                </div>
                            ))}
                        </div>
                    </div>
                    
                    <div className="text-center">
                        <button
                            onClick={handleContinue}
                            className="bg-red-600 hover:bg-red-700 text-white font-bold py-3 px-8 rounded-lg transition-colors duration-200 shadow-lg hover:shadow-xl"
                        >
                            Continue to Lobby
                        </button>
                    </div>
                </div>
            </div>
        </div>
    );
}; 