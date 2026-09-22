// Bot names: the prefix that reserves them, and the city each one renders as.
//
// Replay codes encode only the player NAME, not the is_ai flag, so to recover
// "bot vs human" from a stored replay code alone the name itself must carry the
// signal. Bots are named with a reserved leading prefix that humans are forbidden
// from using (enforced client-side here and, authoritatively, by a BEFORE
// INSERT/UPDATE trigger on auth.users; see server/impls/supabase/seed.sql). This reserves the
// whole namespace, so future/unreleased bots are covered with no per-name list.
//
// The prefix is a single-byte ASCII char ('%') — names are stored UTF-8 in the
// game_snapshots.extras blob, so a 1-byte prefix beats a 4-byte emoji on every
// bot seat of every stored game. '%' has no case, so it survives the signup
// uppercase-normalization unchanged.
const BOT_USERNAME_PREFIX = '%';


// Humans may not put the reserved prefix ANYWHERE in their name (stricter than
// "must not start with it" — there is no legitimate human use of it, and this
// closes any impersonation/confusion path).
export const usernameUsesReservedPrefix = (name: string | null | undefined): boolean =>
    !!name && name.includes(BOT_USERNAME_PREFIX);

// A stored name (bots.nickname, replay-extras seat name) belongs to a bot iff
// it carries the prefix — sound because of the reservation above.
export const isBotName = (name: string): boolean =>
    name.startsWith(BOT_USERNAME_PREFIX);


// ---- the road to Moscow (docs/IOS_BOT_NAMING.md) -----------------------------
//
// THE ROSTER IS NAMED AFTER EXPLOSIVES EVERYWHERE IT IS STORED - the C strategy
// keys, bots.nickname, the names inside a replay blob, the wire - and renders as
// a city whose distance to Moscow falls as the bot gets stronger. Seven rungs:
// Miami, New York, Seoul, Madrid, Vienna, St. Petersburg, Moscow. The site seats
// six of them, seven bots deep each - Seoul (robusta) is an offline-only rung
// and is not seeded, so a full eight-seat table is you plus seven of one city.
//
// The cities are NOT in this file. They are `bot.<strategy key>` in c/i18n, the
// same twenty-five-language table the phone app reads (ios/FoolishApp/PhoneOnly/
// BotNames.swift), so the two products cannot drift and a city is spelled the
// reader's way in every locale - Moskau, Moscou, モスクワ.
//
// RENDER TIME, NEVER STORAGE TIME. Nothing here may reach a stored or
// server-bound field: replay codes stay byte-identical across locales, DB rows
// never carry a localized string, and switching the site's language re-renders
// every seat. Never reverse-map a display name back to a name the server knows.
import { isStringId, type StringId } from '../localization/strings';

/** What a render passes in: `t` from useLocalization(). */
type Translate = (id: StringId, params?: Record<string, string>) => string;

// A seeded nickname's base IS its strategy key with the underscores written as
// spaces ("Simple Heuristic" <- simple_heuristic), so the key is derived rather
// than tabulated: the roster is one table, in c/src/bot_roster.c, and this file
// keeps no second copy of it to fall behind.
const strategyKey = (base: string): string => base.toLowerCase().split(/ +/).join('_');

// One retired family renders as another rung's city. `octogen` IS `semtex` plus
// a wider exact-solve window (c/OCTOGEN.md) and replaced it before either was
// ever seeded beside the other, so a pre-July-2026 replay blob's "% Semtex 2"
// reads as Moscow - the one explosive that could otherwise still reach a page,
// since no live row has carried the key for a year.
const RETIRED: Record<string, string> = { semtex: 'octogen' };

/** A stored name as a page should show it: a human's unchanged, a bot's as its
 *  city, the reserved prefix gone (a ⚙ icon marks the seat instead).
 *
 *  The stored form is `% <Base> [Max] [<n>]`, and the tail survives the rename:
 *  `% Octogen 2` -> `Moscow 2`, `% Cordite Max 1` -> `St. Petersburg Max 1`
 *  (the Max tiers are retired but still sit in old replay blobs). A `% 0x…` name
 *  is left verbatim: the hex bot is no longer seeded, but old replay blobs carry
 *  it embedded at encode time. A base with no city in
 *  c/i18n renders as stored: that is every offline-only rung the site cannot
 *  seat, which is why the roster gate in e2e/validation watches the seeded set. */
export const botDisplayName = (stored: string, t: Translate): string => {
    if (!isBotName(stored)) return stored;
    const body = stored.slice(BOT_USERNAME_PREFIX.length).trim();
    if (body.toLowerCase().startsWith('0x')) return body;

    const parts = body.split(/ +/);
    const index = /^\d+$/.test(parts[parts.length - 1] ?? '') ? parts.pop()! : '';
    const max = (parts[parts.length - 1] ?? '').toLowerCase() === 'max';
    if (max) parts.pop();

    const key = strategyKey(parts.join(' '));
    const id = `bot.${RETIRED[key] ?? key}`;
    if (!isStringId(id)) return body;
    return [t(id), max ? t('bot.max') : '', index].filter(Boolean).join(' ');
};
