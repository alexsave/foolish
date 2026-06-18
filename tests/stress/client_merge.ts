// VERBATIM port of the client's state-merge logic from
// src/contexts/ServerContext.tsx (mergeHandOrder / mergeTableBattles /
// mergeGameData, lines ~309-416), with only the React/UI side-effects removed
// (updateLocalHandOrder, which only affects visual hand ordering, not
// correctness). This lets the harness drive the EXACT reconciliation a live
// client performs when it applies a broadcast snapshot, so any glitch we surface
// is the client's real behaviour, not an approximation.

export interface Battle { attack: { suit: number; value: number }; defense: { suit: number; value: number } | null; }

export const mergeHandOrder = (oldHand: any[], newHand: any[]): any[] => {
  if (!oldHand || !newHand) return newHand || [];
  const oldCardPositions = new Map<string, number>();
  oldHand.forEach((card, index) => oldCardPositions.set(`${card.suit}-${card.value}`, index));
  const newCardSet = new Set(newHand.map((card) => `${card.suit}-${card.value}`));
  const preservedCards: any[] = [];
  oldHand.forEach((card) => {
    if (newCardSet.has(`${card.suit}-${card.value}`)) preservedCards.push(card);
  });
  const newCards = newHand.filter((card) => !oldCardPositions.has(`${card.suit}-${card.value}`));
  return [...preservedCards, ...newCards];
};

// The offender. Keeps incoming battles, then APPENDS any existing battle whose
// attack-card key isn't present in incoming — intended to preserve optimistic
// attacks during out-of-order responses, but it has no notion of which bout a
// battle belongs to.
export const mergeTableBattles = (existingBattles: any[], incomingBattles: any[]): any[] => {
  if (!existingBattles || existingBattles.length === 0) return incomingBattles || [];
  if (!incomingBattles) return existingBattles;
  // If incoming is empty, it's a table clear (pickup/good) - trust server completely
  if (incomingBattles.length === 0) return [];

  const incomingByKey = new Map(incomingBattles.map((b) => [`${b.attack.suit}-${b.attack.value}`, b]));
  const result = [...incomingBattles];
  for (const battle of existingBattles) {
    const key = `${battle.attack.suit}-${battle.attack.value}`;
    if (!incomingByKey.has(key)) result.push(battle);
  }
  return result;
};

// mergeGameData, reduced to the fields that bear on card correctness (table +
// self hand). prev = the client's current game object; incoming = the snapshot
// being applied.
export const mergeGameData = (prev: any, incoming: any): any => {
  const result = {
    ...incoming,
    self: incoming.hasOwnProperty('self') ? incoming.self : prev?.self,
  };
  if (incoming.table_battles && prev?.table_battles) {
    result.table_battles = mergeTableBattles(prev.table_battles, incoming.table_battles);
  }
  if (incoming.self && prev?.self && incoming.self.hand && prev.self.hand) {
    result.self = { ...incoming.self, hand: mergeHandOrder(prev.self.hand, incoming.self.hand) };
  }
  return result;
};
