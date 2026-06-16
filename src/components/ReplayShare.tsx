import React, { useEffect, useMemo, useState } from 'react';
import { QRCodeSVG } from 'qrcode.react';
import supabase from '../backend/Connector';
import { PersonalGame, PublicGame } from '../common/types';
import { Text } from './Text';
import { base32Encode, hexToBytes, URL_PREFIX } from '../replay/codec';
import { useStyles } from '../contexts/StyleContext';
import { useTexture, getTextureStyle } from './TexturedSurface';

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
    // Names + timing are on by default — a replay is far more useful with who
    // played and when. The toggle still lets you drop them for a shorter code /
    // denser QR when the snapshot actually carries an extras blob.
    const [withExtras, setWithExtras] = useState(true);

    // Wood-framed QR to match the lobby join code: the same texture + carved
    // border + bevel, with the QR drawn on transparent so the grain shows
    // through. Soviet theme drops the wood (like the lobby) and keeps the frame.
    const styles = useStyles();
    const { woodUrl } = useTexture();
    const useWoodTexture = styles.texture.useWoodTexture;
    const frameTexture = useWoodTexture ? getTextureStyle(woodUrl, false, 0.2) : null;

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
        return { url: URL_PREFIX + code, hasExtras };
    }, [snapshot, withExtras]);

    const handleCopy = async () => {
        if (!view) return;
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
                // Sit above the absolutely-positioned wool/vignette layers
                // (.bg-wool is z-index 0 but opaque, so a static box renders
                // BEHIND it — which is why the QR was invisible). The lobby QR
                // works for the same reason: .lobby__qr-container is z-index 10.
                position: 'relative',
                zIndex: 'var(--z-content)',
                display: 'flex',
                flexDirection: 'column',
                alignItems: 'center',
                gap: '0.6rem',
                margin: '0 0 0.5rem',
            }}
        >
            {/* Fixed-footprint slot: the code only exists after an async
                snapshot fetch, so reserve the QR's space up front — otherwise the
                section pops in once the fetch lands and shoves the Continue button
                down. The same box shows a loading / unavailable note until ready. */}
            <div
                style={{
                    width: 188,
                    height: 188,
                    boxSizing: 'border-box',
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'center',
                    padding: 12,
                    borderRadius: 10,
                    // Carved wood frame, same as .lobby__qr-container.
                    border: '2px solid #5D3A1A',
                    boxShadow:
                        'inset 0 1px 0 rgba(255,255,255,0.2), inset 0 -1px 0 rgba(0,0,0,0.3), 0 3px 6px rgba(0,0,0,0.4)',
                    backgroundColor: 'rgba(0, 0, 0, 0.25)',
                    ...(frameTexture ?? {}),
                    lineHeight: 0,
                }}
            >
                {view ? (
                    <QRCodeSVG
                        value={view.url}
                        size={160}
                        level="L"
                        fgColor="#000"
                        bgColor="transparent"
                    />
                ) : (
                    <span
                        className="text-shadow"
                        style={{
                            color: 'var(--color-text-primary)',
                            fontSize: '0.8rem',
                            textAlign: 'center',
                            lineHeight: 1.35,
                            opacity: 0.85,
                        }}
                    >
                        <Text id={failed ? 'replay_unavailable' : 'loading'} />
                    </span>
                )}
            </div>

            {/* Reserve the controls row too, so the extras toggle / copy
                button appearing don't nudge the layout a second time. Single row:
                the with-names checkbox and the copy-code link side by side. */}
            <div
                style={{
                    minHeight: '1.5rem',
                    width: '100%',
                    display: 'flex',
                    flexDirection: 'row',
                    flexWrap: 'wrap',
                    alignItems: 'center',
                    justifyContent: 'center',
                    gap: '0.9rem',
                }}
            >
                {view?.hasExtras && (
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

                {view && (
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
                        {copied ? <Text id="copied" /> : <Text id="copy_code" />}
                    </span>
                )}
            </div>
        </div>
    );
};
