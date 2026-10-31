// The promo codes (c/promo/promo_codes.h, generated): a path segment that names
// one opens a game against a bot in this tab, in the code's language.
import { PromoCodes } from '@sdk/ts/gen/promo_codes.ts';
import { PromoLangDefault } from '@sdk/ts/gen/promo_lang_default.ts';

/** A code as the page plays it: every field said, the language defaulted. */
export interface Promo { code: string; src: string; lang: string }

/** The promo a path segment names, in any case (/br, /BR, /Br), or null. */
export const promoOf = (segment: string): Promo | null => {
    const p = PromoCodes.find((r) => r.code === segment.toLowerCase());
    return p ? { code: p.code, src: p.src, lang: p.lang ?? PromoLangDefault[0] } : null;
};

/** The kernel link the page at the address bar needs: promo.wasm for a promo code (c/Makefile), else web.wasm. */
export const kernelLinkForAddress = (): 'web' | 'promo' =>
    promoOf(decodeURIComponent(window.location.pathname.split('/')[1] ?? '')) ? 'promo' : 'web';
