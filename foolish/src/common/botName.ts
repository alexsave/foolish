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
import { kernelBotNickname } from '@sdk/ts/wasm/bots.ts';
import { BOT_NICK_CITY } from '@sdk/ts/gen/view_layout.bots.ts';
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
// the reader's language. Which city a stored name names, and which account, is
// the kernel's (c/src/bot_roster.h bot_nickname_read); the city's words are
// c/i18n's `bot.<key>`, which this says. Until 2026-11-01 nothing mapped, by
// the 2026-09-22 rule, and a Russian reader saw "Miami 1"; Alex asked for the
// city in the reader's language on 2026-10-31.
export const botDisplayName = (name: string, t: (id: StringId) => string): string => {
    const n = kernelBotNickname(name);
    return n.kind === BOT_NICK_CITY ? `${t(`bot.${n.key}` as StringId)} ${n.instance}` : n.show;
};

/** botDisplayName in the page's language, for a component. */
export const useBotName = (): ((name: string) => string) => {
    const { t } = useLocalization();
    return (name) => botDisplayName(name, t);
};
