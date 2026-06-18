// Reproduce the "crazy card swaps" in the live hand.
//
// The displayed hand order is a local React preference (localHandOrders), kept in
// sync with the server hand by mergeHandOrder (ServerContext.tsx:309-342):
//   preserved = local cards still present in the new hand (in LOCAL order)
//   new       = cards in the new hand not in local (appended at the END)
//
// That append-at-end is the bug. The optimistic action handlers
// (ServerContext attack/pass/cover) remove a played card from the local order
// after ANIMATION_TIME *unconditionally*. If the server then REJECTS the move
// (stale view, CAS exhaustion, illegal, double-tap…), the card returns via the
// next authoritative hand — and because it's no longer in the local order,
// mergeHandOrder treats it as new and drops it at the END. A card the player had
// carefully placed in the middle suddenly jumps to the right edge.
//
//   npx tsx tests/stress/hand_order.ts   (exit 0 = pass/expected)

type Card = { suit: number; value: number };
const k = (c: Card) => `${c.suit}-${c.value}`;
const show = (h: Card[]) => h.map(k).join(' ');

// VERBATIM mergeHandOrder from ServerContext.tsx
const mergeHandOrder = (oldHand: Card[], newHand: Card[]): Card[] => {
  if (!oldHand || !newHand) return newHand || [];
  const oldCardPositions = new Map<string, number>();
  oldHand.forEach((card, index) => oldCardPositions.set(k(card), index));
  const newCardSet = new Set(newHand.map((c) => k(c)));
  const preservedCards: Card[] = [];
  oldHand.forEach((card) => { if (newCardSet.has(k(card))) preservedCards.push(card); });
  const newCards = newHand.filter((c) => !oldCardPositions.has(k(c)));
  return [...preservedCards, ...newCards];
};

let fail = 0;
const check = (cond: boolean, label: string) => { console.log(`  ${cond ? 'ok  ' : 'BUG '} ${label}`); if (!cond) fail++; };

const C = (s: number, v: number): Card => ({ suit: s, value: v });
// a player's arranged hand: 6♠ 10♥ Q♣ 7♦ A♠  (their preferred order)
const arranged = [C(0, 5), C(1, 9), C(2, 11), C(3, 6), C(0, 13)];

console.log('=== hand-order "crazy swap" repro ===');

// Baseline: normal play (a card is really gone) keeps the rest in place.
{
  const local = arranged;
  const afterPlay = local.filter((c) => k(c) !== k(C(2, 11))); // played Q♣
  const serverHand = afterPlay; // server agrees
  const merged = mergeHandOrder(afterPlay, serverHand);
  check(show(merged) === show(afterPlay), `baseline: playing Q♣ leaves order ${show(merged)}`);
}

// BUG: optimistic play of the MIDDLE card Q♣, then the server REJECTS it, so the
// card returns. The optimistic handler already removed it from the local order;
// the authoritative hand still has it → mergeHandOrder re-appends it at the END.
{
  const localAfterOptimisticRemove = arranged.filter((c) => k(c) !== k(C(2, 11))); // Q♣ removed locally
  const authoritativeHand = arranged; // server rejected → Q♣ still in hand (original membership)
  const merged = mergeHandOrder(localAfterOptimisticRemove, authoritativeHand);
  const qcIndex = merged.findIndex((c) => k(c) === k(C(2, 11)));
  console.log(`     arranged:   ${show(arranged)}`);
  console.log(`     after merge: ${show(merged)}`);
  check(qcIndex === merged.length - 1 && show(merged) !== show(arranged),
    `rejected Q♣ teleports from index 2 to the END (index ${qcIndex}) — the crazy swap`);
}

// It compounds: every rejected/transiently-missing card stacks up at the end, so
// repeated rejections scramble the arrangement.
{
  let local = arranged.slice();
  // optimistically remove two middle cards (e.g. two quick taps), both rejected
  local = local.filter((c) => k(c) !== k(C(1, 9)) && k(c) !== k(C(2, 11)));
  const merged = mergeHandOrder(local, arranged);
  check(show(merged) !== show(arranged),
    `two rejected cards reorder the hand: ${show(merged)} (was ${show(arranged)})`);
}

console.log(`\nReproduced the swap: mergeHandOrder appends returning cards at the end instead of restoring their slot.\n`);

// ===========================================================================
// THE FIX: a STICKY arrangement memory + an authoritative-only displayed hand.
//  - reconcileMemory: arrangement memory only GROWS (new cards appended); it
//    never drops a card on transient absence, so a slot survives a rejected play.
//  - displayedHand: the rendered hand is the authoritative hand, deduped and
//    ordered by the memory. Cards not in the authoritative hand (played, or on
//    the table) are never shown; duplicates are impossible by construction.
// ===========================================================================
const reconcileMemory = (memory: Card[], authHand: Card[]): Card[] => {
  const seen = new Set<string>();
  const dedupMem = memory.filter((c) => { const key = k(c); if (seen.has(key)) return false; seen.add(key); return true; });
  const additions = authHand.filter((c) => !seen.has(k(c)));
  return [...dedupMem, ...additions];
};
const displayedHand = (memory: Card[], authHand: Card[]): Card[] => {
  const authByKey = new Map(authHand.map((c) => [k(c), c]));
  const used = new Set<string>();
  const out: Card[] = [];
  for (const m of memory) { const key = k(m); if (authByKey.has(key) && !used.has(key)) { out.push(authByKey.get(key)!); used.add(key); } }
  for (const c of authHand) { const key = k(c); if (!used.has(key)) { out.push(c); used.add(key); } } // safety: any auth card not in memory
  return out;
};

let fixFail = 0;
const fcheck = (cond: boolean, label: string) => { console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${label}`); if (!cond) fixFail++; };

console.log('=== fix validation ===');

// No swap: optimistic play of Q♣ (gone from auth), then REJECTED (Q♣ back in auth).
{
  let memory = reconcileMemory([], arranged);               // memory learns arrangement
  const authWhilePlaying = arranged.filter((c) => k(c) !== k(C(2, 11)));
  memory = reconcileMemory(memory, authWhilePlaying);        // sticky: Q♣ stays in memory
  const displayWhilePlaying = displayedHand(memory, authWhilePlaying);
  fcheck(!displayWhilePlaying.some((c) => k(c) === k(C(2, 11))), 'optimistic: Q♣ hidden while in flight');
  // rejected -> Q♣ returns to authoritative hand
  memory = reconcileMemory(memory, arranged);
  const displayAfter = displayedHand(memory, arranged);
  fcheck(show(displayAfter) === show(arranged), `rejected Q♣ returns to its ORIGINAL slot: ${show(displayAfter)}`);
}

// No duplicates: even if the memory somehow contains a dup, display is deduped.
{
  const memory = [...arranged, C(2, 11), C(2, 11)]; // corrupted memory with dup Q♣
  const display = displayedHand(memory, arranged);
  fcheck(new Set(display.map(k)).size === display.length, 'duplicate in memory does not duplicate in the rendered hand');
}

// No in-hand-and-on-table: a card that's on the table (absent from auth hand) is
// never rendered in the hand, even if memory still lists it.
{
  const onTable = C(2, 11); // Q♣ is on the table now
  const authHand = arranged.filter((c) => k(c) !== k(onTable));
  const memory = arranged; // memory still has Q♣
  const display = displayedHand(memory, authHand);
  fcheck(!display.some((c) => k(c) === k(onTable)), 'card on the table is never shown in the hand');
}

console.log(fixFail === 0 ? '\nfix validation: PASS' : `\nfix validation: FAIL (${fixFail})`);
process.exit(fixFail === 0 ? 0 : 1);
