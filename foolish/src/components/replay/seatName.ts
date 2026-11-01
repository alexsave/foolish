// Split out of src/components/ReplayScreen.tsx (docs/C_GAME_SHAPE_MIGRATION.md
// Phase 9 step 4). A PURE MOVE: not a character of the markup or the rules
// changed, which is what the 16 DOM goldens and the 21 animation traces prove.

// A replay's seat names are the names the blob stored, shown through
// botDisplayName (src/common/botName.ts): a bot's city in the reader's language.
const seatName = (seat: number, names: (string | null)[] | null | undefined, botName: (name: string) => string) =>
    botName(names?.[seat] || `P${seat + 1}`);

export { seatName };
