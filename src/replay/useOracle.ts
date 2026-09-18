/* =============================================================================
 * The Infinite Oracle, as the replay screen holds it (docs/INFINITE_ORACLE_DESIGN.md)
 * =============================================================================
 * A client-side octogen deliberation over the PAUSED decision: strengths stream
 * in and sharpen. Analysis only arms once the animation settles on a step that
 * is a decision at all; the fleet stays warm across steps and is torn down on
 * unmount. StrictMode-safe (start/stop bump a run generation inside the
 * controller).
 * ========================================================================== */

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useAnimation } from '../contexts/AnimationContext';
import { createOracleController, IOracleController } from '../oracle/oracleControllerFactory';
import { buildOracleJob, findDecisionIndex } from '../oracle/replayOracleInput';
import { OracleSnapshot } from '../oracle/types';
import type { ReplayFrame } from './frames';

/** The Oracle's panel, and what the screen needs to draw its button. */
export interface Oracle {
    open: boolean;
    setOpen: (next: boolean | ((o: boolean) => boolean)) => void;
    snapshot: OracleSnapshot | null;
    /** The decision this step is, or null when the step decides nothing. */
    decision: number | null;
    toggleMemory: () => void;
    /** Re-run the deliberation for the step on screen. */
    retry: () => void;
}

export function useOracle(
    frames: ReplayFrame[],
    code: Uint8Array,
    stepIdx: number,
    playing: boolean,
    gameId: string,
): Oracle {
    const { isAnimating } = useAnimation();
    const oracleRef = useRef<IOracleController | null>(null);
    const [oracleOpen, setOracleOpen] = useState(false);
    const [oracleMemory, setOracleMemory] = useState(true);
    const [oracleSnap, setOracleSnap] = useState<OracleSnapshot | null>(null);
    const oracleDecision = useMemo(() => findDecisionIndex(frames, stepIdx), [frames, stepIdx]);
    const getOracle = useCallback(() => {
        if (!oracleRef.current) oracleRef.current = createOracleController();
        return oracleRef.current;
    }, []);
    useEffect(() => {
        if (!oracleOpen) return;
        return getOracle().subscribe(setOracleSnap);
    }, [oracleOpen, getOracle]);
    useEffect(() => {
        if (!oracleOpen) return;
        const ctrl = getOracle();
        if (playing || isAnimating) { ctrl.stopCurrent(); return; }
        const job = buildOracleJob(frames, code, stepIdx, oracleMemory, gameId);
        if (!job) { setOracleSnap(null); return; }
        void ctrl.start(job);
        return () => ctrl.stopCurrent();
    }, [oracleOpen, stepIdx, isAnimating, playing, oracleMemory, code, frames, gameId, getOracle]);
    useEffect(() => () => { oracleRef.current?.dispose(); oracleRef.current = null; }, []);

    const toggleMemory = useCallback(() => setOracleMemory((m) => !m), []);
    const retry = useCallback(() => {
        const j = buildOracleJob(frames, code, stepIdx, oracleMemory, gameId);
        if (j) void getOracle().start(j);
    }, [frames, code, stepIdx, oracleMemory, gameId, getOracle]);

    return { open: oracleOpen, setOpen: setOracleOpen, snapshot: oracleSnap, decision: oracleDecision, toggleMemory, retry };
}
