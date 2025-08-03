import { useEffect } from "react";
import { errorLogger } from "../utils/errorLogger";

export const usePreventScroll = () => {
    useEffect(() => {
        try {
            errorLogger.logCustomError('usePreventScroll - Hook Initialize', new Error('Initializing touch event handlers'), {
                userAgent: navigator.userAgent,
                touchPoints: navigator.maxTouchPoints,
                platform: navigator.platform,
            });
        // Handle touchstart events to prevent unwanted gestures
        const handleTouchStart = (e: TouchEvent) => {
            try {
                const target = e.target as HTMLElement;

                // Allow multi-touch on draggable elements (for potential drag gestures)
                if (target.closest('[draggable="true"]')) {
                    return;
                }

                // Allow multi-touch on chat scrollable areas (though we'll limit to pan-y via CSS)
                if (target.closest('[data-chat-scrollable]')) {
                    return;
                }

                // Allow single touch on interactive elements
                if (target.closest('[data-touch-interactive]') ||
                    target.tagName === 'BUTTON' ||
                    target.tagName === 'INPUT' ||
                    target.closest('input')) {
                    // But prevent multi-touch (pinch-to-zoom) on interactive elements
                    if (e.touches.length > 1) {
                        e.preventDefault();
                    }
                    return;
                }

                // For all other areas, prevent multi-touch gestures
                if (e.touches.length > 1) {
                    e.preventDefault();
                }
            } catch (error) {
                errorLogger.logCustomError('usePreventScroll - TouchStart Error', error as Error, {
                    touchCount: e.touches?.length,
                    targetTag: (e.target as HTMLElement)?.tagName,
                });
            }
        };

        // Handle touchmove events to prevent unwanted scrolling/dragging
        const handleTouchMove = (e: TouchEvent) => {
            try {
                const target = e.target as HTMLElement;

                // Allow dragging on draggable elements (cards)
                if (target.closest('[draggable="true"]')) {
                    return;
                }

                // Allow scrolling within chat area
                if (target.closest('[data-chat-scrollable]')) {
                    return;
                }

                // Allow limited interaction on interactive elements (taps, not drags)
                if (target.closest('[data-touch-interactive]') ||
                    target.tagName === 'BUTTON' ||
                    target.tagName === 'INPUT' ||
                    target.closest('input')) {
                    // Prevent dragging on interactive elements
                    e.preventDefault();
                    return;
                }

                // Prevent all other touch movements (background scrolling/panning)
                e.preventDefault();
            } catch (error) {
                errorLogger.logCustomError('usePreventScroll - TouchMove Error', error as Error, {
                    touchCount: e.touches?.length,
                    targetTag: (e.target as HTMLElement)?.tagName,
                });
            }
        };

        // Handle touchend to clean up any zoom gestures
        const handleTouchEnd = (e: TouchEvent) => {
            try {
                const target = e.target as HTMLElement;

                // Allow normal touch end on draggable elements
                if (target.closest('[draggable="true"]')) {
                    return;
                }

                // Allow normal touch end on chat scrollable areas
                if (target.closest('[data-chat-scrollable]')) {
                    return;
                }

                // For multi-touch on non-draggable elements, prevent zoom
                if (e.changedTouches.length > 1) {
                    e.preventDefault();
                }
            } catch (error) {
                errorLogger.logCustomError('usePreventScroll - TouchEnd Error', error as Error, {
                    changedTouchCount: e.changedTouches?.length,
                    targetTag: (e.target as HTMLElement)?.tagName,
                });
            }
        };

            // Add event listeners with passive: false to allow preventDefault
            document.addEventListener("touchstart", handleTouchStart, { passive: false });
            document.addEventListener("touchmove", handleTouchMove, { passive: false });
            document.addEventListener("touchend", handleTouchEnd, { passive: false });

            errorLogger.logCustomError('usePreventScroll - Listeners Added', new Error('Touch event listeners attached'), {
                listenerCount: 3,
            });

            // Cleanup function
            return () => {
                try {
                    document.removeEventListener("touchstart", handleTouchStart);
                    document.removeEventListener("touchmove", handleTouchMove);
                    document.removeEventListener("touchend", handleTouchEnd);

                    errorLogger.logCustomError('usePreventScroll - Cleanup', new Error('Touch event listeners removed'), {
                        listenerCount: 3,
                    });
                } catch (error) {
                    errorLogger.logCustomError('usePreventScroll - Cleanup Error', error as Error);
                }
            };
        } catch (error) {
            errorLogger.logCustomError('usePreventScroll - Hook Setup Error', error as Error);
        }
    }, []);
}