# Hand-order bugs: crazy swaps, duplicates, and cards in hand + on table

Three reported symptoms, one family of causes — all client-side display. The
**server never** puts a card in two places: the card-conservation invariant
(`checkCards`) has held across tens of thousands of committed moves (0
violations), and a card in both hand and table would register as a duplicate.
So these were purely how the client maintained the displayed hand.

## Root causes

1. **Crazy swaps.** The displayed order is a local preference kept in
   `localHandOrders` and reconciled with the server hand by `mergeHandOrder`,
   which **appends any card not currently in the local order to the END**. The
   optimistic action handlers (attack/pass/cover) removed a played card from the
   local order after `ANIMATION_TIME` *unconditionally* — even when the server
   then **rejected** the move (stale view, CAS contention, illegal, double-tap).
   The rejected card returned via the next authoritative hand, was no longer in
   the local order, and so got appended at the end: a card the player had placed
   in the middle teleported to the right edge. `hand_order.ts` reproduces it
   (Q♣ jumps from index 2 to the end; two rejects scramble the hand).

2. **Duplicate cards in hand / cards in hand *and* on the table.** The displayed
   hand was maintained incrementally and separately from the authoritative hand,
   so any desync (overlapping optimistic updates, stale `setTimeout` closures,
   out-of-order broadcasts) could leave a card listed in the hand that the
   authoritative state had moved to the table or removed — or list it twice.

## The fix

The rendered hand is now a **pure function of the authoritative hand** plus a
**sticky arrangement memory**:

- `displayedHand(memory, authHand)` — the rendered hand is the authoritative
  `self.hand`, **deduplicated** and ordered by the memory. A card that isn't in
  the authoritative hand (played, or on the table) is **never shown**; duplicates
  are impossible by construction.
- `reconcileHandMemory` — the arrangement memory only **grows** (new cards
  appended) and never forgets a slot on a transient absence, so an
  optimistically-played-then-rejected card returns to its **original slot**.
- The optimistic `setLocalHandOrders` mutations in attack/pass/pickup/cover were
  removed; the displayed hand derives the optimistic removal/addition from
  `self.hand` (which those handlers already update), so there's nothing to get
  out of sync.
- Drag-reorder (`setLocalHandOrder`) updates the memory stickily.

Net: the displayed hand is always a deduplicated reordering of exactly the
authoritative hand — no swaps, no duplicates, no in-hand-and-on-table — while the
preferred order stays entirely client-side (rebuilt cheaply after the rare
refresh, exactly as desired).

## Validation

- `tests/stress/hand_order.ts` — reproduces the swap with the old `mergeHandOrder`
  and proves the new `displayedHand` + `reconcileHandMemory`: rejected card
  returns to its slot, a duplicated memory entry renders once, a table card is
  never shown in the hand. PASS.
- Card conservation re-run: **0** violations over ~1,900 committed moves
  (verifies the server never produces hand+table / duplicates; the client now
  can't either).
- `tsc --noEmit`: clean.
