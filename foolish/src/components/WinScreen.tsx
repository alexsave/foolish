import React, { useEffect, useState } from 'react';
import { useServer } from '../contexts/ServerContext';
import { GAME_STATUS } from '../state/view';
import { TexturedSurface, useTexture, getTextureStyle } from './TexturedSurface';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { Text } from './Text';
import { RankIcon } from './SovietIcon';
import { botDisplayName } from '../common/botName';
import { ReplayShare } from './ReplayShare';
// The finish order is the kernel's (anim_plan.c anim_finish_rows), over the
// board's own elimination order and fool, not a second ranking derived here.
import { animFinishRows } from '@sdk/ts/wasm/bots.ts';

interface PlayerResult {
    player_id: string;
    seat: number;
    name: string;
    rank: number;
}

export const WinScreen: React.FC = () => {
    const { view: game, continueGame, loadGame } = useServer();
    const { woodUrl } = useTexture();

    // WHAT THE GAME DID TO EACH RATING IS THE GAME'S OWN RECORD: the kernel
    // stamped it on the game as it ended (c/src/view.h GameRatings) and the view
    // carries it by seat (rated, ratingBefore, ratingDelta). This screen used to
    // ask the rating tables for each player's "rating now" and "rating before
    // their last game" and subtract - which is that player's last change, in
    // whatever game it came from. Reopen an old table, or seat a bot that plays
    // many tables at once, and every row showed a number from some other game.
    //
    // The stamp rides the game's ENVELOPE. The board a game-ending push leaves
    // has none (a push has nowhere to carry one), so a screen that arrives here
    // on a push reads its envelope once, and draws when that read has answered:
    // the rows and their ratings appear together, as they always have, rather
    // than the ratings popping in a moment after. A game that was not rated (or
    // ended before the stamp existed) answers unrated, and shows no rating.
    const gameId = game?.gameId ?? null;
    const over = game?.status === GAME_STATUS.GAME_OVER;
    const rated = !!game?.rated;
    const [askedFor, setAskedFor] = useState<string | null>(null);
    const [answeredFor, setAnsweredFor] = useState<string | null>(null);
    useEffect(() => {
        if (!gameId || !over || rated || askedFor === gameId) return;
        setAskedFor(gameId);
        loadGame(gameId).catch(() => { /* the screen then shows the game unrated */ }).finally(() => setAnsweredFor(gameId));
    }, [gameId, over, rated, askedFor, loadGame]);

    // Render nothing while the game state settles / its envelope loads — the
    // app-wide background shows through, so the game→rankings handoff is seamless.
    if (!game || !over) {
        return null;
    }
    if (!rated && answeredFor !== gameId) {
        return null;
    }

    // The finish order is the kernel's (anim_finish_rows), over the board's own
    // elimination order and fool.
    const sortedResults: PlayerResult[] = [];
    for (const row of animFinishRows([...game.elimination], game.fool, game.seats.length, -1)) {
        const player = game.seats[row.seat];
        if (!player) continue;
        // The name is stored, not rendered: botDisplayName runs below, so a
        // language switch re-renders the city.
        sortedResults.push({ player_id: player.id, seat: row.seat, name: player.name, rank: row.place });
    }
    sortedResults.sort((a, b) => a.rank - b.rank);
    const totalPlayers = sortedResults.length;

    const handleContinue = async () => {
        try {
            await continueGame(game.gameId);
        } catch (error) {
            console.error('Error continuing game:', error);
        }
    };

    return (
        <div className="page" style={{ padding: '0.25rem 1rem 1rem' }}>
            <WoolBackgroundLayer />
            
            <h1 className="win-screen__title">done - {game.title}</h1>

            <div className="win-screen__results">
                {/* One wood plank behind the whole list (CSS hides it in Soviet
                    mode via [data-theme="soviet"] .bg-wood { display: none }). */}
                <div className="bg-wood" style={getTextureStyle(woodUrl, false, 0.42)} />
                {sortedResults.map((result) => {
                    const isCurrentUser = result.seat === game.mySeat;

                    const before = game.ratingBefore[result.seat];
                    const change = game.ratingDelta[result.seat];
                    const eloClass = change > 0
                        ? 'result-card__elo-change--positive'
                        : change < 0
                            ? 'result-card__elo-change--negative'
                            : 'result-card__elo-change--neutral';

                    return (
                        <div
                            key={result.player_id}
                            className={`result-card ${isCurrentUser ? 'result-card--current-user' : ''}`}
                        >
                            <div className="flex items-center gap-md flex-1 min-w-0">
                                <div className="result-card__rank">
                                    <RankIcon rank={result.rank} totalPlayers={totalPlayers} size={22} />
                                </div>

                                <div className="flex items-baseline gap-sm min-w-0">
                                    <span className={`result-card__name ${isCurrentUser ? 'result-card__name--current' : ''}`}>
                                        {botDisplayName(result.name)}
                                    </span>
                                    {isCurrentUser && (
                                        <span className="result-card__you">(<Text id="you" />)</span>
                                    )}
                                </div>
                            </div>

                            {rated && (
                                <div className="result-card__elo">
                                    <span style={{ color: 'var(--color-text-primary)', fontSize: '0.85rem' }}>
                                        {before} → {before + change}
                                    </span>
                                    <span className={`result-card__elo-change ${eloClass}`}>
                                        {change > 0 ? '+' : ''}{change}
                                    </span>
                                </div>
                            )}
                        </div>
                    );
                })}
            </div>

            <ReplayShare gameId={game.gameId} />

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
