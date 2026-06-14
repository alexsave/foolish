// Bot username reservation.
//
// Replay codes encode only the player NAME, not the is_ai flag, so to recover
// "bot vs human" from a stored replay code alone the name itself must carry the
// signal. Bots are named with a reserved leading prefix — the robot emoji — that
// humans are forbidden from using (enforced client-side here and, authoritatively,
// by a BEFORE INSERT/UPDATE trigger on auth.users; see supabase/seed.sql). This
// reserves the whole namespace, so future/unreleased bots are covered with no
// per-name list. Symbols have no case, so the prefix survives the signup
// uppercase-normalization unchanged.
export const BOT_USERNAME_PREFIX = '\u{1F916}'; // 🤖 U+1F916 ROBOT FACE

// A name belongs to a bot iff it starts with the reserved prefix. Single source
// of truth for the codec/analysis pipeline and any display logic.
export const isBotUsername = (name: string | null | undefined): boolean =>
    !!name && name.startsWith(BOT_USERNAME_PREFIX);

// Humans may not put the reserved prefix ANYWHERE in their name (stricter than
// "must not start with it" — there is no legitimate human use of it, and this
// closes any impersonation/confusion path).
export const usernameUsesReservedPrefix = (name: string | null | undefined): boolean =>
    !!name && name.includes(BOT_USERNAME_PREFIX);
