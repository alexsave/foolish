// The iMessage concurrency model, end to end through the real kernel:
// Rule P (which chain everyone prefers) and Rule R (rebasing the move that lost
// a race). Spec: docs/IMESSAGE_GAME_DESIGN.md §7 and §14's worked examples.
//
// Durak has NO turn alternation — several seats may legally act at the same
// moment (§5.1) — so two players WILL compose against the same parent. There is
// no server to arbitrate. Every device must reach the same answer from the bytes
// alone, without clocks and without trusting delivery order.
//
// NOTE ON SHAPE. The handoff (§4 M0.5) asked for Rule P/R as pure TS functions,
// to be "the reference implementation the Swift port must match fixture-for-
// fixture in M3". They are in C instead (msg_wire.c), so there is no port and
// nothing to keep in step: the phone runs these same bytes through libfoolish.a.
// This suite is therefore the model's TEST, not its home.
//
// Run: npx tsx --test e2e/msg_concurrency.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import {
    kernelMsgDecode, kernelMsgSeal, kernelMsgRuleP, kernelMsgRebase, kernelMsgLegalMoves,
    MSG_REBASE_REAPPLY, MSG_REBASE_DISCARD_ROUND, MSG_REBASE_DISCARD_ILLEGAL,
} from '../sdk/ts/wasm/bots.ts';

// Mid-game turn bubbles sealed by the native kernel (sdk/c/build/msg_wire_test
// --fixture). Every chain below is grown from one of these.
const PARENTS: Record<number, string> = {
    2: 'f7020002efcdab89674523010700000200010000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e31070003a9cc795118a16a9edd28d516',
    3: 'f7020002efcdab89674523010a0000030001000000000000000079d87206410d37d302c19dfb6cacbc8bebf879d242622082315709cc0f183788030004416e6e300104416e6e310204416e6e320a00012cb4fce6acbe29ba5d0adae18a66b4fdc7f6',
    4: 'f7020002efcdab89674523010500000400010000000000000000449bbad52d5dfb1bdb68d87a09fe591b9419f9f39b0ec35e9f2b75c5a359a138040004416e6e300104416e6e310204416e6e320304416e6e33050003b7ddc3ef88a264acb5183fbe413a46',
};

const hex = (h: string) => Uint8Array.from(h.match(/../g)!.map(b => parseInt(b, 16)));

const AWIRE = { attack: 0, cover: 1, pass: 2, pickup: 3, good: 4 } as const;
const wireCard = (c: { suit: number; value: number }) => c.suit * 13 + (c.value - 1);

// A move, as the bytes the kernel takes (awire — sdk/c/src/awire.h).
function toWire(m: { type: string; cards?: { suit: number; value: number }[]; attack_cards?: { suit: number; value: number }[] }): Uint8Array {
    const kind = AWIRE[m.type as keyof typeof AWIRE];
    if (kind === AWIRE.pickup || kind === AWIRE.good) return Uint8Array.from([kind, 0]);
    const cards = m.cards ?? [];
    const out = [kind, cards.length, ...cards.map(wireCard)];
    if (kind === AWIRE.cover) out.push(...(m.attack_cards ?? []).map(wireCard));
    return Uint8Array.from(out);
}

// Adopt a chain, play one move on top of it, and seal the result — the exact
// send path a device takes. `parent` is the envelope being built on.
function playOn(parent: Uint8Array, seat: number, move: { type: string; cards?: any[]; attack_cards?: any[] }): Uint8Array {
    const p = kernelMsgDecode(parent);
    const verdict = kernelMsgRebase(p.round, seat, toWire(move));
    assert.equal(verdict, MSG_REBASE_REAPPLY, `${move.type} by seat ${seat} should be legal on the parent`);
    return kernelMsgSeal({
        flags: 0, phase: 2, n_players: p.n_players, variant: 0,
        last_actor_seat: seat, game_id: p.game_id,
        parent8: p.digest.slice(0, 8), seed: p.seed, joins: p.joins,
    });
}

// Who can do what, on the parent.
function seatsWith(parent: Uint8Array, type: string): { seat: number; move: any }[] {
    const p = kernelMsgDecode(parent);
    const out: { seat: number; move: any }[] = [];
    for (let s = 0; s < p.n_players; s++) {
        const m = kernelMsgLegalMoves(s).find(x => x.type === type);
        if (m) out.push({ seat: s, move: m });
    }
    return out;
}

// ---------------------------------------------------------------------------
// Rule P is a total order (§7.2)
// ---------------------------------------------------------------------------

test('Rule P: a chain never beats itself, and equal chains tie', () => {
    for (const h of Object.values(PARENTS)) {
        const e = hex(h);
        assert.equal(kernelMsgRuleP(e, e), 0, 'reflexive: the same bytes are the same chain');
    }
});

test('Rule P: antisymmetric — swapping the arguments flips the verdict', () => {
    const p = hex(PARENTS[4]);
    const pick = seatsWith(p, 'pickup');
    const atk = seatsWith(p, 'attack');
    assert.ok(pick.length && atk.length, 'the fixture must offer both a pickup and an attack');
    const a = playOn(p, pick[0].seat, pick[0].move);
    const b = playOn(p, atk[0].seat, atk[0].move);
    const ab = kernelMsgRuleP(a, b);
    const ba = kernelMsgRuleP(b, a);
    assert.notEqual(ab, 0, 'two different chains must order');
    assert.equal(ab, -ba, 'a<b iff b>a');
});

test('Rule P: transitive over a fan of real chains', () => {
    const p = hex(PARENTS[4]);
    const chains: Uint8Array[] = [];
    for (let s = 0; s < 4; s++) {
        for (const m of kernelMsgLegalMoves(s).slice(0, 3)) {
            try { chains.push(playOn(p, s, m)); } catch { /* not legal for this seat */ }
        }
        kernelMsgDecode(p); // re-adopt: the enumeration above mutated nothing, but be explicit
    }
    assert.ok(chains.length >= 3, `need a fan of chains, got ${chains.length}`);
    const sorted = [...chains].sort((x, y) => kernelMsgRuleP(x, y));
    for (let i = 0; i < sorted.length - 1; i++) {
        for (let j = i + 1; j < sorted.length; j++) {
            assert.ok(kernelMsgRuleP(sorted[i], sorted[j]) <= 0,
                `sorted order must be consistent: ${i} vs ${j}`);
        }
    }
});

test('Rule P: delivery order is never an input', () => {
    // The same set of chains, adopted in every order, converges on one winner.
    const p = hex(PARENTS[3]);
    const pick = seatsWith(p, 'pickup')[0];
    const atk = seatsWith(p, 'attack')[0];
    const a = playOn(p, pick.seat, pick.move);
    const b = playOn(p, atk.seat, atk.move);
    const c = hex(PARENTS[3]);

    const adopt = (order: Uint8Array[]) => order.reduce((best, cur) =>
        kernelMsgRuleP(cur, best) < 0 ? cur : best);
    const orders = [[a, b, c], [a, c, b], [b, a, c], [b, c, a], [c, a, b], [c, b, a]];
    const winners = orders.map(o => Buffer.from(adopt(o)).toString('hex'));
    assert.equal(new Set(winners).size, 1, 'every delivery order must pick the same chain');
});

// ---------------------------------------------------------------------------
// §14's worked examples
// ---------------------------------------------------------------------------

// §14.1 / §7.5 — THE canonical race. The defender's pickup and an attacker's
// throw-in are composed against the same parent. Pickup closes the bout, so its
// chain has the higher round and wins EVERYWHERE. That is also the right game
// outcome: picking up is the defender's prerogative at any moment, and the
// throw-in simply did not make it in time.
test('§14.1 pickup ∥ throw-in: pickup wins, in BOTH delivery orders', () => {
    for (const np of [3, 4]) {
        const p = hex(PARENTS[np]);
        const parent = kernelMsgDecode(p);
        const pick = seatsWith(p, 'pickup')[0];
        const atk = seatsWith(p, 'attack')[0];
        assert.ok(pick && atk, `${np}p fixture must offer a pickup and a throw-in`);

        const D = playOn(p, pick.seat, pick.move);   // defender picks up
        const A = playOn(p, atk.seat, atk.move);     // attacker throws in

        const d = kernelMsgDecode(D);
        const a = kernelMsgDecode(A);
        assert.ok(d.round > parent.round, 'pickup closes the bout');
        assert.equal(a.round, parent.round, 'a throw-in does not');

        // Both orders, same verdict — that is the whole claim.
        assert.ok(kernelMsgRuleP(D, A) < 0, `${np}p: D must win`);
        assert.ok(kernelMsgRuleP(A, D) > 0, `${np}p: D must win, arguments swapped`);
    }
});

// The loser's device then rebases, and the guard fires: the throw-in was chosen
// against round N's table, and round N is over. It is not silently re-applied as
// an opening attack of round N+1 — legal per the kernel, but not what the player
// chose. "Sveta picked up before your 9♣ landed."
test('§7.4 the round-boundary guard discards the throw-in that lost', () => {
    const p = hex(PARENTS[4]);
    const parent = kernelMsgDecode(p);
    const pick = seatsWith(p, 'pickup')[0];
    const atk = seatsWith(p, 'attack')[0];
    const D = playOn(p, pick.seat, pick.move);

    // The attacker adopts D and rebases the throw-in it composed against `parent`.
    kernelMsgDecode(D);
    const verdict = kernelMsgRebase(parent.round, atk.seat, toWire(atk.move));
    assert.equal(verdict, MSG_REBASE_DISCARD_ROUND,
        'an action never survives rebase across a round boundary');
});

// §14.3 — the common, boring race: two seats act on DIFFERENT battles in the
// same bout. Nothing conflicts; the loser's move re-applies cleanly.
test('§14.3 cover ∥ throw-in: the loser rebases with no user-visible conflict', () => {
    const p = hex(PARENTS[4]);
    const parent = kernelMsgDecode(p);
    const cov = seatsWith(p, 'cover')[0];
    const atk = seatsWith(p, 'attack')[0];
    if (!cov || !atk) return; // this fixture cannot pose the race

    const C = playOn(p, cov.seat, cov.move);
    const c = kernelMsgDecode(C);
    if (c.round !== parent.round) return; // the cover closed the bout; §14.3 assumes it did not

    // Attacker adopts the cover chain, rebases its throw-in: same round, and the
    // kernel re-validates capacity.
    const verdict = kernelMsgRebase(parent.round, atk.seat, toWire(atk.move));
    assert.ok(verdict === MSG_REBASE_REAPPLY || verdict === MSG_REBASE_DISCARD_ILLEGAL,
        'within a round, kernel legality is the only arbiter');
    if (verdict === MSG_REBASE_REAPPLY) {
        // ...and the merged envelope is sealable: "send to confirm".
        const merged = kernelMsgSeal({
            flags: 0, phase: 2, n_players: c.n_players, variant: 0,
            last_actor_seat: atk.seat, game_id: c.game_id,
            parent8: c.digest.slice(0, 8), seed: c.seed, joins: c.joins,
        });
        const m = kernelMsgDecode(merged);
        assert.equal(m.turn, c.turn + 1, 'the rebased move is on top of the adopted chain');
        assert.ok(kernelMsgRuleP(merged, C) < 0, 'and the merge supersedes what it rebased onto');
    }
});

// §14.2 — two attackers throw in at once. Same round, same turn, so Rule P falls
// through to the digest: arbitrary, but the SAME arbitrary everywhere. The loser
// rebases and the kernel re-checks capacity — exactly a physical table, where the
// fastest hand lands first.
test('§14.2 two attackers at once: the digest breaks the tie, identically everywhere', () => {
    const p = hex(PARENTS[4]);
    const parent = kernelMsgDecode(p);
    const attackers = seatsWith(p, 'attack');
    if (attackers.length < 2) return;

    const A1 = playOn(p, attackers[0].seat, attackers[0].move);
    const A2 = playOn(p, attackers[1].seat, attackers[1].move);
    const a1 = kernelMsgDecode(A1);
    const a2 = kernelMsgDecode(A2);

    if (a1.round === a2.round && a1.turn === a2.turn) {
        const v = kernelMsgRuleP(A1, A2);
        assert.notEqual(v, 0, 'different chains');
        // The verdict is the digest comparison, and it is stable and symmetric.
        assert.equal(v, -kernelMsgRuleP(A2, A1));
        const lex = Buffer.compare(Buffer.from(a1.digest), Buffer.from(a2.digest));
        assert.equal(Math.sign(v), Math.sign(lex), 'lexicographically smaller digest wins');
    }

    // The loser rebases within the same round: legality (capacity) decides.
    const winner = kernelMsgRuleP(A1, A2) < 0 ? A1 : A2;
    const loser = winner === A1 ? attackers[1] : attackers[0];
    kernelMsgDecode(winner);
    const verdict = kernelMsgRebase(parent.round, loser.seat, toWire(loser.move));
    assert.ok([MSG_REBASE_REAPPLY, MSG_REBASE_DISCARD_ILLEGAL, MSG_REBASE_DISCARD_ROUND].includes(verdict));
});

// §14.6 — a straggler races the terminal action. A FINISHED chain outranks it on
// round or turn, so the straggler's move is discarded and the result card shows.
test('§14.6 move ∥ game-over: the finished chain outranks a straggler', () => {
    // Constructed rather than played out: any chain with a higher round beats a
    // same-round straggler, which is the property §7.6's FINISHED row relies on.
    const p = hex(PARENTS[3]);
    const parent = kernelMsgDecode(p);
    const pick = seatsWith(p, 'pickup')[0];
    const atk = seatsWith(p, 'attack')[0];
    const closed = playOn(p, pick.seat, pick.move);   // round advanced
    const straggler = playOn(p, atk.seat, atk.move);  // round unchanged
    assert.ok(kernelMsgRuleP(closed, straggler) < 0, 'a closed round is settled history');
    kernelMsgDecode(closed);
    assert.equal(kernelMsgRebase(parent.round, atk.seat, toWire(atk.move)),
        MSG_REBASE_DISCARD_ROUND, 'the straggler is discarded, not silently re-applied');
});

// ---------------------------------------------------------------------------
// The invariants, over a fan of real races
// ---------------------------------------------------------------------------

test('every rebase verdict is one of §7.4\'s three reasons, and REAPPLY always seals', () => {
    const seen = new Set<number>();
    for (const np of [2, 3, 4]) {
        const p = hex(PARENTS[np]);
        const parent = kernelMsgDecode(p);
        const parentMoves: { seat: number; move: any }[] = [];
        for (let s = 0; s < np; s++) {
            for (const m of kernelMsgLegalMoves(s).slice(0, 4)) parentMoves.push({ seat: s, move: m });
        }
        for (const first of parentMoves) {
            let chain: Uint8Array;
            try { chain = playOn(p, first.seat, first.move); } catch { continue; }
            const c = kernelMsgDecode(chain);
            for (const pending of parentMoves) {
                kernelMsgDecode(chain); // re-adopt for each pending action
                const v = kernelMsgRebase(parent.round, pending.seat, toWire(pending.move));
                assert.ok([MSG_REBASE_REAPPLY, MSG_REBASE_DISCARD_ROUND, MSG_REBASE_DISCARD_ILLEGAL].includes(v),
                    `verdict ${v} is not one of the three reasons`);
                seen.add(v);
                if (v === MSG_REBASE_REAPPLY) {
                    // A re-applied move must produce a sendable envelope — the
                    // "send to confirm" path. If this throws, a legal move was
                    // silently lost, which is the one thing Rule R forbids.
                    const merged = kernelMsgSeal({
                        flags: 0, phase: 2, n_players: c.n_players, variant: 0,
                        last_actor_seat: pending.seat, game_id: c.game_id,
                        parent8: c.digest.slice(0, 8), seed: c.seed, joins: c.joins,
                    });
                    assert.ok(merged.length > 0);
                }
            }
        }
    }
    // The suite is worthless if it never actually exercised the guard.
    assert.ok(seen.has(MSG_REBASE_REAPPLY), 'no move ever re-applied');
    assert.ok(seen.has(MSG_REBASE_DISCARD_ROUND), 'the round guard never fired');
});

test('a chain a device builds is always one it would itself accept', () => {
    // Every envelope this suite seals must survive a full decode+replay by a
    // FRESH reader — the property that lets a chain cross any number of devices.
    for (const np of [2, 3, 4]) {
        const p = hex(PARENTS[np]);
        const moves = seatsWith(p, 'pickup').concat(seatsWith(p, 'attack'));
        for (const m of moves) {
            const chain = playOn(p, m.seat, m.move);
            const e = kernelMsgDecode(chain);   // throws if the header lies
            assert.equal(e.n_players, np);
            assert.ok(e.turn > 0);
        }
    }
});
