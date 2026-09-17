// Split out of src/components/ReplayScreen.tsx (docs/C_GAME_SHAPE_MIGRATION.md
// Phase 9 step 4). A PURE MOVE: not a character of the markup or the rules
// changed, which is what the 16 DOM goldens and the 21 animation traces prove.

import { botDisplayName } from '../../common/botName';

const seatName = (seat: number, names?: (string | null)[] | null) =>
    botDisplayName(names?.[seat] || `P${seat + 1}`);

export { seatName };
