import React, { useEffect, useMemo, useState } from 'react';
import { QRCodeSVG } from 'qrcode.react';
import supabase from '../backend/Connector';
import { PersonalGame, PublicGame } from '@api/core/types.ts';
import { Text } from './Text';
import { hexToBytes } from '@api/common/replay/codec.ts';
import { kernelB32Encode, kernelReplayLink, REPLAY_LINK } from '@sdk/ts/wasm/bots.ts';
import { useStyles } from '../contexts/StyleContext';
import { useTexture, getTextureStyle } from './TexturedSurface';

/**
 * Post-game replay sharing. The server compresses the finished session at
 * game end (server/api/common/replay/) into one game_snapshots row,
 * stored binary: `moves` (the rANS move integer) and `extras` (player names +
 * per-move timing). The code is base32(moves), with '-' + base32(extras) when
 * the checkbox is on, and both the base32 and the link around it come from the
 * kernel (replay_b32_encode, replay_extras_link_styled) rather than being
 * assembled here.
 *
 * The QR and the clipboard get DIFFERENT forms of the same link. Uppercase
 * base32 with a scheme-less prefix keeps the QR in alphanumeric mode, a full
 * version smaller than byte mode; what a person copies wants the https form,
 * which a browser or a chat client auto-links. RLS lets exactly the game's
 * participants read the row.
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
        const movesCode = kernelB32Encode(snapshot.moves);
        const hasExtras = snapshot.extras !== null && snapshot.extras.length > 0;
        const code =
            withExtras && hasExtras
                ? `${movesCode}-${kernelB32Encode(snapshot.extras!)}`
                : movesCode;
        // Two forms of ONE link, both built by the kernel
        // (replay_extras_link_styled). The QR takes the uppercase scheme-less
        // one, which stays in QR alphanumeric mode and so fits a smaller
        // version - that is the whole reason the uppercase form exists. What a
        // person copies takes the https one, which a browser or a chat client
        // will actually auto-link.
        return {
            url: kernelReplayLink(code, [], REPLAY_LINK.url),
            qr: kernelReplayLink(code, [], REPLAY_LINK.qr),
            hasExtras,
        };
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
                        value={view.qr}
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
