// The site does not scroll. Cards do.
//
// A Durak board is a fixed screen you drag things around on, not a document you
// pan. On a phone the browser's default touch gestures fight that constantly:
// a drag that starts on a card slides the page instead, a two-finger rest
// zooms, and the address bar shows and hides as the viewport moves. This hook
// takes those gestures away, document-wide, on every primary surface (Lobby,
// Tutorial, GameDisplay, the replay stage).
//
// The listeners are NON-PASSIVE, which is the whole point: `passive: false` is
// what makes preventDefault legal on touchstart and touchmove. A passive
// listener here would be inert.
//
// TWO EXEMPTIONS, AND ONLY TWO:
//
//   [draggable="true"]      a card. Its touches belong to the drag layer, and
//                           refusing any of them stops the card moving. This is
//                           the exemption the hook exists to protect.
//   [data-chat-scrollable]  a real scrolling region, which needs the default
//                           pan it is asking for.
//
// `closest` rather than an equality test because a touch lands on whatever the
// card rendered inside itself - a pip, a suit glyph - not on the card's own
// element.
//
// WHAT THIS REPLACED, AND WHY THE SHAPE CHANGED. The original wrote the three
// handlers out longhand and gave each one a third branch for "interactive"
// elements - [data-touch-interactive], BUTTON, INPUT - meaning to treat them
// more gently than the board. It did not: in all three handlers that branch
// reached the same verdict as the default one directly below it (touchstart and
// touchend refused only multi-touch either way; touchmove refused everything
// either way), so the ladder cost three lookups per touch and decided nothing.
// Dropping it is not a behaviour change, and e2e/validation/
// prevent_scroll_validation.test.ts was written against the ORIGINAL and passed
// against it before this file was replaced, so that claim is checked rather
// than asserted. If buttons ever do need gentler treatment than the board, that
// is a new rule to add here deliberately, not a branch to resurrect.
//
// The original also wrapped every handler body and the effect itself in
// `try { ... } catch (error) {}` - four bare swallows, which were every bare
// swallow in src/. They are gone. Nothing in a handler can throw: `closest`
// only throws on an invalid selector and these are literals, and `preventDefault`
// on a cancelable event cannot. The effect-level one was worse than useless:
// if the second addEventListener had thrown, the first would have stayed
// attached and no cleanup would have been returned, leaving a document-level
// non-passive touchmove refusing gestures for the life of the tab.
import { useEffect } from 'react';

/** A card or a scroll region: the browser's own gesture wins here. */
const isExempt = (target: EventTarget | null): boolean =>
    target instanceof Element
    && !!(target.closest('[draggable="true"]') || target.closest('[data-chat-scrollable]'));

export const usePreventScroll = () => {
    useEffect(() => {
        // touchstart and touchend refuse only MULTI-touch: a single tap has to
        // reach buttons, inputs and the board itself, so refusing it would make
        // the site unusable, while two fingers means a pinch-zoom nobody wants.
        const pinch = (e: TouchEvent) => {
            const points = e.type === 'touchend' ? e.changedTouches : e.touches;
            if (points.length > 1 && !isExempt(e.target)) e.preventDefault();
        };

        // touchmove refuses EVERYTHING outside the two exemptions, one finger
        // included. This is the line that stops the page sliding under a drag.
        const pan = (e: TouchEvent) => {
            if (!isExempt(e.target)) e.preventDefault();
        };

        const opts = { passive: false } as const;
        document.addEventListener('touchstart', pinch, opts);
        document.addEventListener('touchmove', pan, opts);
        document.addEventListener('touchend', pinch, opts);

        return () => {
            document.removeEventListener('touchstart', pinch);
            document.removeEventListener('touchmove', pan);
            document.removeEventListener('touchend', pinch);
        };
    }, []);
};
