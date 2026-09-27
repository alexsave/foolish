/* =============================================================================
 * Infinite Oracle - engine selection (docs/INFINITE_ORACLE_DESIGN.md §8b.2/§8b.8)
 * Both controllers share one public surface, so the overlay + ReplayScreen are
 * mode-agnostic. Mode B (shared-memory threads) runs only where the page is
 * cross-origin isolated (SharedArrayBuffer available); everywhere else it falls
 * back to the Mode A instance fleet - automatically, with no UI difference.
 *
 * Cross-origin isolation is NOT free: it needs COOP/COEP response headers on
 * every document of this origin. Until they are sent, oracleMode() returns 'A'
 * on the deployed site and Mode B is dead code there - see
 * docs/INFINITE_ORACLE_MODE_B.md for what turning it on costs.
 * ========================================================================== */

import { OracleController } from './OracleController';
import { OracleModeBController } from './OracleModeBController';
import { OracleJob, OracleSnapshot } from './types';

export interface IOracleController {
    subscribe(cb: (s: OracleSnapshot) => void): () => void;
    start(job: OracleJob): Promise<void>;
    stopCurrent(): void;
    dispose(): void;
}

/** 'B' when shared-memory threads can run here, else 'A'. */
export function oracleMode(): 'A' | 'B' {
    const isolated = typeof globalThis !== 'undefined'
        && typeof SharedArrayBuffer !== 'undefined'
        && (globalThis as { crossOriginIsolated?: boolean }).crossOriginIsolated === true;
    return isolated ? 'B' : 'A';
}

export function createOracleController(): IOracleController {
    const mode = oracleMode();
    if (typeof window !== 'undefined') (window as { __oracleMode?: string }).__oracleMode = mode;
    return mode === 'B' ? new OracleModeBController() : new OracleController();
}
