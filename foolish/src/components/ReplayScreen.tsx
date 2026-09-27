import React, { useEffect, useState } from 'react';
import Link from 'next/link';
import { Text } from './Text';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { ReplayServerProvider } from '../contexts/ServerContext';
import { AnimationProvider } from '../contexts/AnimationContext';
import { GameProvider } from '../contexts/GameContext';
import { DragProvider } from '../contexts/DragContext';
import { FernFractalProvider } from '../utils/fernFractal';
import { bigintToBytes, bytesToBigint } from '@api/common/replay/codec.ts';
import { ensureBotsAsync, kernelB32Decode, replaySummary } from '@sdk/ts/wasm/bots.ts';
import {
    buildReplayFrames,
    buildReverseFrames,
    preDealGame,
    stepTimes,
    REPLAY_KEY,
} from '../replay/frames';
import { splitReplayCode, decodeExtras, ReplayExtras } from '@api/common/replay/extras.ts';
import { ReplayStage } from './replay/ReplayStage';

/**
 * Self-contained replay viewer: WWW.FOOLISH.CARDS/<base32> - the path segment
 * IS the entire game (decoded client-side, no auth, no database row).
 *
 * It IS the real game UI: the same display components, driven by the same
 * AnimationProvider, fed the same animation-sequence messages a live game
 * receives - just published into src/state/animationFeed from the decoded
 * integer instead of a supabase channel. Stepping forward plays the event
 * with its full animation; seeking commits the target state directly.
 *
 * The screen itself is the code, the providers and the failure page. What the
 * deck head does is src/replay/usePlayback.ts, what the Oracle does is
 * src/replay/useOracle.ts, and the board and its controls are
 * src/components/replay/ReplayStage.tsx.
 */

const buildReplayData = async (code: string) => {
    const { moves, extras: extrasCode } = splitReplayCode(code);
    const bytes = bigintToBytes(bytesToBigint(kernelB32Decode(moves)));
    await ensureBotsAsync();

    // The code at a glance, from the kernel: the seats, the fool, and how many
    // moves the extras time. A code that does not decode is not a replay.
    const summary = replaySummary(bytes);
    if (!summary) throw new Error('replay: the code does not decode');

    // extras (names + timing) are decoration: a malformed blob never
    // breaks the replay itself
    let extras: ReplayExtras = { names: null, startTime: null, moveGaps: null };
    if (extrasCode) {
        try {
            extras = decodeExtras(extrasCode, summary.numPlayers, summary.moves);
        } catch (e) {
            console.error('Replay extras ignored:', e);
        }
    }

    const names = extras.names;
    const fool = summary.fool >= 0 ? summary.fool : null;
    // The game, replayed by the engine: one frame per step, each the board the
    // engine really committed and the events it really produced.
    // The gaps come in with the names: they are what tells one multi-cover apart
    // from a defender who covered, sent, and covered again (frames.ts mergeCoverRuns).
    const frames = buildReplayFrames(bytes, REPLAY_KEY, names, { fool, moveGaps: extras.moveGaps });
    const reverses = buildReverseFrames(frames);
    const initial = preDealGame(frames[0]);
    const times = stepTimes(frames, extras.startTime, extras.moveGaps);
    return { fool, code: bytes, frames, reverses, initial, names, times };
};

export const ReplayScreen = ({ code }: { code: string }) => {
    // Client-only: the game display reads window dimensions during render
    // (Chat's viewport state, the animation overlay's slot keys), so skip
    // SSR/prerender entirely.
    const [mounted, setMounted] = useState(false);
    useEffect(() => setMounted(true), []);

    // The link names the replay (the Oracle seeds its decisions by it); the store
    // holds its boards under a short key of its own, as the tutorial's does. A
    // board's game id is a table's, and a share link - its moves and its extras -
    // is longer than a table's id may be: the kernel's board writer refuses it.
    const gameId = code.toLowerCase();

    // Async: the replay runs in bots.wasm, which the browser must compile
    // asynchronously. undefined = still decoding, null = failed.
    const [result, setResult] = useState<Awaited<ReturnType<typeof buildReplayData>> | null | undefined>(undefined);
    useEffect(() => {
        if (!mounted) return;
        let cancelled = false;
        setResult(undefined); // a code change must not keep showing the old replay
        buildReplayData(code)
            .then((r) => { if (!cancelled) setResult(r); })
            .catch((e) => {
                console.error('Replay decode failed:', e);
                if (!cancelled) setResult(null);
            });
        return () => { cancelled = true; };
    }, [code, mounted]);

    if (!mounted || result === undefined) {
        return null;
    }

    if (!result) {
        return (
            <div className="page" style={{ padding: '2rem', textAlign: 'center' }}>
                <WoolBackgroundLayer />
                <h2 className="text-shadow" style={{ color: 'var(--color-text-primary)' }}>
                    <Text id="invalid_replay" />
                </h2>
                <Link href="/" className="text-shadow" style={{ color: 'var(--color-text-primary)' }}>
                    <Text id="back_to_home" />
                </Link>
            </div>
        );
    }

    return (
        <div data-game-container className="game-container">
            <WoolBackgroundLayer />
            <ReplayServerProvider gameId={REPLAY_KEY} initialGame={result.initial}>
                <FernFractalProvider>
                    <AnimationProvider>
                        <GameProvider>
                            <DragProvider>
                                <ReplayStage
                                    fool={result.fool}
                                    code={result.code}
                                    frames={result.frames}
                                    reverses={result.reverses}
                                    gameId={gameId}
                                    names={result.names}
                                    times={result.times}
                                />
                            </DragProvider>
                        </GameProvider>
                    </AnimationProvider>
                </FernFractalProvider>
            </ReplayServerProvider>
        </div>
    );
};
