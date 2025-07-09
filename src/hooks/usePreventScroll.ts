import { useEffect } from "react";

export const usePreventScroll = () => {
  // Prevent page scrolling/dragging during touch interactions but allow legitimate drags
  useEffect(() => {
    const preventPageScroll = (e: TouchEvent) => {
      const target = e.target as HTMLElement;

      // Allow dragging on draggable elements (cards)
      if (target.closest('[draggable="true"]')) {
        return; // Don't prevent - allow legitimate drag
      }

      // Allow interactions on interactive elements (buttons, inputs, or anything with higher z-index)
      if (target.tagName === 'BUTTON' ||
        target.tagName === 'INPUT' ||
        target.closest('input') ||
        window.getComputedStyle(target).zIndex === '1000') {
        return; // Don't prevent - allow button clicks and input focus
      }

      // Prevent page scrolling/panning on background/text areas
      if (e.touches.length > 1 || (e.touches.length === 1 && e.type === 'touchmove')) {
        e.preventDefault();
      }
    };

    // Only prevent touchmove to avoid interfering with clicks/taps
    document.addEventListener("touchmove", preventPageScroll, { passive: false });

    // Cleanup function to remove event listeners
    return () => {
      document.removeEventListener("touchmove", preventPageScroll);
    };
  }, []);
}