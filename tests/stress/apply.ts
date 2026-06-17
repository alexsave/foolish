// Dispatch a Move to the REAL action handler, producing the same {game, events}
// an edge-function operation closure returns. The handlers are unchanged
// production code; we only route to them.

import { Game, AnimationEvent } from '../../supabase/functions/_shared/types.ts';
import { handleAttack } from '../../supabase/functions/_shared/actions/attack.ts';
import { handleCover } from '../../supabase/functions/_shared/actions/cover.ts';
import { handlePass } from '../../supabase/functions/_shared/actions/pass.ts';
import { handlePickup } from '../../supabase/functions/_shared/actions/pickup.ts';
import { handleGood } from '../../supabase/functions/_shared/actions/good.ts';
import { Move } from './moves.ts';

export const applyMove = (game: Game, move: Move): { game: Game; events: AnimationEvent[] } => {
  let events: AnimationEvent[];
  switch (move.type) {
    case 'attack': events = handleAttack(game, move.player_id, move.cards); break;
    case 'cover': events = handleCover(game, move.player_id, move.cover_cards, move.attack_cards); break;
    case 'pass': events = handlePass(game, move.player_id, move.cards); break;
    case 'pickup': events = handlePickup(game, move.player_id); break;
    case 'good': events = handleGood(game, move.player_id); break;
    default: throw new Error(`unknown move`);
  }
  return { game, events };
};
