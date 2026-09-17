import { useEffect, useRef } from 'react';
import { useParams } from 'next/navigation';
import { useAuth } from '../contexts/AuthContext';
import { useServer } from '../contexts/ServerContext';
import { animationFeed } from './animationFeed';
import { holdPrivateChannel } from './privateChannel';

/**
 * The LIVE game's producer for the animation feed: subscribes to the per-user
 * supabase broadcast channel and republishes every animation_events payload
 * into animationFeed, where AnimationProvider consumes it.
 *
 * This is the channel-subscription half of what used to live inside
 * AnimationProvider — extracted so the provider itself is transport-agnostic
 * and the replay screen can feed it synthesized sequences instead. Mounted in
 * ProtectedRoute (needs auth + a live game id); renders nothing.
 */
export const RealtimeAnimationFeed = () => {
    const { user_id } = useAuth();
    const { loadGame, views } = useServer();
    const url_game_id = useParams<{ game_id: string }>().game_id?.toLowerCase();
    // Only a seated player has a gu- stream. Realtime admits the private join
    // only for a member of the game, so a spectator's join is refused every
    // time (and each refusal stalls the whole socket, see privateChannel.ts);
    // spectators get the game-<id> stream from ServerContext instead.
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
        // A join can be refused while the membership row is not yet visible to
        // Realtime (opening a game you are joining, or one whose create is still
        // persisting); holdPrivateChannel removes a refused channel and rejoins.
        return holdPrivateChannel(`gu-${url_game_id}-${user_id}`, {
            bind: (channel) => {
                channel.on('broadcast', { event: 'animation_events' }, (payload) => {
                    // Attach the game id this channel is subscribed for: the
                    // packed envelope ({t:'as3', s, v, b}) carries no JS game
                    // state, so the consumer needs it to pick the decode roster.
                    animationFeed.publish({ ...payload.payload, game_id: url_game_id });
                });
            },
            onJoined: (missed) => {
                // Broadcasts sent while we were not subscribed are lost - realtime
                // has no catch-up. After a rejoin, or a first join that followed
                // refused joins, refetch authoritative state so the client can't
                // be left showing a stale / mixed-bout table.
                if (missed) loadGameRef.current(url_game_id).catch(console.error);
            },
            onFailed: (status, _err, retryInMs) => {
                console.log(`animation channel ${status}, retrying in ${retryInMs}ms`);
            },
        });
    }, [user_id, url_game_id, seated]);

    return null;
};
