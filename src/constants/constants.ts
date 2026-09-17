export const WEBSITE_DOMAIN = 'foolish.cards';
export const ANIMATION_TIME = 500; // milliseconds
// How long a refused move waits for the pushes it was refused over before the page
// loads the game (ServerContext reconcileAfter): two flights, time for a late push.
export const RECONCILE_GRACE_MS = 2 * ANIMATION_TIME;
