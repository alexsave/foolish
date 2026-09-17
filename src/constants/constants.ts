import { ANIM_TIME_MS } from '@sdk/ts/gen/anim.bots.ts';

export const WEBSITE_DOMAIN = 'foolish.cards';
// How long a refused move waits for the pushes it was refused over before the page
// loads the game (ServerContext reconcileAfter): two flights, time for a late push.
//
// DERIVED FROM THE KERNEL'S FLIGHT, not from a TypeScript copy of it. This file
// used to hold `ANIMATION_TIME = 500` and every duration, gap and deadline in
// the product was that one number; the flight itself is the kernel's
// (c/src/anim_plan.h ANIM_TIME_MS, generated into sdk/ts/gen/anim.bots.ts), so
// the coupling this comment relies on cannot silently come apart.
export const RECONCILE_GRACE_MS = 2 * ANIM_TIME_MS;
