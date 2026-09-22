// Split out of src/components/ReplayScreen.tsx (docs/C_GAME_SHAPE_MIGRATION.md
// Phase 9 step 4). A PURE MOVE: not a character of the markup or the rules
// changed, which is what the 16 DOM goldens and the 21 animation traces prove.

import { botDisplayName } from '../../common/botName';
import type { StringId } from '../../localization/strings';

// `t` comes from the calling component's useLocalization(): a replay's seat
// names are stored names, and a bot's renders as its city (src/common/botName.ts).
const seatName = (seat: number, names: (string | null)[] | null | undefined,
    t: (id: StringId, params?: Record<string, string>) => string) =>
    botDisplayName(names?.[seat] || `P${seat + 1}`, t);

export { seatName };
