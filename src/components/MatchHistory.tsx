import React, { useEffect, useMemo, useState } from 'react';
import { useRouter } from 'next/navigation';
import supabase from '../backend/Connector';
import { useAuth } from '../contexts/AuthContext';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { BackButton } from './BackButton';
import { LanguageSwitcher } from './LanguageSwitcher';
import { Text } from './Text';
import { SovietIcon, RankIcon } from './SovietIcon';
import { useTexture, getTextureStyle, seedFromString, flipFromString } from './TexturedSurface';
import { botDisplayName, isBotName } from '../common/botName';
import { decodeReplay } from '@api/common/replay/decode.ts';
import { ensureBotsAsync } from '@sdk/ts/wasm/bots.ts';
import { decodeExtras, joinReplayCode } from '@api/common/replay/extras.ts';
import { INFO_TYPES } from '@api/common/replay/core.ts';
import { bytesToBigint, hexToBytes } from '@api/common/replay/codec.ts';
import { kernelB32Encode } from '@sdk/ts/wasm/bots.ts';

/**
 * Match history: every finished game the signed-in user played, straight from
 * game_snapshots (RLS returns exactly the rows whose player_ids contain this
 * uid — no extra filter needed). Each row's binary snapshot IS the game, so
 * everything shown — seats, names, who was the fool, your placement — is
 * decoded client-side with the same codec the replay screen uses, and the
 * "watch" link is the same self-contained base32 URL ReplayShare builds.
 * Before this screen, a replay was only reachable from the WinScreen moment;
 * lose the URL and the game was gone.
 */

const PAGE_SIZE = 50;

const INFO_SET = new Set(INFO_TYPES);

interface HistoryEntry {
    id: string;
    code: string;           // base32 moves[-extras] — the replay URL path
    createdAt: Date;
    playerCount: number;
    names: string[];        // seat order, '%'-prefixed for bots
    mySeat: number;
    myRank: number;         // 1 = first out (best), playerCount = the fool
    foolSeat: number;
    durationSec: number | null;
}

const formatDuration = (sec: number): string => {
    const total = Math.round(sec);
    const m = Math.floor(total / 60);
    const s = total % 60;
    return m > 0 ? `${m}m ${s}s` : `${s}s`;
};

// game_snapshots.created_at is TIMESTAMP WITHOUT TIME ZONE holding UTC wall
// time; PostgREST serializes it with no offset, and new Date() would read
// that as LOCAL time. Pin it to UTC unless an offset is already present.
const parseUtcTimestamp = (ts: string): Date =>
    new Date(/[zZ]|[+-]\d\d:?\d\d$/.test(ts) ? ts : ts + 'Z');

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
                    // decodeReplay re-simulates the whole game; yield to the
                    // event loop periodically so a full page of long games
                    // can't freeze input on slow devices.
                    if (i > 0 && i % 10 === 0)
                        await new Promise((r) => setTimeout(r, 0));
                    if (cancelled) return;

                    const row = rows[i];
                    // A snapshot that fails to decode (corrupt / future format)
                    // shouldn't take the whole screen down — skip it.
                    try {
                        const playerIds = (row.player_ids as string[]) ?? [];
                        const mySeat = playerIds.indexOf(user_id);
                        if (mySeat < 0) continue; // cheap check before the expensive decode

                        const movesBytes = hexToBytes(row.moves as string);
                        const extrasBytes = row.extras ? hexToBytes(row.extras as string) : null;
                        const extrasCode = extrasBytes && extrasBytes.length > 0
                            ? kernelB32Encode(extrasBytes)
                            : null;
                        const d = await decodeReplay(bytesToBigint(movesBytes));

                        let names: string[] | null = null;
                        let durationSec: number | null = null;
                        if (extrasCode) {
                            try {
                                let moveCount = 0;
                                for (const l of d.logs) if (INFO_SET.has(l.log_type)) moveCount++;
                                const extras = decodeExtras(extrasCode, d.playerCount, moveCount);
                                names = extras.names;
                                if (extras.moveGaps && extras.moveGaps.length > 0)
                                    durationSec = extras.moveGaps.reduce((a, b) => a + b, 0);
                            } catch (e) {
                                console.error('History extras ignored:', e);
                            }
                        }

                        // The fool is never in eliminationOrder (see DecodedReplay),
                        // so the fallback IS the fool's last place.
                        const rankIndex = d.eliminationOrder.indexOf(mySeat);
                        const myRank = rankIndex >= 0 ? rankIndex + 1 : d.playerCount;

                        decoded.push({
                            id: row.id as string,
                            code: joinReplayCode(kernelB32Encode(movesBytes), extrasCode),
                            createdAt: parseUtcTimestamp(row.created_at as string),
                            playerCount: d.playerCount,
                            names: names ?? playerIds.map((_, seat) => `#${seat + 1}`),
                            mySeat,
                            myRank,
                            foolSeat: d.fool,
                            durationSec,
                        });
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

            {stats && (
                <div className="z-content flex flex-col items-center" style={{ width: '100%', maxWidth: 420 }}>
                    <span className="text-shadow" style={{ color: 'var(--color-text-muted)', fontSize: '0.75rem', marginBottom: 4 }}>
                        <Text id="your_stats" />
                    </span>
                    <div className="result-card" style={{ marginBottom: '0.8rem' }}>
                        <div
                            className="bg-wood"
                            style={{
                                ...getTextureStyle(woodUrl, false, seedFromString(user_id ?? 'stats')),
                            }}
                        />
                        <div className="flex flex-1 items-center" style={{ justifyContent: 'space-around', gap: '0.5rem' }}>
                            {[
                                { label: 'rating' as const, value: elo ? String(elo.rating) : '—' },
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
            )}

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

                {entries?.map((entry) => {
                    const survived = entry.mySeat !== entry.foolSeat;
                    const seed = seedFromString(entry.id);
                    const flip = flipFromString(entry.id);

                    return (
                        <div
                            key={entry.id}
                            className="result-card"
                            style={{ cursor: 'pointer' }}
                            onClick={() => router.push(`/${entry.code}`)}
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
                })}
            </div>

            <LanguageSwitcher />
        </div>
    );
};
