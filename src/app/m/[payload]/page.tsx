'use client';

// /m/<payload> — the web fallback for an iMessage game (design §13, mockup M7).
//
// macOS Messages does not run iOS extensions, and Android/SMS recipients see the
// raw URL, so this link has to BE a real page rather than an app-store bounce.
// It is also the only marketing surface the protocol gets for free: the payload
// is the whole game, so a stranger with the link can watch it.
//
// Read-only and PUBLIC by construction. Every hand renders as backs and the deck
// is masked — not because this page chooses to hide them, but because it asks
// the kernel for the SPECTATOR view (view.c), the same masking every other
// viewer in the product goes through. Nothing here decides what a stranger sees.
// v1 is deliberately view-only: web-side PLAY (decode → move → produce a link →
// paste it back into the thread) works mechanically but the paste-back UX is
// unproven (§13).
//
// Client-side only, like the replay page: no auth, no database row. The payload
// in the URL is the entire game.
import { useEffect, useState } from 'react';
import { useParams } from 'next/navigation';
import Link from 'next/link';
import { ErrorBoundary } from '../../../components/ErrorBoundary';
import { GameBoard } from '../../../components/GameBoard';
import { ReplayServerProvider } from '../../../contexts/ServerContext';
import { AnimationProvider } from '../../../contexts/AnimationContext';
import { GameProvider } from '../../../contexts/GameContext';
import { DragProvider } from '../../../contexts/DragContext';
import { FernFractalProvider } from '../../../utils/fernFractal';
import type { PersonalGame } from '@api/core/types.ts';

const GAME_ID = 'imessage';

type Loaded = { game: PersonalGame; turn: number; finished: boolean; replayUrl: string | null };

export default function MessagePayloadPage() {
    const segment = useParams<{ payload: string }>().payload || '';
    const [state, setState] = useState<Loaded | null>(null);
    const [error, setError] = useState<string | null>(null);

    useEffect(() => {
        let cancelled = false;
        (async () => {
            try {
                // Dynamic import: the kernel is ~60KB gzipped and this is a cold
                // link from a messenger. Nothing else on the route needs it, so
                // it must not sit in the shared bundle.
                //
                // The BIG module (bots.wasm), because FMSG lives only there —
                // sealing reads a session log the small module cannot hold, and
                // splitting decode out to dodge that would be a second kernel in
                // the tree. The browser fetches it as a static asset — one
                // tracked binary, no base64 twin to drift (see wasm_asset.ts).
                const { kernelMsgDecode, kernelMsgPublicView, kernelResidentReplayCodeV6, ensureBotsAsync } =
                    await import('@sdk/ts/wasm/bots.ts');
                // The bytes arrive over the network here, so the module has to be
                // ready before any of the synchronous kernel calls below.
                await ensureBotsAsync();
                const { base32Decode, base32Encode } = await import('@api/common/replay/codec.ts');
                const { viewToGame } = await import('@sdk/ts/wire/view.ts');

                // The leading char is the TEXT-level format version, so the route
                // dispatches before it decodes any binary (§4.3).
                const text = segment.trim();
                if (text[0] !== '1') throw new Error(`unsupported link version ${text[0]}`);

                // Decoding VALIDATES: the chain replays through the kernel, so a
                // hand-edited payload throws here rather than half-rendering.
                const env = kernelMsgDecode(base32Decode(text.slice(1)));
                // ...and leaves the game resident, which is what this reads.
                const { view } = kernelMsgPublicView();

                const roster = {
                    id: GAME_ID,
                    name: 'iMessage game',
                    players: Array.from({ length: env.n_players }, (_, i) => ({
                        player_id: `p${i}`,
                        // Nicknames are the only identity the payload carries, and
                        // they are self-reported (§4.1). No participant UUID ever
                        // enters an envelope, so there is nothing else to show.
                        name: env.joins.find(j => j.seat === i)?.name || `Seat ${i + 1}`,
                        is_ai: false,
                    })),
                };
                const game = viewToGame(view, roster, -1, { preGood: [], prevGoodTs: null }) as PersonalGame;

                // §12 / batch 6 item B: a FINISHED payload's bubble now links
                // HERE (not to a bare replay code — see MessagesViewController.
                // stage's doc), so the funnel moves one hop out: derive the
                // replay code from what we just decoded and offer it as this
                // page's own CTA. kernelMsgDecode already replayed the whole
                // chain through the ordinary kernel handlers (same as any other
                // play), so the resident game already carries the session log a
                // v6 code needs — only the envelope's own seed has to be handed
                // back in. If derivation is ever impossible for some reason this
                // build can't foresee, fail soft: the finished board still
                // renders, just without the replay CTA (a game-over banner and
                // the install/play CTAs alone still funnel).
                let replayUrl: string | null = null;
                if (env.phase === 3) {
                    try {
                        const code = kernelResidentReplayCodeV6(env.seed);
                        replayUrl = `https://foolish.cards/${base32Encode(code)}`;
                    } catch (e) {
                        // eslint-disable-next-line no-console
                        console.error('[m] replay code derivation failed:', e);
                    }
                }

                if (cancelled) return;
                setState({ game, turn: env.turn, finished: env.phase === 3, replayUrl });
            } catch (e) {
                if (cancelled) return;
                // Never attempt partial recovery: a chain either replays or it
                // does not (§7.3). One sentence for a stranger; the reason goes
                // to the console.
                setError('This game link is damaged.');
                // eslint-disable-next-line no-console
                console.error('[m] payload rejected:', e);
            }
        })();
        return () => { cancelled = true; };
    }, [segment]);

    if (error || !state) {
        return (
            <ErrorBoundary context="iMessage Payload Page">
                <main className="mx-auto flex min-h-screen max-w-md flex-col items-center justify-center gap-4 px-5 text-center">
                    <p className={error ? 'text-lg' : 'opacity-70'}>
                        {error ?? 'Reading the game…'}
                    </p>
                    {error && (
                        <Link className="text-sm underline opacity-80" href="/">
                            Play Durak free right here ›
                        </Link>
                    )}
                </main>
            </ErrorBoundary>
        );
    }

    return (
        <ErrorBoundary context="iMessage Payload Page">
            <FernFractalProvider>
                <ReplayServerProvider gameId={GAME_ID} initialGame={state.game}>
                    <GameProvider>
                        <AnimationProvider>
                            <DragProvider>
                                {/* interactive={false}: no hand, no action bar, no
                                    drag. Watching is the whole offer. */}
                                <GameBoard
                                    interactive={false}
                                    title={state.finished
                                        ? 'An iMessage Durak game'
                                        : `A live iMessage Durak game — turn ${state.turn}`}
                                    chrome={<Funnel finished={state.finished} replayUrl={state.replayUrl} />}
                                />
                            </DragProvider>
                        </AnimationProvider>
                    </GameProvider>
                </ReplayServerProvider>
            </FernFractalProvider>
        </ErrorBoundary>
    );
}

// The funnel (mockup M7). The host app drives installs — the Messages drawer is
// buried under "+" and is not a growth plan (handoff §3.5).
//
// FINISHED (batch 6 item B): the bubble that sent someone here no longer links
// straight to a replay code (see MessagesViewController.stage's doc) — this
// page derived one instead, if it could — so a finished game gets its own
// prominent "Watch the replay" CTA above the usual install/play ones. If
// derivation failed (replayUrl is null), fall back to a plain game-over banner
// so the funnel still reads as "this game ended", just without a broken link.
function Funnel({ finished, replayUrl }: { finished: boolean; replayUrl: string | null }) {
    return (
        <div className="flex flex-col items-center gap-2 px-4 py-3 text-center">
            {finished && (
                replayUrl ? (
                    <a className="rounded-xl px-5 py-2 font-semibold" href={replayUrl}>
                        🎬 Watch the replay
                    </a>
                ) : (
                    <p className="text-sm font-medium opacity-90">This game has ended.</p>
                )
            )}
            <p className="text-sm opacity-70">Hands stay hidden here. Watching is free.</p>
            <a className="rounded-xl px-5 py-2 font-medium" href="https://apps.apple.com/app/foolish">
                📲 Get Foolish on the App Store
            </a>
            <Link className="text-sm underline opacity-80" href="/">
                …or play Durak free right here ›
            </Link>
        </div>
    );
}
