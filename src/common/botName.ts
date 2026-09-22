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


// Bots render with an icon, so drop the prefix for display.
//
// NO MAPPING LIVES HERE, and that is the design (owner, 2026-09-22). The roster
// is named after cities where it is STORED - bots.nickname is '%Moscow 1', the
// name a replay blob embeds is '%Moscow 1' - so a name needs no translating on
// the way to a page. What a replay code says is what the page shows, including
// an old code that says '%Octogen 1': those games were played by a bot with that
// name and they render it.
//
// The cost, stated where someone will look for it: a stored name cannot be
// localized, so bot names read the same in every language. The `bot.*` strings
// in c/i18n stay for the phone's OFFLINE picker, which names a seat from its
// strategy key rather than from a stored row.
export const botDisplayName = (name: string): string =>
    isBotName(name) ? name.slice(BOT_USERNAME_PREFIX.length) : name;
