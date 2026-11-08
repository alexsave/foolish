// The sound tuner's local endpoint (tools/soundgen/tune.c, `npm run tune`), in
// development only. One place for its address, for the tuner (SoundTuner.tsx,
// tunerPlay.ts) and for the engine's development table (cueAudio.ts).

export const TUNE_URL = 'http://127.0.0.1:3101';

/** The take table soundgen last wrote, as bytes (GET /table), or null when the
 *  endpoint is not running or has written none since it started. */
export const fetchTable = async (): Promise<Uint8Array | null> => {
    try {
        const r = await fetch(`${TUNE_URL}/table`, { cache: 'no-store' });
        return r.ok ? new Uint8Array(await r.arrayBuffer()) : null;
    } catch {
        return null;
    }
};

/** A table's bytes as a short version string (FNV-1a), so a file fetched under it
 *  is never one a cache kept from another table. */
export const tableVersion = (bytes: Uint8Array): string => {
    let h = 0x811c9dc5;
    for (let i = 0; i < bytes.length; i++) h = Math.imul(h ^ bytes[i], 0x01000193);
    return (h >>> 0).toString(36);
};
