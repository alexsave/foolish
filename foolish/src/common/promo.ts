// The promo codes (c/promo/promo_codes.h, generated): a path segment that names
// one opens a game against a bot in this tab, in the code's language.
import { PromoCodes } from '@sdk/ts/gen/promo_codes.ts';

export type Promo = (typeof PromoCodes)[number];

/** The promo a path segment names, in any case (/br, /BR, /Br), or null. */
export const promoOf = (segment: string): Promo | null =>
    PromoCodes.find((p) => p.code === segment.toLowerCase()) ?? null;
