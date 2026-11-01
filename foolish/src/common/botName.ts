// Bot names: the prefix that reserves them, and the city each one renders as.
//
// Replay codes encode only the player NAME, not the is_ai flag, so to recover
// "bot vs human" from a stored replay code alone the name itself must carry the
// signal. Bots are named with a reserved leading prefix that humans are forbidden
// from using (enforced client-side here and, authoritatively, by the server's
// sign-up, c_base/src/auth/auth.c). This reserves the
// whole namespace, so future/unreleased bots are covered with no per-name list.
//
// The prefix is a single-byte ASCII char ('%') — names are stored UTF-8 in the
// game_snapshots.extras blob, so a 1-byte prefix beats a 4-byte emoji on every
// bot seat of every stored game. '%' has no case, so it survives the signup
// uppercase-normalization unchanged.
import { BotRegistry } from '@sdk/ts/gen/bot_registry.ts';
import { useLocalization } from '../contexts/LocalizationContext';
import type { StringId } from './strings';

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


// Bots render with an icon, so drop the prefix for display, and name the city in
// the reader's language.
//
// A seeded bot is STORED by its city, in English, as its server account's
// nickname: "%<city> <n>" (server/src/store/bot_seed.c; the promo page's local
// bot, c/src/local_table.c, takes the same form). The city is a row of the
// kernel's bot registry (c/src/bots.c, generated as sdk/ts/gen/bot_registry.ts),
// and that row's key names its translation in c/i18n (`bot.<key>`: Miami is
// Майами in Russian). So the stored name stays the one language-neutral fact a
// replay blob carries, and the page reads it in whatever language it is in.
// (Until 2026-11-01 nothing mapped: a Russian reader saw "Miami 1".)
//
// A name whose city no row has - an old replay's '%Octogen 1', a bot renamed
// since - is shown as stored, minus the prefix: that game really was played by a
// bot of that name.
const CITY_ROW = /^(.+) (\d+)$/;
export const botDisplayName = (name: string, t: (id: StringId) => string): string => {
    if (!isBotName(name)) return name;
    const bare = name.slice(BOT_USERNAME_PREFIX.length);
    const m = CITY_ROW.exec(bare);
    const row = m ? BotRegistry.find((r) => r.city === m[1] && r.key) : undefined;
    return m && row ? `${t(`bot.${row.key}` as StringId)} ${m[2]}` : bare;
};

/** botDisplayName in the page's language, for a component. */
export const useBotName = (): ((name: string) => string) => {
    const { t } = useLocalization();
    return (name) => botDisplayName(name, t);
};
