// The product's domain, in the QR code the lobby shows and in the synthetic
// address an anonymous account is registered under.
//
// The only thing left in this file. It used to hold `ANIMATION_TIME = 500`, from
// which every duration, gap and deadline in the product was derived; Phase 9
// moved the timing into the kernel (c/src/anim_plan.h, generated into
// sdk/ts/gen/anim.bots.ts) and RECONCILE_GRACE_MS, the last thing built on it,
// now lives beside its one caller in ServerContext.tsx.
//
// It stays a module of its own because e2e/lobby_add_bot.test.ts mocks this
// exact path (`mock.module('../src/constants/constants.ts', ...)`) to point the
// QR code at example.com.
export const WEBSITE_DOMAIN = 'foolish.cards';
