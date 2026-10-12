import React, { useEffect, useMemo, useState } from 'react';
import { useRouter } from 'next/navigation';
import supabase from '../common/Connector';
import { useAuth } from '../contexts/AuthContext';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { BackButton } from './BackButton';
import { LanguageSwitcher } from './LanguageSwitcher';
import { Text } from './Text';
import { SovietIcon, RankIcon } from './SovietIcon';
import { useTexture, getTextureStyle, seedFromString, flipFromString } from './TexturedSurface';
import { botDisplayName, isBotName } from '../common/botName';
import { ensureBotsAsync } from '@sdk/ts/wasm/bots.ts';
import { decodeHistoryRow, type HistoryEntry } from './matchHistoryRow';

/**
 * Match history: every finished game the signed-in user played, straight from
 * game_snapshots (RLS returns exactly the rows whose player_ids contain this
 * uid - no extra filter needed). Each row's binary snapshot IS the game, so
 * everything shown - seats, names, who was the fool, your placement - is
 * read client-side by the kernel the replay screen uses, and the
 * "watch" link is the same self-contained base32 URL ReplayShare builds.
 * Before this screen, a replay was only reachable from the WinScreen moment;
 * lose the URL and the game was gone.
 */

const PAGE_SIZE = 50;

interface HistoryStats {
    games: number;
    fools: number;
    survival: number;       // percent of the listed games not lost
}

const formatDuration = (sec: number): string => {
    const total = Math.round(sec);
    const m = Math.floor(total / 60);
    const s = total % 60;
    return m > 0 ? `${m}m ${s}s` : `${s}s`;
};

// The four tiles above the list: the lifetime rating, then the listed games' totals.
const StatsCard: React.FC<{ stats: HistoryStats; elo: { rating: number } | null; woodUrl: string | null; userId: string | null }> = ({ stats, elo, woodUrl, userId }) => (
    <div className="z-content flex flex-col items-center" style={{ width: '100%', maxWidth: 420 }}>
        <span className="text-shadow" style={{ color: 'var(--color-text-muted)', fontSize: '0.75rem', marginBottom: 4 }}>
            <Text id="your_stats" />
        </span>
        <div className="result-card" style={{ marginBottom: '0.8rem' }}>
            <div
                className="bg-wood"
                style={{
                    ...getTextureStyle(woodUrl, false, seedFromString(userId ?? 'stats')),
                }}
            />
            <div className="flex flex-1 items-center" style={{ justifyContent: 'space-around', gap: '0.5rem' }}>
                {[
                    { label: 'rating' as const, value: elo ? String(elo.rating) : '-' },
                    { label: 'games_label' as const, value: String(stats.games) },
                    { label: 'survival_rate' as const, value: `${stats.survival}%` },
                    { label: 'times_fool' as const, value: String(stats.fools) },
                ].map((tile) => (
                    <div key={tile.label} className="flex flex-col items-center" style={{ minWidth: 0 }}>
                        <span className="text-shadow" style={{ color: 'var(--color-text-primary)', fontSize: '1.1rem', fontWeight: 'bold' }}>
                            {tile.value}
                        </span>
                        <span className="text-shadow" style={{ color: 'var(--color-text-muted)', fontSize: '0.65rem', textAlign: 'center' }}>
                            <Text id={tile.label} />
                        </span>
                    </div>
                ))}
            </div>
        </div>
    </div>
);

// One finished game: how it went for you, who sat where, and the way into its replay.
const HistoryRow: React.FC<{ entry: HistoryEntry; woodUrl: string | null; onOpen: () => void }> = ({ entry, woodUrl, onOpen }) => {
    const survived = entry.mySeat !== entry.foolSeat;
    const seed = seedFromString(entry.id);
    const flip = flipFromString(entry.id);

    return (
        <div
            className="result-card"
            style={{ cursor: 'pointer' }}
            onClick={onOpen}
        >
            <div
                className="bg-wood"
                style={{
                    ...getTextureStyle(woodUrl, false, seed),
                    transform: `scaleX(${flip})`,
                }}
            />

            <div className="flex items-center gap-md flex-1 min-w-0">
                <div className="result-card__rank">
                    <RankIcon rank={entry.myRank} totalPlayers={entry.playerCount} size={26} />
                </div>

                <div className="flex flex-col min-w-0" style={{ gap: '2px' }}>
                    <span
                        className={`result-card__name ${survived ? 'result-card__name--current' : ''}`}
                        style={survived ? undefined : { color: 'var(--color-error)' }}
                    >
                        <Text id={survived ? 'result_survived' : 'result_fool'} />
                    </span>
                    <span className="text-shadow" style={{ color: 'var(--color-text-primary)', fontSize: '0.75rem', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                        {entry.names.map((name, seat) => (
                            <span key={seat} style={{ marginRight: 6, fontWeight: seat === entry.mySeat ? 'bold' : 'normal' }}>
                                {isBotName(name) && <SovietIcon name="bot" size={11} />}
                                {seat === entry.foolSeat && <SovietIcon name="fool" size={11} />}
                                {botDisplayName(name)}
                            </span>
                        ))}
                    </span>
                    <span className="text-shadow" style={{ color: 'var(--color-text-muted)', fontSize: '0.65rem' }}>
                        {entry.createdAt.toLocaleDateString()}
                        {entry.durationSec !== null && ` · ${formatDuration(entry.durationSec)}`}
                        {` · ${entry.playerCount}p`}
                    </span>
                </div>
            </div>

            <div className="result-card__elo" style={{ flexShrink: 0 }}>
                <span className="text-shadow" style={{ color: 'var(--color-text-primary)', fontSize: '0.75rem', opacity: 0.9 }}>
                    <Text id="watch_replay" /> ▶
                </span>
            </div>
        </div>
    );
};

export const MatchHistory: React.FC = () => {
    const router = useRouter();
    const { user_id } = useAuth();
    const { woodUrl } = useTexture();
    const [entries, setEntries] = useState<HistoryEntry[] | null>(null);
    const [elo, setElo] = useState<{ rating: number } | null>(null);
    const [failed, setFailed] = useState(false);

    useEffect(() => {
        if (!user_id) return;
        let cancelled = false;

        const load = async () => {
            try {
                const [snapshots, rating] = await Promise.all([
                    supabase
                        .from('game_snapshots')
                        .select('id, player_ids, moves, extras, created_at')
                        .order('created_at', { ascending: false })
                        .limit(PAGE_SIZE),
                    supabase
                        .from('user_elo_ratings')
                        .select('elo_rating')
                        .eq('user_id', user_id)
                        .limit(1),
                ]);
                if (snapshots.error) throw snapshots.error;
                if (rating.error) console.error('Rating load failed:', rating.error);

                const rows = snapshots.data ?? [];
                // The extras blob is decoded by the kernel (decodeExtras ->
                // wasm_replay_extras_decode), which is a synchronous call into
                // a module that has to be warm first.
                await ensureBotsAsync();
                const decoded: HistoryEntry[] = [];
                for (let i = 0; i < rows.length; i++) {
                    // the summary decodes the whole code; yield to the
                    // event loop periodically so a full page of long games
                    // can't freeze input on slow devices.
                    if (i > 0 && i % 10 === 0)
                        await new Promise((r) => setTimeout(r, 0));
                    if (cancelled) return;

                    // A snapshot that fails to decode (corrupt / future format)
                    // shouldn't take the whole screen down - skip it.
                    try {
                        const entry = decodeHistoryRow(rows[i], user_id);
                        if (entry) decoded.push(entry);
                    } catch (e) {
                        console.error('History snapshot skipped:', e);
                    }
                }

                if (!cancelled) {
                    setEntries(decoded);
                    const r = rating.data?.[0];
                    if (r) setElo({ rating: r.elo_rating as number });
                }
            } catch (e) {
                console.error('Match history load failed:', e);
                if (!cancelled) setFailed(true);
            }
        };

        load();
        return () => {
            cancelled = true;
        };
    }, [user_id]);

    // All three computed tiles describe the games listed below (the fetched
    // window), so the card stays internally consistent; only the rating is a
    // lifetime figure, and it's current by definition.
    const stats = useMemo(() => {
        if (!entries || entries.length === 0) return null;
        const fools = entries.filter((e) => e.mySeat === e.foolSeat).length;
        return {
            games: entries.length,
            fools,
            survival: Math.round(((entries.length - fools) / entries.length) * 100),
        };
    }, [entries]);

    return (
        <div className="page page--full-height">
            <WoolBackgroundLayer />
            <BackButton />

            <h1 className="title title--section">
                <Text id="match_history" />
            </h1>

            {stats && <StatsCard stats={stats} elo={elo} woodUrl={woodUrl} userId={user_id} />}

            <div className="win-screen__results" style={{ maxWidth: 420 }}>
                {entries === null && (
                    <div className="empty-state">
                        <Text id={failed ? 'load_failed' : 'loading'} />
                    </div>
                )}

                {entries !== null && entries.length === 0 && (
                    <div className="empty-state">
                        <Text id="no_match_history" />
                    </div>
                )}

                {entries?.map((entry) => (
                    <HistoryRow key={entry.id} entry={entry} woodUrl={woodUrl} onOpen={() => router.push(`/${entry.code}`)} />
                ))}
            </div>

            <LanguageSwitcher />
        </div>
    );
};
