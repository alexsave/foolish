// The order the lobby's bot picker walks: up the ladder, weakest to strongest,
// and within a rung by its instance number - Miami 1 … Miami 7, New York 1 …
// Moscow 7. The road to Moscow, in the order the road runs.
//
// It used to be `.order('created_at', { ascending: false })`, which is the order
// seed.sql happens to INSERT, backwards: the picker opened on Moscow 7 and
// walked out through the families in reverse insert order. Nothing about a row's
// age is a property of the bot.
//
// THE LADDER ORDER IS THE KERNEL'S. `tier` in c/src/bot_roster.c is the strength
// order (docs/IOS_BOT_NAMING.md), and this file takes it as an argument rather
// than keeping a second copy of it - the same reason src/common/botName.ts
// derives a strategy key instead of tabulating one. Pass `tierOf` from
// kernelBotRoster(); the comparator itself is pure, so a test can hand it any
// ladder it likes.

/** The instance number a stored nickname ends with: `%Cordite 3` -> 3.
 *  A name with no trailing number sorts after the numbered ones in its rung,
 *  which is where `0x00C0FFEE` sat before it left the site. */
export const instanceOf = (stored: string): number => {
    const m = /\s(\d+)$/.exec(stored.trim());
    return m ? Number(m[1]) : Number.MAX_SAFE_INTEGER;
};

/** Rows the picker can add, in ladder order. `tierOf` returns the kernel's tier
 *  for a strategy key; a key it does not know sorts last rather than wedging
 *  into the middle of the ladder, so an unmapped bot is visibly at the end. */
export function sortBotsByLadder<T extends { nickname: string; strategy_key: string }>(
    rows: readonly T[],
    tierOf: (strategyKey: string) => number | undefined,
): T[] {
    const tier = (r: T) => tierOf(r.strategy_key) ?? Number.MAX_SAFE_INTEGER;
    return [...rows].sort((a, b) =>
        tier(a) - tier(b)
        || instanceOf(a.nickname) - instanceOf(b.nickname)
        // Same rung and same number, or neither numbered: the stored name, so
        // the order is at least stable from one load to the next.
        || a.nickname.localeCompare(b.nickname));
}
