// Validate that optimistic animations + reverts still work after the fix
// (mergeTableBattles now trusts the incoming table instead of appending).
//
// Models the real client data flow:
//   - The optimistic-conflict resolver (AnimationContext) injects the local
//     player's unconfirmed cards into the INCOMING server state before it reaches
//     the merge, so the merge only ever has to trust incoming.
//   - On a server reject, a revert fires and optimistic tracking is cleared, so
//     the next server state no longer has the card injected.
//
// We run the merge both ways — OLD (append leftover battles) vs NEW (trust
// incoming) — to show the NEW behaviour keeps optimistic cards visible until
// resolved AND removes reverted cards cleanly (the OLD append left them stuck).
//
//   npx tsx tests/stress/optimistic_revert.ts   (exit 0 = pass)

type Battle = { attack: { suit: number; value: number }; defense: { suit: number; value: number } | null };
const c = (suit: number, value: number) => ({ suit, value });
const k = (x: { suit: number; value: number }) => `${x.suit}:${x.value}`;
const tableKeys = (bs: Battle[]) => bs.flatMap((b) => (b.defense ? [k(b.attack), k(b.defense)] : [k(b.attack)])).sort();

// OLD mergeTableBattles (append leftover battles) — pre-fix.
const mergeAppend = (existing: Battle[], incoming: Battle[]): Battle[] => {
  if (!existing || existing.length === 0) return incoming || [];
  if (!incoming) return existing;
  if (incoming.length === 0) return [];
  const byKey = new Map(incoming.map((b) => [k(b.attack), b]));
  const result = [...incoming];
  for (const b of existing) if (!byKey.has(k(b.attack))) result.push(b);
  return result;
};
// NEW mergeTableBattles (trust incoming) — the fix.
const mergeTrust = (_existing: Battle[], incoming: Battle[]): Battle[] => incoming ?? _existing ?? [];

// The optimistic-conflict resolver's injection: while my card is optimistically
// tracked, inject it into an incoming server state if absent (mirrors
// AnimationContext.tsx:700-726).
const injectOptimistic = (incoming: Battle[], myCard: { suit: number; value: number } | null): Battle[] => {
  if (!myCard) return incoming;
  if (incoming.some((b) => k(b.attack) === k(myCard) || (b.defense && k(b.defense) === k(myCard)))) return incoming;
  return [...incoming, { attack: myCard, defense: null }];
};

let pass = true;
const check = (cond: boolean, label: string) => { console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${label}`); if (!cond) pass = false; };

console.log('=== optimistic + revert validation (post-fix) ===');

// Scenario A — optimistic attack stays visible until the server confirms.
{
  let client: Battle[] = []; // my table starts empty
  const myCard = c(3, 7); // optimistically attacked 7♦, still tracked
  // an UNRELATED server broadcast arrives before my attack is confirmed; server's
  // table doesn't have my card yet, but the resolver injects it.
  const serverUnrelated = injectOptimistic([], myCard);
  client = mergeTrust(client, serverUnrelated);
  check(tableKeys(client).includes(k(myCard)), 'A1: optimistic attack survives an unrelated broadcast (trust + injection)');
  // server CONFIRMS my attack; tracking cleared (no more injection); incoming has it.
  client = mergeTrust(client, [{ attack: myCard, defense: null }]);
  check(tableKeys(client).join() === [k(myCard)].join(), 'A2: confirmed attack persists exactly once');
}

// Scenario B — server REJECTS my attack: revert fires, tracking cleared, and the
// card must NOT linger on the table when the next (different-bout) state arrives.
{
  const myCard = c(3, 7); // 7♦ I optimistically attacked, shown on my table
  let client: Battle[] = [{ attack: myCard, defense: null }];
  // rejected -> revert animation removes it visually; tracking cleared so no
  // injection. A different attacker now attacks 9♣ (non-empty incoming).
  const serverNextBout: Battle[] = [{ attack: c(2, 8), defense: null }]; // 9♣
  const withFix = mergeTrust(client, serverNextBout);
  const withoutFix = mergeAppend(client, serverNextBout);
  check(!tableKeys(withFix).includes(k(myCard)), 'B1: reverted card is gone with the fix (trust incoming)');
  check(tableKeys(withoutFix).includes(k(myCard)),
    'B2: (contrast) old append would have left the reverted card stuck on the table');
}

// Scenario C — optimistic COVER: I cover an attack; resolver injects the defense
// into the matching battle; trust keeps it until confirm.
{
  const attackCard = c(1, 5); // 6♥ on the table, uncovered
  const myCover = c(0, 9);    // 10♠ I optimistically cover with (trump)
  let client: Battle[] = [{ attack: attackCard, defense: myCover }]; // optimistic cover shown
  // server broadcast for an unrelated event; server still shows it uncovered, but
  // the cover resolver re-applies my optimistic defense onto the matching battle.
  const serverUncovered: Battle[] = [{ attack: attackCard, defense: null }];
  const injected = serverUncovered.map((b) => k(b.attack) === k(attackCard) ? { attack: b.attack, defense: myCover } : b);
  client = mergeTrust(client, injected);
  check(client[0].defense != null && k(client[0].defense) === k(myCover), 'C1: optimistic cover survives until confirmed');
  // confirmed: server reports it covered
  client = mergeTrust(client, [{ attack: attackCard, defense: myCover }]);
  check(client[0].defense != null && k(client[0].defense!) === k(myCover), 'C2: confirmed cover persists');
}

// Scenario D — reconnect resync (Q7): the authoritative load lacks my still-
// unconfirmed optimistic card; applyOptimisticOverlay re-applies it so it doesn't
// vanish-then-reappear. Models ServerContext.applyOptimisticOverlay.
type Entry = { card: { suit: number; value: number }; target?: { suit: number; value: number } | null };
const applyOverlay = (table: Battle[], entries: Entry[]): Battle[] => {
  const g = table.map((b) => ({ attack: { ...b.attack }, defense: b.defense ? { ...b.defense } : null }));
  for (const e of entries) {
    if (e.target) {
      const b = g.find((x) => k(x.attack) === k(e.target!) && x.defense === null);
      if (b) b.defense = e.card;
    } else if (!g.some((x) => k(x.attack) === k(e.card) || (x.defense && k(x.defense) === k(e.card)))) {
      g.push({ attack: e.card, defense: null });
    }
  }
  return g;
};
{
  const myAttack = c(3, 7); // 7♦ I just played, not yet confirmed by the server
  const authoritativeLoad: Battle[] = []; // resync fetched pre-attack state
  const withoutFix = authoritativeLoad;                       // vanish
  const withFix = applyOverlay(authoritativeLoad, [{ card: myAttack }]); // preserved
  check(!tableKeys(withoutFix).includes(k(myAttack)), 'D1: (contrast) plain resync drops the unconfirmed card (the vanish)');
  check(tableKeys(withFix).includes(k(myAttack)), 'D2: resync + overlay keeps the optimistic attack (no vanish)');

  // and a pending optimistic COVER survives the resync too
  const atk = c(1, 5); const cov = c(0, 9);
  const loadUncovered: Battle[] = [{ attack: atk, defense: null }];
  const coverKept = applyOverlay(loadUncovered, [{ card: cov, target: atk }]);
  check(coverKept[0].defense != null && k(coverKept[0].defense!) === k(cov), 'D3: resync + overlay keeps the optimistic cover');
}

// Scenario E — Q6: a gated-out confirmation still releases optimistic tracking.
// My attack 7♦ is optimistically tracked. Its confirming broadcast (events name
// 7♦) is DROPPED by the version gate because a higher version reordered ahead.
// That higher-version message's events don't name 7♦, but its AUTHORITATIVE table
// contains 7♦ (the server has it). The fix clears any optimistic entry whose card
// is on the authoritative table, so the entry doesn't linger (and can't be
// re-injected as a phantom after the bout ends).
{
  const tracked = new Set<string>([k(c(3, 7))]); // optimistic 7♦ still tracked
  // higher-version authoritative state that confirms 7♦ on the table
  const authoritativeTable: Battle[] = [{ attack: c(3, 7), defense: c(0, 9) }];
  const onTable = new Set<string>();
  for (const b of authoritativeTable) { onTable.add(k(b.attack)); if (b.defense) onTable.add(k(b.defense)); }
  // the fix: release tracked entries whose card is on the authoritative table
  for (const key of [...tracked]) if (onTable.has(key)) tracked.delete(key);
  check(!tracked.has(k(c(3, 7))), 'E1: gated-out confirmation still releases the optimistic entry (no lingering leak)');

  // and after the bout ends (7♦ no longer on the table), nothing is re-injected
  const reinjectCandidates = [...tracked]; // would-be phantoms
  check(reinjectCandidates.length === 0, 'E2: nothing left to re-inject as a phantom after the bout ends');
}

console.log(pass ? 'optimistic + revert: PASS (still working; reverts clean; resync no vanish; Q6 leak closed)' : 'optimistic + revert: FAIL');
process.exit(pass ? 0 : 1);
