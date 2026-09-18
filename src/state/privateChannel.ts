import type { RealtimeChannel } from '@supabase/supabase-js';
import supabase from '../backend/Connector';

/**
 * Holds one PRIVATE Realtime channel open for as long as the caller wants it,
 * rejoining it when a join is refused, times out or is dropped.
 *
 * Why a refused join is expensive, and why this removes the channel at once:
 * Realtime decides a private join in milliseconds but sends its refusal about
 * five seconds later, and it answers nothing else on that websocket in the
 * meantime. Every channel the page holds shares the one socket, so each refused
 * join stalls every other join and every broadcast on it for those five
 * seconds (measured on the local stack: a spectator's game- pushes arrived up
 * to 5 s late, and a join queued behind two refusals timed out). realtime-js
 * also rejoins an errored channel by itself (1 s, 2 s, 5 s, then every 10 s)
 * for as long as the channel object lives, so a channel that is refused and
 * left alone stalls the socket again and again, forever.
 *
 * So a refused channel is removed before anything else happens (which stops
 * realtime-js's own rejoin), and the rejoin is this loop's alone, backing off
 * from RETRY_FIRST_MS to RETRY_MAX_MS. Callers should also not ask for a
 * channel the policy will refuse (a spectator's chat:, for one).
 */
export interface PrivateChannelHandlers {
    /** Adds the channel's listeners before it subscribes. */
    bind: (channel: RealtimeChannel) => void;
    /**
     * The channel is joined. `missed` is true when anything could have been
     * broadcast while it was not (a rejoin, or a first join after refusals):
     * Realtime has no catch-up.
     */
    onJoined?: (missed: boolean) => void;
    /** A join did not get through; the loop retries after `retryInMs`. */
    onFailed?: (status: string, error: unknown, retryInMs: number) => void;
}

export const RETRY_FIRST_MS = 500;
export const RETRY_MAX_MS = 10_000;

/** Joins `topic` privately and keeps it joined; the returned function leaves it for good. */
export function holdPrivateChannel(topic: string, handlers: PrivateChannelHandlers): () => void {
    let stopped = false;
    let channel: RealtimeChannel | null = null;
    let retryTimer: ReturnType<typeof setTimeout> | null = null;
    let retryMs = RETRY_FIRST_MS;
    let everJoined = false;
    let missed = false;
    // Bumped per attempt, so a late status from a removed channel is ignored.
    let attempt = 0;

    const drop = (ch: RealtimeChannel | null) => {
        if (!ch) return;
        supabase.removeChannel(ch).catch(() => { /* the socket may already be closed */ });
    };

    const fail = (status: string, error: unknown) => {
        missed = true;
        const failed = channel;
        channel = null;
        // Before anything else: realtime-js would otherwise rejoin it on its own.
        drop(failed);
        if (stopped) return;
        const wait = retryMs;
        retryMs = Math.min(retryMs * 2, RETRY_MAX_MS);
        handlers.onFailed?.(status, error, wait);
        if (retryTimer) clearTimeout(retryTimer);
        retryTimer = setTimeout(() => { retryTimer = null; join().catch((e) => fail('ERROR', e)); }, wait);
    };

    const join = async () => {
        if (stopped) return;
        const mine = ++attempt;
        await supabase.realtime.setAuth();
        if (stopped || mine !== attempt) return;
        const ch = supabase.channel(topic, { config: { private: true } });
        channel = ch;
        handlers.bind(ch);
        ch.subscribe((status, err) => {
            if (stopped || mine !== attempt) return;
            if (status === 'SUBSCRIBED') {
                retryMs = RETRY_FIRST_MS;
                const wasMissed = everJoined || missed;
                everJoined = true;
                missed = false;
                handlers.onJoined?.(wasMissed);
            } else if (status === 'CHANNEL_ERROR' || status === 'TIMED_OUT' || status === 'CLOSED') {
                attempt++;
                fail(status, err);
            }
        });
    };

    join().catch((e) => fail('ERROR', e));

    return () => {
        stopped = true;
        attempt++;
        if (retryTimer) clearTimeout(retryTimer);
        retryTimer = null;
        const ch = channel;
        channel = null;
        drop(ch);
    };
}
