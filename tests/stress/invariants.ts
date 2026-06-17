// Core-correctness invariants for Durak, checked against the durably-committed
// DB state (so they reflect what the server actually persisted, not in-memory
// optimism).

import { Game, Card } from '../../supabase/functions/_shared/types.ts';
import { Delivery } from './orchestrator.ts';

export const expectedTotalCards = (numPlayers: number): number => (numPlayers > 4 ? 52 : 36);

const key = (c: Card) => `${c.suit}:${c.value}`;

export interface CardCheck {
  ok: boolean;
  total: number;
  expected: number;
  live: number;
  discard: number;
  duplicates: string[];
  cardBacks: number;
  detail: string;
}

// Every physical card is in exactly one place: deck, flipped, a hand, the table
// (as an attack or a defense), or the discard pile. The live (non-discard) cards
// must all be distinct, and live + discard must equal the full deck size.
export const checkCards = (game: Game): CardCheck => {
  const live: Card[] = [];
  for (const c of game.deck) live.push(c);
  if (game.flipped) live.push(game.flipped);
  for (const p of game.players) for (const c of p.hand) live.push(c);
  for (const b of game.table_battles) {
    live.push(b.attack);
    if (b.defense) live.push(b.defense);
  }

  const seen = new Map<string, number>();
  let cardBacks = 0;
  for (const c of live) {
    if (c.suit === -1 || c.value === -1) cardBacks++;
    seen.set(key(c), (seen.get(key(c)) ?? 0) + 1);
  }
  const duplicates = [...seen.entries()].filter(([, n]) => n > 1).map(([k, n]) => `${k}×${n}`);

  const expected = expectedTotalCards(game.players.length);
  const total = live.length + game.discard_pile_length;
  const ok = total === expected && duplicates.length === 0 && cardBacks === 0;
  return {
    ok, total, expected, live: live.length, discard: game.discard_pile_length,
    duplicates, cardBacks,
    detail: `total=${total} (expected ${expected}), live=${live.length}, discard=${game.discard_pile_length}`
      + (duplicates.length ? `, DUPLICATES=[${duplicates.join(',')}]` : '')
      + (cardBacks ? `, CARD_BACKS=${cardBacks}` : ''),
  };
};

// Per-client broadcast-ordering analysis. A "version regression" means a client
// received an animation sequence emitted at a committed version OLDER than one it
// already received — i.e. the client would animate cards moving to a state it has
// already moved past (a visible glitch / rubber-band).
export interface BroadcastReport {
  totalDeliveries: number;
  regressions: { recipient: string; prevVer: number; gotVer: number; reqId: string }[];
}

export const analyzeBroadcasts = (perClient: Map<string, Delivery[]>): BroadcastReport => {
  const regressions: BroadcastReport['regressions'] = [];
  let totalDeliveries = 0;
  for (const [recipient, deliveries] of perClient) {
    const inArrivalOrder = [...deliveries].sort((a, b) => a.arriveSeq - b.arriveSeq);
    let maxVer = -1;
    for (const d of inArrivalOrder) {
      totalDeliveries++;
      if (d.committedVersion < maxVer) {
        regressions.push({ recipient, prevVer: maxVer, gotVer: d.committedVersion, reqId: d.reqId });
      }
      maxVer = Math.max(maxVer, d.committedVersion);
    }
  }
  return { totalDeliveries, regressions };
};
