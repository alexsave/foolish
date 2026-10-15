import { useEffect, useRef } from 'react';
import { useUrlGameId } from '../utils/useUrlGameId';
import { useAuth } from '../contexts/AuthContext';
import { useServer } from '../contexts/ServerContext';
import { animationFeed } from './animationFeed';
import { stream } from '../common/backend';

/**
 * The LIVE game's producer for the animation feed: holds the seated player's
 * own cast stream of the game (backend.ts) and republishes every cast into
 * animationFeed, where AnimationProvider consumes it.
 *
 * This is the channel-subscription half of what used to live inside
 * AnimationProvider — extracted so the provider itself is transport-agnostic
 * and the replay screen can feed it synthesized sequences instead. Mounted in
 * ProtectedRoute (needs auth + a live game id); renders nothing.
 */
export const RealtimeAnimationFeed = () => {
    const { user_id } = useAuth();
    const { loadGame, views } = useServer();
    const url_game_id = useUrlGameId()?.toLowerCase();
    // Only a seated player hears their own seat's casts; spectators get the
    // spectators' stream from ServerContext instead.
    // The board the signed-in user holds names their seat (mySeat, -1 for a
    // spectator). A boolean: this renders on every state change, but the
    // subscription effect re-runs only when the seat itself comes or goes.
    const seated = !!user_id && !!url_game_id && (views[url_game_id]?.mySeat ?? -1) >= 0;

    // Keep loadGame reachable from inside the subscription callback without
    // re-running the effect when its identity changes.
    const loadGameRef = useRef(loadGame);
    loadGameRef.current = loadGame;

    useEffect(() => {
        if (!user_id || !url_game_id || !seated) {
            return;
        }
        // The cast is the kernel's (evwire.h); attach the game id the stream
        // names, so the consumer can pick the identity to decode it against.
        // Casts sent while the stream was not held are not replayed: after a
        // rejoin (the socket was reopened), or a first join that followed
        // refusals (a membership the server did not see yet), refetch
        // authoritative state so the table is never stale or mixed-bout. A
        // first join that was admitted at once missed nothing.
        let missed = false;
        let joined = false;
        const unlisten = stream.listen({
            cast: (gid, cast) => { if (gid === url_game_id) animationFeed.publish({ cast, game_id: url_game_id }); },
            refused: (gid) => { if (gid === url_game_id) missed = true; },
            joined: (gid) => {
                if (gid !== url_game_id) return;
                if (joined || missed) loadGameRef.current(url_game_id).catch(console.error);
                joined = true;
                missed = false;
            },
        });
        const release = stream.game(url_game_id);
        return () => { unlisten(); release(); };
    }, [user_id, url_game_id, seated]);

    return null;
};
