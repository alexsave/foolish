import { useEffect } from "react";

export const usePreventScroll = () => {
    useEffect(() => {
        try {
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
            }
        };

            // Add event listeners with passive: false to allow preventDefault
            document.addEventListener("touchstart", handleTouchStart, { passive: false });
            document.addEventListener("touchmove", handleTouchMove, { passive: false });
            document.addEventListener("touchend", handleTouchEnd, { passive: false });

            // Cleanup function
            return () => {
                    document.removeEventListener("touchstart", handleTouchStart);
                    document.removeEventListener("touchmove", handleTouchMove);
                    document.removeEventListener("touchend", handleTouchEnd);
            };
        } catch (error) {
        }
    }, []);
}