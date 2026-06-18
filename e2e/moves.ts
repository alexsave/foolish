// Legal-move enumeration for driving games. Mirrors the validators in
// supabase/functions/_shared/actions/* so the harness fires moves a real client
// could legitimately send (the point is to stress the orchestration, not to feed
// the well-tested handlers illegal input).

import { Game, Card, PLAYER_STATUS, GAME_STATUS } from '../supabase/functions/_shared/types.ts';
import { canCover, get_next_player_index, no_cards_left } from '../supabase/functions/_shared/common_utils.ts';

export type Move =
  | { type: 'attack'; player_id: string; cards: Card[] }
  | { type: 'cover'; player_id: string; cover_cards: Card[]; attack_cards: Card[] }
  | { type: 'pass'; player_id: string; cards: Card[] }
  | { type: 'pickup'; player_id: string }
  | { type: 'good'; player_id: string };

const sameVal = (cards: Card[], v: number) => cards.filter((c) => c.value === v);

export const legalMoves = (game: Game): Move[] => {
  if (game.status !== GAME_STATUS.PLAYING) return [];
  const moves: Move[] = [];
  const defenderIdx = game.defender;
  const defender = game.players[defenderIdx];
  const uncovered = game.table_battles.filter((b) => b.defense === null);

  // ---- Defender moves ----
  if (defender && defender.status === PLAYER_STATUS.IN) {
    // cover: cover one uncovered attack with one legal card
    for (const battle of uncovered) {
      for (const card of defender.hand) {
        if (canCover(battle.attack, card, game.power_suit)) {
          moves.push({ type: 'cover', player_id: defender.player_id, cover_cards: [card], attack_cards: [battle.attack] });
        }
      }
    }
    // pickup
    if (game.table_battles.length > 0) moves.push({ type: 'pickup', player_id: defender.player_id });
    // pass: no cover present, all table attacks same value, defender has that value,
    // next player has capacity
    if (game.table_battles.length > 0 && !game.table_battles.some((b) => b.defense !== null)) {
      const tableVal = game.table_battles[0].attack.value;
      if (game.table_battles.every((b) => b.attack.value === tableVal)) {
        const matching = sameVal(defender.hand, tableVal);
        if (matching.length > 0) {
          const nextIdx = get_next_player_index(game, game.defender);
          const next = game.players[nextIdx];
          // pass 1 card (simplest legal pass)
          if (next.hand.length >= 1 + game.table_battles.length) {
            moves.push({ type: 'pass', player_id: defender.player_id, cards: [matching[0]] });
          }
        }
      }
    }
  }

  // ---- Attacker moves ----
  const isFirstAttack = game.table_battles.length === 0;
  for (let i = 0; i < game.players.length; i++) {
    if (i === defenderIdx) continue;
    const p = game.players[i];
    if (p.status !== PLAYER_STATUS.IN) continue;

    if (isFirstAttack) {
      if (i === game.first_attacker) {
        // first attacker plays one card (any) — defender always has >=1 capacity at round start
        for (const card of p.hand) {
          if (defender.hand.length >= uncovered.length + 1) {
            moves.push({ type: 'attack', player_id: p.player_id, cards: [card] });
          }
        }
      }
      // non-first-attacker can't act until first attack lands (and can't `good` on empty table)
    } else {
      // subsequent attack: any card whose value already appears on the table
      const tableVals = new Set<number>();
      for (const b of game.table_battles) {
        tableVals.add(b.attack.value);
        if (b.defense) tableVals.add(b.defense.value);
      }
      for (const card of p.hand) {
        if (tableVals.has(card.value) && defender.hand.length >= uncovered.length + 1) {
          moves.push({ type: 'attack', player_id: p.player_id, cards: [card] });
        }
      }
      // good (concede the round): allowed for non-defender IN players not already good
      if (!game.good_players.includes(p.player_id)) {
        moves.push({ type: 'good', player_id: p.player_id });
      }
    }
  }
  return moves;
};

export const noCardsLeft = no_cards_left;
