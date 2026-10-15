import React, { useEffect, useMemo, useState } from 'react';
import { board } from '../common/backend';
import { useAuth } from '../contexts/AuthContext';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { BackButton } from './BackButton';
import { LanguageSwitcher } from './LanguageSwitcher';
import { Text } from './Text';
import { SovietIcon, RankIcon } from './SovietIcon';
import { TexturedSurface, useTexture, getTextureStyle, seedFromString, flipFromString } from './TexturedSurface';
import { botDisplayName } from '../common/botName';

/**
 * Global standings: the server's top players and top bots who have played
 * (GET /v1/leaderboard). Both are rated by the same per-game pairwise Elo, so
 * they share one ladder; ranks are computed once over the merged list and the
 * tabs only filter — an entry keeps its overall rank on every tab.
 */

type Filter = 'all' | 'humans' | 'bots';

interface Entry {
    id: string;
    name: string;
    is_ai: boolean;
    elo: number;
    games: number;
    rank: number;
}

const TABS: { key: Filter; label: 'tab_all' | 'tab_humans' | 'tab_bots' }[] = [
    { key: 'all', label: 'tab_all' },
    { key: 'humans', label: 'tab_humans' },
    { key: 'bots', label: 'tab_bots' },
];

// Each source query is capped at 100 rows, so ranks are exact only within the
// top 100 of each population; the merged board is capped to the top 100
// overall to avoid showing ranks past the point where they could be wrong.
const BOARD_SIZE = 100;

export const Leaderboard: React.FC = () => {
    const { user_id } = useAuth();
    const { woodUrl } = useTexture();
    const [entries, setEntries] = useState<Entry[] | null>(null);
    const [failed, setFailed] = useState(false);
    const [filter, setFilter] = useState<Filter>('all');

    useEffect(() => {
        let cancelled = false;

        const load = async () => {
            try {
                const rows = await board();

                const merged = rows.map((r) => ({
                    id: r.id,
                    // A deleted account keeps its row with no name; a truncated
                    // uid still identifies it. A bot's stored nickname, prefix
                    // and all, is rendered through botDisplayName below, so a
                    // language switch re-renders the city.
                    name: r.name || `#${r.id.slice(0, 8)}`,
                    is_ai: r.isBot === 1,
                    elo: r.rating,
                    games: r.games,
                }))
                    .sort((a, b) => b.elo - a.elo)
                    .slice(0, BOARD_SIZE)
                    .map((e, i) => ({ ...e, rank: i + 1 }));

                if (!cancelled) setEntries(merged);
            } catch (e) {
                console.error('Leaderboard load failed:', e);
                if (!cancelled) setFailed(true);
            }
        };

        load();
        return () => {
            cancelled = true;
        };
    }, []);

    const visible = useMemo(() => {
        if (!entries) return null;
        if (filter === 'humans') return entries.filter((e) => !e.is_ai);
        if (filter === 'bots') return entries.filter((e) => e.is_ai);
        return entries;
    }, [entries, filter]);

    return (
        <div className="page page--full-height">
            <WoolBackgroundLayer />
            <BackButton />

            <h1 className="title title--section">
                <Text id="leaderboard" />
            </h1>

            <div className="flex items-center gap-sm mb-md z-content">
                {TABS.map((tab, i) => (
                    <TexturedSurface
                        key={tab.key}
                        as="button"
                        seed={0.3 + i * 0.17}
                        onClick={() => setFilter(tab.key)}
                        className={`btn-wood btn-wood--sm ${filter === tab.key ? 'leaderboard__tab--active' : ''}`}
                    >
                        <span className="btn-wood-text">
                            <Text id={tab.label} />
                        </span>
                    </TexturedSurface>
                ))}
            </div>

            <div className="win-screen__results" style={{ maxWidth: 420 }}>
                {visible === null && (
                    <div className="empty-state">
                        <Text id={failed ? 'load_failed' : 'loading'} />
                    </div>
                )}

                {visible !== null && visible.length === 0 && (
                    <div className="empty-state">
                        <Text id="no_ranked_players" />
                    </div>
                )}

                {visible?.map((entry) => {
                    const isCurrentUser = !entry.is_ai && entry.id === user_id;
                    const seed = seedFromString(entry.id);
                    const flip = flipFromString(entry.id);

                    return (
                        <div
                            key={entry.id}
                            className={`result-card ${isCurrentUser ? 'result-card--current-user' : ''}`}
                        >
                            {/* CSS hides this in Soviet mode via [data-theme="soviet"] .bg-wood { display: none } */}
                            <div
                                className="bg-wood"
                                style={{
                                    ...getTextureStyle(woodUrl, false, seed),
                                    transform: `scaleX(${flip})`,
                                }}
                            />

                            <div className="flex items-center gap-md flex-1 min-w-0">
                                <div className="result-card__rank">
                                    {/* totalPlayers is a "never the fool" sentinel: RankIcon
                                        shows the fool card at rank === totalPlayers, which has
                                        no meaning on a ladder. */}
                                    <RankIcon rank={entry.rank} totalPlayers={(entries?.length ?? 0) + 1} size={26} />
                                </div>

                                <div className="flex flex-col min-w-0">
                                    <span className={`result-card__name ${isCurrentUser ? 'result-card__name--current' : ''}`}>
                                        <SovietIcon name={entry.is_ai ? 'bot' : 'person'} size={14} /> {botDisplayName(entry.name)}
                                    </span>
                                    {isCurrentUser && (
                                        <span className="result-card__you">
                                            (<Text id="you" />)
                                        </span>
                                    )}
                                </div>
                            </div>

                            <div className="result-card__elo">
                                <span className="text-shadow" style={{ color: 'var(--color-text-primary)', fontSize: '1rem', fontWeight: 'bold' }}>
                                    {entry.elo}
                                </span>
                                <span className="text-shadow" style={{ color: 'var(--color-text-muted)', fontSize: '0.7rem' }}>
                                    <Text id="games_label" />: {entry.games}
                                </span>
                            </div>
                        </div>
                    );
                })}
            </div>

            <LanguageSwitcher />
        </div>
    );
};
