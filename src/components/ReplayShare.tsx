import React, { useEffect, useMemo, useState } from 'react';
import { QRCodeSVG } from 'qrcode.react';
import supabase from '../backend/Connector';
import { PersonalGame, PublicGame } from '../common/types';
import { Text } from './Text';
import { base64Decode, bytesToBigint, gameToUrl } from '../replay/codec';

/**
 * Post-game replay sharing. The server compresses the finished session into a
 * single integer at game end (supabase/functions/_shared/replay/encode.ts)
 * and appends it, base64-encoded, to games.snapshots; the session's logs are
 * wiped — the snapshot IS the game now. This component just shows the latest
 * snapshot as a copyable base64 string and a QR code of the replay URL.
 *
 * The QR holds the uppercase base32 URL form: every character is in the QR
 * alphanumeric charset, which keeps the symbol small. The future replay
 * screen decodes that URL with no auth and no database — the whole game is in
 * the path (src/replay/decode.ts).
 */

interface ReplayShareProps {
    game: PersonalGame | PublicGame;
}

export const ReplayShare: React.FC<ReplayShareProps> = ({ game }) => {
    const [snapshot, setSnapshot] = useState<string | null>(
        game.snapshots && game.snapshots.length > 0
            ? game.snapshots[game.snapshots.length - 1]
            : null,
    );
    const [failed, setFailed] = useState(false);
    const [copied, setCopied] = useState(false);

    useEffect(() => {
        if (snapshot) return;
        let cancelled = false;

        // The broadcast game state usually carries snapshots already; this
        // fallback covers clients holding a stale state from before game end.
        const fetchSnapshot = async () => {
            try {
                const { data, error } = await supabase
                    .from('games')
                    .select('snapshots')
                    .eq('id', game.id)
                    .single();
                if (error) throw error;
                const all: string[] = data?.snapshots ?? [];
                if (all.length === 0) throw new Error('no snapshots on game row');
                if (!cancelled) setSnapshot(all[all.length - 1]);
            } catch (e) {
                console.error('Replay snapshot unavailable:', e);
                if (!cancelled) setFailed(true);
            }
        };

        fetchSnapshot();
        return () => {
            cancelled = true;
        };
    }, [game.id, snapshot]);

    const view = useMemo(() => {
        if (!snapshot) return null;
        try {
            const bytes = base64Decode(snapshot);
            return { url: gameToUrl(bytesToBigint(bytes)), byteLength: bytes.length };
        } catch (e) {
            console.error('Bad replay snapshot:', e);
            return null;
        }
    }, [snapshot]);

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
            await navigator.clipboard.writeText(snapshot);
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

            <div style={{ background: '#fff', padding: '10px', borderRadius: '8px', lineHeight: 0 }}>
                <QRCodeSVG value={view.url} size={168} level="L" marginSize={1} />
            </div>

            <code
                onClick={handleCopy}
                title={`${view.byteLength} bytes`}
                style={{
                    maxWidth: 'min(90vw, 28rem)',
                    wordBreak: 'break-all',
                    fontSize: '0.7rem',
                    cursor: 'pointer',
                    padding: '0.4rem 0.6rem',
                    borderRadius: '6px',
                    background: 'rgba(0, 0, 0, 0.35)',
                    color: 'var(--color-text-primary, #eee)',
                    userSelect: 'all',
                }}
            >
                {snapshot}
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
