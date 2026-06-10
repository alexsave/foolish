import React, { useEffect, useMemo, useState } from 'react';
import { QRCodeSVG } from 'qrcode.react';
import supabase from '../backend/Connector';
import { PersonalGame, PublicGame } from '../common/types';
import { Text } from './Text';
import { base32Encode, hexToBytes, URL_PREFIX } from '../replay/codec';

/**
 * Post-game replay sharing. The server compresses the finished session at
 * game end (supabase/functions/_shared/replay/) into one game_snapshots row,
 * stored binary: `moves` (the rANS move integer) and `extras` (player names +
 * per-move timing). The share code is derived here — base32(moves), with
 * '-' + base32(extras) appended when the checkbox is on. Uppercase base32
 * keeps the QR in alphanumeric mode, a full version smaller than base64 in
 * byte mode. RLS lets exactly the game's participants read the row.
 */

interface ReplayShareProps {
    game: PersonalGame | PublicGame;
}

export const ReplayShare: React.FC<ReplayShareProps> = ({ game }) => {
    const [snapshot, setSnapshot] = useState<{ moves: Uint8Array; extras: Uint8Array | null } | null>(null);
    const [failed, setFailed] = useState(false);
    const [copied, setCopied] = useState(false);
    const [withExtras, setWithExtras] = useState(false);

    useEffect(() => {
        let cancelled = false;

        const fetchSnapshot = async () => {
            try {
                const { data, error } = await supabase
                    .from('game_snapshots')
                    .select('moves, extras')
                    .eq('game_id', game.id)
                    .order('created_at', { ascending: false })
                    .limit(1);
                if (error) throw error;
                if (!data || data.length === 0)
                    throw new Error('no snapshot rows for this game');
                const moves = hexToBytes(data[0].moves as string);
                if (moves.length === 0) throw new Error('empty moves payload');
                const extras = data[0].extras ? hexToBytes(data[0].extras as string) : null;
                if (!cancelled) setSnapshot({ moves, extras });
            } catch (e) {
                console.error('Replay snapshot unavailable:', e);
                if (!cancelled) setFailed(true);
            }
        };

        fetchSnapshot();
        return () => {
            cancelled = true;
        };
    }, [game.id]);

    const view = useMemo(() => {
        if (!snapshot) return null;
        const movesCode = base32Encode(snapshot.moves);
        const hasExtras = snapshot.extras !== null && snapshot.extras.length > 0;
        const code =
            withExtras && hasExtras
                ? `${movesCode}-${base32Encode(snapshot.extras!)}`
                : movesCode;
        return {
            code,
            url: URL_PREFIX + code,
            hasExtras,
            byteLength:
                snapshot.moves.length +
                (withExtras && hasExtras ? snapshot.extras!.length : 0),
        };
    }, [snapshot, withExtras]);

    if (failed || (snapshot && !view)) {
        return (
            <div className="replay-share" style={{ textAlign: 'center', opacity: 0.6, margin: '0.5rem 0' }}>
                <span className="text-shadow" style={{ color: 'var(--color-text-primary)', fontSize: '0.8rem' }}>
                    <Text id="replay_unavailable" />
                </span>
            </div>
        );
    }

    if (!snapshot || !view) {
        return null;
    }

    const handleCopy = async () => {
        try {
            await navigator.clipboard.writeText(view.url);
            setCopied(true);
            setTimeout(() => setCopied(false), 2000);
        } catch (e) {
            console.error('Clipboard write failed:', e);
        }
    };

    return (
        <div
            className="replay-share"
            style={{
                display: 'flex',
                flexDirection: 'column',
                alignItems: 'center',
                gap: '0.6rem',
                margin: '0.75rem 0',
            }}
        >
            <h3 className="win-screen__subtitle" style={{ margin: 0 }}>
                <Text id="share_replay" />
            </h3>

            {/* Uppercase base32 + '-' stay inside the QR alphanumeric charset,
                keeping the symbol small in both formats. */}
            <div style={{ background: '#fff', padding: '10px', borderRadius: '8px', lineHeight: 0 }}>
                <QRCodeSVG value={view.url} size={168} level="L" marginSize={1} />
            </div>

            {view.hasExtras && (
                <label
                    className="text-shadow"
                    style={{
                        display: 'flex',
                        alignItems: 'center',
                        gap: '0.4rem',
                        color: 'var(--color-text-primary)',
                        fontSize: '0.8rem',
                        cursor: 'pointer',
                        userSelect: 'none',
                    }}
                >
                    <input
                        type="checkbox"
                        checked={withExtras}
                        onChange={(e) => setWithExtras(e.target.checked)}
                    />
                    <Text id="with_names_time" />
                </label>
            )}

            <code
                onClick={handleCopy}
                title={`${view.byteLength} bytes`}
                style={{
                    maxWidth: 'min(90vw, 28rem)',
                    wordBreak: 'break-all',
                    fontSize: '0.65rem',
                    cursor: 'pointer',
                    padding: '0.4rem 0.6rem',
                    borderRadius: '6px',
                    background: 'rgba(0, 0, 0, 0.35)',
                    color: 'var(--color-text-primary, #eee)',
                    userSelect: 'all',
                }}
            >
                {view.code}
            </code>

            <span
                className="text-shadow"
                onClick={handleCopy}
                style={{
                    color: 'var(--color-text-primary)',
                    fontSize: '0.75rem',
                    cursor: 'pointer',
                    opacity: 0.8,
                }}
            >
                {copied ? <Text id="copied" /> : <Text id="copy_code" />} · {view.byteLength} B
            </span>
        </div>
    );
};
