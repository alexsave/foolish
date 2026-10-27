// words.ts - what the page says about a game's state and a rating's speed (the numbers are the
// kernel's, game.h and speed.h).
import {
  CC_ABORTED, CC_BLITZ, CC_BULLET, CC_CLASSICAL, CC_RAPID, CC_DRAW_AGREED, CC_DRAW_FIFTY, CC_DRAW_FLAG_MATERIAL, CC_DRAW_MATERIAL, CC_DRAW_MAX_PLY, CC_DRAW_REPETITION,
  CC_FLAG, CC_MATE, CC_PLAYING, CC_RESIGN, CC_STALEMATE,
} from '@/gen/web_layout';

export function resultLine(status: number, winner: number): string {
  const who = winner === 0 ? 'White' : 'Black';
  switch (status) {
    case CC_PLAYING: return '';
    case CC_MATE: return `Checkmate. ${who} wins`;
    case CC_RESIGN: return `${winner === 0 ? 'Black' : 'White'} resigned. ${who} wins`;
    case CC_FLAG: return `${winner === 0 ? 'Black' : 'White'} ran out of time. ${who} wins`;
    case CC_STALEMATE: return 'Stalemate. Draw';
    case CC_DRAW_AGREED: return 'Draw by agreement';
    case CC_DRAW_REPETITION: return 'Draw by threefold repetition';
    case CC_DRAW_FIFTY: return 'Draw by the fifty-move rule';
    case CC_DRAW_MATERIAL: return 'Draw: insufficient material';
    case CC_DRAW_FLAG_MATERIAL: return 'Time out, but no mating material. Draw';
    case CC_DRAW_MAX_PLY: return 'Draw: the game reached 600 plies';
    case CC_ABORTED: return 'Game aborted';
    default: return '';
  }
}

export function score(status: number, winner: number): string {
  if (status === CC_PLAYING || status === CC_ABORTED) return '';
  return winner === 0 ? '1-0' : winner === 1 ? '0-1' : '½-½';
}

/** The speeds in the kernel's order (speed.h), and what the page calls them. */
export const SPEEDS: readonly number[] = [CC_BULLET, CC_BLITZ, CC_RAPID, CC_CLASSICAL];
export const SPEED_NAME: Readonly<Record<number, string>> = { [CC_BULLET]: 'Bullet', [CC_BLITZ]: 'Blitz', [CC_RAPID]: 'Rapid', [CC_CLASSICAL]: 'Classical' };
