import React, { useEffect, useState } from 'react';
import { useServer } from '../contexts/ServerContext';
import { useAuth } from '../contexts/AuthContext';
import { GAME_STATUS } from '@shared/types.ts';
import supabase from '../backend/Connector';
import { TexturedSurface, useTexture, getTextureStyle, seedFromString, flipFromString } from './TexturedSurface';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { Text } from './Text';
import { SovietIcon, RankIcon } from './SovietIcon';
import { ReplayShare } from './ReplayShare';

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
    const { woodUrl } = useTexture();
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

    // Render nothing while the game state settles / ELO data loads — the
    // app-wide background shows through, so the game→rankings handoff is seamless.
    if (!game || game.status !== GAME_STATUS.GAME_OVER) {
        return null;
    }

    if (loading) {
        return null;
    }

    const handleContinue = async () => {
        try {
            await continueGame(game.id);
        } catch (error) {
            console.error('Error continuing game:', error);
        }
    };

    // getRankEmoji is now replaced by RankIcon component

    const sortedResults = Array.from(playerResults.values()).sort((a, b) => a.rank - b.rank);
    const totalPlayers = sortedResults.length;

    return (
        <div className="page" style={{ padding: '1rem' }}>
            <WoolBackgroundLayer />
            
            <h1 className="win-screen__title">
                <SovietIcon name="celebration" size={32} /> <Text id="game_over" /> <SovietIcon name="celebration" size={32} />
            </h1>

            <div className="win-screen__results">
                {sortedResults.map((result) => {
                    const isCurrentUser = result.player_id === user_id;
                    const playerSeed = seedFromString(result.player_id);
                    const flip = flipFromString(result.player_id);

                    const eloClass = result.elo_change > 0 
                        ? 'result-card__elo-change--positive' 
                        : result.elo_change < 0 
                            ? 'result-card__elo-change--negative' 
                            : 'result-card__elo-change--neutral';

                    return (
                        <div
                            key={result.player_id}
                            className={`result-card ${isCurrentUser ? 'result-card--current-user' : ''}`}
                        >
                            {/* CSS hides this in Soviet mode via [data-theme="soviet"] .bg-wood { display: none } */}
                            <div 
                                className="bg-wood"
                                style={{
                                    ...getTextureStyle(woodUrl, false, playerSeed),
                                    transform: `scaleX(${flip})`,
                                }} 
                            />

                            <div className="flex items-center gap-md flex-1 min-w-0">
                                <div className="result-card__rank">
                                    <RankIcon rank={result.rank} totalPlayers={totalPlayers} size={28} />
                                </div>

                                <div className="flex flex-col min-w-0">
                                    <span className={`result-card__name ${isCurrentUser ? 'result-card__name--current' : ''}`}>
                                        {result.is_ai ? <><SovietIcon name="bot" size={14} /> </> : ''}{result.name}
                                    </span>
                                    {isCurrentUser && (
                                        <span className="result-card__you">
                                            (<Text id="you" />)
                                        </span>
                                    )}
                                </div>
                            </div>

                            <div className="result-card__elo">
                                <span className="text-shadow" style={{ color: 'var(--color-text-primary)', fontSize: '0.85rem' }}>
                                    {result.old_elo} → {result.new_elo}
                                </span>
                                <span className={`result-card__elo-change ${eloClass}`}>
                                    {result.elo_change > 0 ? '+' : ''}{result.elo_change}
                                </span>
                            </div>
                        </div>
                    );
                })}
            </div>

            <ReplayShare game={game} />

            <TexturedSurface
                as="button"
                seed={0.6}
                onClick={handleContinue}
                className="btn-wood btn-wood--lg"
            >
                <span className="btn-wood-text">
                    <Text id="continue_to_lobby" />
                </span>
            </TexturedSurface>
        </div>
    );
};
