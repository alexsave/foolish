import { useEffect, useRef } from 'react';
import { useParams } from 'next/navigation';
import supabase from '../backend/Connector';
import { useAuth } from '../contexts/AuthContext';
import { useServerActions } from '../contexts/ServerContext';
import { animationFeed } from './animationFeed';

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
    const { loadGame } = useServerActions();
    const url_game_id = useParams<{ game_id: string }>().game_id?.toLowerCase();

    // Keep loadGame reachable from inside the subscription callback without
    // re-running the effect when its identity changes.
    const loadGameRef = useRef(loadGame);
    loadGameRef.current = loadGame;

    // Store channel reference for proper cleanup
    const gameUserChannelRef = useRef<any>(null);

    // Simple retry interval for animation channel
    const animationChannelRetryInterval = useRef(500); // Start with 0.5 seconds
    const MAX_RETRY_INTERVAL = 5000; // Cap at 5 seconds

    useEffect(() => {
        if (!user_id || !url_game_id) {
            return;
        }

        // Track if this effect instance is still mounted
        let isMounted = true;
        let isSubscribing = false;
        let hasEverConnected = false;
        let retryTimeoutId: NodeJS.Timeout | null = null;

        const subscribeToGameAnimations = async () => {
            // Prevent multiple simultaneous subscription attempts
            if (isSubscribing || !isMounted) {
                return;
            }

            isSubscribing = true;

            try {
                // Clean up any existing channel first
                if (gameUserChannelRef.current) {
                    await supabase.removeChannel(gameUserChannelRef.current);
                    gameUserChannelRef.current = null;
                }

                // Check if we're still mounted after async operation
                if (!isMounted) {
                    isSubscribing = false;
                    return;
                }

                // Ensure we have proper auth before subscribing
                await supabase.realtime.setAuth();

                if (!isMounted) {
                    isSubscribing = false;
                    return;
                }

                // Subscribe to personalized game-user channel for game updates
                const gameUserChannel = supabase.channel(`gu-${url_game_id}-${user_id}`, {
                    config: { private: true }
                });

                // Store the channel reference
                gameUserChannelRef.current = gameUserChannel;

                gameUserChannel
                    .on('broadcast', { event: 'animation_events' }, (payload) => {
                        animationFeed.publish(payload.payload);
                    })
                    .subscribe((status, err) => {
                        if (status === 'SUBSCRIBED') {
                            animationChannelRetryInterval.current = 500; // Reset retry interval on success
                            isSubscribing = false;
                            const wasReconnect = hasEverConnected;
                            hasEverConnected = true;
                            // Broadcasts sent while we were disconnected are lost —
                            // realtime has no catch-up. After a RE-subscribe, refetch
                            // authoritative state so the client can't be left showing a
                            // stale / mixed-bout table.
                            if (wasReconnect && url_game_id) {
                                loadGameRef.current(url_game_id).catch(console.error);
                            }
                        } else if (status === 'CHANNEL_ERROR' || status === 'TIMED_OUT') {
                            // Only retry on actual errors, not on CLOSED
                            console.log('connection error: ' + status + ', retrying in ', animationChannelRetryInterval.current, 'ms');
                            isSubscribing = false;

                            if (!isMounted) {
                                return;
                            }

                            retryTimeoutId = setTimeout(() => {
                                if (isMounted) {
                                    subscribeToGameAnimations().catch(console.error);
                                }
                            }, animationChannelRetryInterval.current);
                        } else if (status === 'CLOSED') {
                            // Channel closed - only log if we had a successful connection
                            if (hasEverConnected) {
                            } else {
                                console.log('channel closed before connecting, retrying in', animationChannelRetryInterval.current, 'ms');
                                isSubscribing = false;

                                if (!isMounted) {
                                    return;
                                }

                                retryTimeoutId = setTimeout(() => {
                                    if (isMounted) {
                                        subscribeToGameAnimations().catch(console.error);
                                    }
                                }, animationChannelRetryInterval.current);
                            }
                        }
                    });
            } catch (error) {
                console.error('Error setting up game animation subscription:', error);
                isSubscribing = false;

                if (!isMounted) {
                    return;
                }

                retryTimeoutId = setTimeout(() => {
                    if (isMounted) {
                        subscribeToGameAnimations().catch(console.error);
                        // Double the interval but cap at MAX_RETRY_INTERVAL
                        animationChannelRetryInterval.current = Math.min(animationChannelRetryInterval.current * 2, MAX_RETRY_INTERVAL);
                    }
                }, animationChannelRetryInterval.current);
            }
        };

        subscribeToGameAnimations();

        // Cleanup function
        return () => {
            isMounted = false;

            // Clear any pending retry
            if (retryTimeoutId) {
                clearTimeout(retryTimeoutId);
            }

            if (gameUserChannelRef.current) {
                const channelToRemove = gameUserChannelRef.current;
                gameUserChannelRef.current = null;

                // Remove immediately, no timeout
                supabase.removeChannel(channelToRemove).catch(error => {
                    // Ignore cleanup errors - channel might already be closed
                    console.debug('Channel cleanup error (expected if WebSocket closed):', error);
                });
            }
        };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [user_id, url_game_id]);

    return null;
};
