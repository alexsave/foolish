// words.ts - what the page says about a game's state and a rating's speed (the numbers are the
// kernel's, game.h and speed.h).
import {
  CC_ARENA_FINISHED, CC_ARENA_RUNNING, CC_ARENA_UPCOMING, CW_E_ARENA_GUEST, CW_E_ARENA_JOINED, CW_E_ARENA_NAME, CW_E_ARENA_NONE,
  CW_E_ARENA_NOT_JOINED, CW_E_ARENA_OVER, CW_E_ARENA_TOO_MANY, CC_ABORTED, CC_BLITZ, CC_BULLET, CC_CLASSICAL, CC_RAPID, CC_DRAW_AGREED, CC_DRAW_FIFTY, CC_DRAW_FLAG_MATERIAL, CC_DRAW_MATERIAL, CC_DRAW_MAX_PLY, CC_DRAW_REPETITION,
  CC_FLAG, CC_MATE, CC_PLAYING, CC_RESIGN, CC_STALEMATE,
  CC_CHAT_CONTROL, CC_CHAT_EMPTY, CC_CHAT_LONG, CC_CHAT_UTF8, CV_CHAT_BANNED,
  CW_E_CHAT_FAST, CW_E_CHAT_GUEST, CW_E_CHAT_MUTED, CW_E_CHAT_ROOM, CW_E_CHAT_TEXT, CW_E_CHAT_WORDS,
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

/** An arena's status (arena.h), and why a request about one was refused (wire.h CW_E_ARENA_..). */
export const ARENA_STATUS: Readonly<Record<number, string>> = { [CC_ARENA_UPCOMING]: 'Upcoming', [CC_ARENA_RUNNING]: 'Running', [CC_ARENA_FINISHED]: 'Finished' };
export const ARENA_REFUSAL: Readonly<Record<number, string>> = {
  [CW_E_ARENA_NONE]: 'There is no such arena.',
  [CW_E_ARENA_OVER]: 'This arena is over.',
  [CW_E_ARENA_JOINED]: 'You are in it already.',
  [CW_E_ARENA_NOT_JOINED]: 'You are not in it.',
  [CW_E_ARENA_GUEST]: 'Sign in to make an arena.',
  [CW_E_ARENA_TOO_MANY]: 'You have four upcoming arenas already.',
  [CW_E_ARENA_NAME]: 'That name will not do: 3 to 30 letters, digits, spaces, - and _.',
};

/** "4:05", "1:02:09": a countdown of ms (none below zero). */
export function countdown(ms: number): string {
  const t = Math.max(0, Math.ceil(ms / 1000)), h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), s = t % 60;
  const ss = String(s).padStart(2, '0');
  return h ? `${h}:${String(m).padStart(2, '0')}:${ss}` : `${m}:${ss}`;
}

/** "in 12 min", "in 1 h 5 min", "now" */
export function inWords(ms: number): string {
  const m = Math.round(ms / 60000);
  if (m <= 0) return 'now';
  return m < 60 ? `in ${m} min` : `in ${Math.floor(m / 60)} h${m % 60 ? ` ${m % 60} min` : ''}`;
}

/** A clock time from ms since the epoch, in the reader's zone: "14:00". */
export const clockTime = (at: number): string => new Date(at).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });

/** Why a chat line will not go: the kernel's answer before it is sent (cv_chat_check: chat.h's
 * reasons and a banned word), and the server's refusal after (wire.h CW_E_CHAT_..). */
export const CHAT_CHECK: Readonly<Record<number, string>> = {
  [CC_CHAT_EMPTY]: '',
  [CC_CHAT_LONG]: 'Too long: 140 bytes at most.',
  [CC_CHAT_UTF8]: 'That line holds a character chat does not take.',
  [CC_CHAT_CONTROL]: 'That line holds a character chat does not take.',
  [CV_CHAT_BANNED]: 'Keep it friendly: that word is not allowed.',
};
export function chatRefusal(code: number, why: number, wait: number): string {
  switch (code) {
    case CW_E_CHAT_ROOM: return 'This chat has closed.';
    case CW_E_CHAT_TEXT: return CHAT_CHECK[why] || 'That line will not do.';
    case CW_E_CHAT_GUEST: return 'Sign in to chat.';
    case CW_E_CHAT_FAST: return wait <= 1 ? 'Slow down: one line a second.' : `Slow down: wait ${wait} seconds.`;
    case CW_E_CHAT_WORDS: return CHAT_CHECK[CV_CHAT_BANNED];
    case CW_E_CHAT_MUTED: {
      const m = Math.ceil(wait / 60);
      return `You are muted for ${m === 1 ? 'a minute' : `${m} more minutes`}.`;
    }
    default: return 'The line did not go. Try again.';
  }
}
