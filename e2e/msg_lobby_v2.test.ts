// Lobby v2 — the open-count group lobby (docs/IMESSAGE_LOBBY_V2.md, batch 6
// item C, notes 19/20/25), end to end through the SAME wasm kernel the web and
// the phone both replay against.
//
// THE CLAIM UNDER TEST: a lobby is created OPEN — dealt at the wire's max
// capacity (n_players=8) so seats stay free — and Start later re-derives the
// SAME locked seed at the ACTUAL joined count (here 3), sealing a LIVE
// envelope whose n_players (3) legitimately differs from its WAITING parent's
// (8). Nothing in msg_wire.c cross-checks a child's n_players against its
// parent chain: parentage is only the 8-byte digest tag (parent8), carried and
// read back, never independently verified against the parent's own header.
// Each envelope is decoded/replayed entirely from its OWN bytes (msg_decode +
// msg_replay's deal_from_envelope re-deals fresh from `seed` at `n_players`
// every time), so a WAITING(8) -> LIVE(3) transition is not a special case the
// wire has to allow — it is simply two independent, self-describing envelopes
// that happen to share a seed and a parent8 tag. This test proves that in
// practice: create -> 3 joins (still WAITING, never auto-starting) -> start at
// the real count (3) -> play a move, decoding and validating every single leg.
//
// Run: npx tsx --test e2e/msg_lobby_v2.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import {
    kernelMsgDecode, kernelMsgSeal, kernelMsgRebase, kernelMsgLegalMoves,
    kernelMsgPublicView, kernelMsgRuleP, MSG_REBASE_REAPPLY,
} from '../sdk/ts/wasm/bots.ts';

const AWIRE = { attack: 0, cover: 1, pass: 2, pickup: 3, good: 4 } as const;
const wireCard = (c: { suit: number; value: number }) => c.suit * 13 + (c.value - 1);

function toWire(m: { type: string; cards?: any[]; attack_cards?: any[] }): Uint8Array {
    const kind = AWIRE[m.type as keyof typeof AWIRE];
    if (kind === AWIRE.pickup || kind === AWIRE.good) return Uint8Array.from([kind, 0]);
    const cards = m.cards ?? [];
    const out = [kind, cards.length, ...cards.map(wireCard)];
    if (kind === AWIRE.cover) out.push(...(m.attack_cards ?? []).map(wireCard));
    return Uint8Array.from(out);
}

// A 32-byte deal seed, non-zero (an all-zero seed is refused, MSG_ESEED).
const SEED = Uint8Array.from({ length: 32 }, (_, i) => (i * 41 + 7) & 0xff);
const GAME_ID = 268501037n;
const ZERO8 = new Uint8Array(8);

test('create-open-lobby(8) -> 3 joins (never auto-starting) -> start-at-3 -> play a move, decoding every leg', () => {
    // ---- Create: lock the seed in, dealt OPEN at the wire's max (8) --------
    const waiting0 = kernelMsgSeal({
        flags: 0, phase: 0, n_players: 8, variant: 0, last_actor_seat: 0,
        game_id: GAME_ID, parent8: ZERO8, seed: SEED,
        joins: [{ seat: 0, name: 'Alex' }],
    });
    let env = kernelMsgDecode(waiting0);
    assert.equal(env.phase, 0, 'WAITING');
    assert.equal(env.n_players, 8, 'open-lobby convention: max capacity, not a chosen count');
    assert.equal(env.joins.length, 1);
    assert.deepEqual(env.seed, SEED, 'the locked seed round-trips unchanged');

    // ---- Join seat 1 --------------------------------------------------------
    let waiting = kernelMsgSeal({
        flags: 0, phase: 0, n_players: 8, variant: 0, last_actor_seat: 1,
        game_id: GAME_ID, parent8: env.digest.slice(0, 8), seed: env.seed,
        joins: [...env.joins, { seat: 1, name: 'Sveta' }],
    });
    env = kernelMsgDecode(waiting);
    assert.equal(env.phase, 0, 'still WAITING after one join');
    assert.equal(env.n_players, 8, 'the open capacity never shrinks on a join');
    assert.equal(env.joins.length, 2);

    // ---- Join seat 2 — THREE joined now, and STILL just a lobby ------------
    waiting = kernelMsgSeal({
        flags: 0, phase: 0, n_players: 8, variant: 0, last_actor_seat: 2,
        game_id: GAME_ID, parent8: env.digest.slice(0, 8), seed: env.seed,
        joins: [...env.joins, { seat: 2, name: 'Boris' }],
    });
    const lobbyAt3 = kernelMsgDecode(waiting);
    assert.equal(lobbyAt3.phase, 0, 'lobby v2: joining NEVER auto-starts the game, at any count');
    assert.equal(lobbyAt3.n_players, 8, 'still the open capacity, not 3');
    assert.equal(lobbyAt3.joins.length, 3);
    assert.deepEqual(lobbyAt3.joins.map(j => j.seat), [0, 1, 2], 'seats claimed lowest-free-first: contiguous');

    // ---- Start, as seat 1 (ANY joined player, not just the creator) --------
    // Re-derives the SAME locked seed at the REAL joined count (3) — the LIVE
    // envelope's n_players (3) differs from its WAITING parent's (8), and nothing
    // about that is special-cased: deal_from_envelope re-deals fresh from
    // `env.seed` at `env.n_players` on EVERY decode, whatever the last envelope
    // claimed. parent8 is the only link back to the lobby, and it is just a tag.
    const live0 = kernelMsgSeal({
        flags: 0, phase: 2, n_players: 3, variant: 0, last_actor_seat: 1,
        game_id: GAME_ID, parent8: lobbyAt3.digest.slice(0, 8), seed: lobbyAt3.seed,
        joins: lobbyAt3.joins,
    });
    env = kernelMsgDecode(live0);
    assert.equal(env.phase, 2, 'LIVE handoff');
    assert.equal(env.turn, 0, 'no move yet — just the re-deal');
    assert.notEqual(env.n_players, lobbyAt3.n_players, 'THE claim: child n_players (3) != parent (8)');
    assert.equal(env.n_players, 3, 'the REAL joined count, not the lobby capacity');
    assert.deepEqual(env.parent8, lobbyAt3.digest.slice(0, 8),
                     'parentage is carried as this tag alone, independent of either side\'s n_players');
    // Rule P: a fresh handoff (turn 0) sitting on top of a lobby (round/turn 0
    // too, phase WAITING) — a LIVE envelope must still be comparable, and never
    // crash the moment n_players first diverges from what came before it.
    assert.doesNotThrow(() => kernelMsgRuleP(live0, waiting));

    // ---- Play one legal move on the freshly-dealt 3p game ------------------
    let chosen: { seat: number; move: any } | null = null;
    for (let s = 0; s < env.n_players && !chosen; s++) {
        const m = kernelMsgLegalMoves(s).find(x => x.type !== 'wait');
        if (m) chosen = { seat: s, move: m };
    }
    assert.ok(chosen, 'the freshly-dealt 3p game must have a first attacker with a legal move');

    const verdict = kernelMsgRebase(env.round, chosen!.seat, toWire(chosen!.move));
    assert.equal(verdict, MSG_REBASE_REAPPLY, `${chosen!.move.type} by seat ${chosen!.seat} should REAPPLY`);

    const finished = kernelMsgPublicView().view.gameOver >= 0;
    const live1 = kernelMsgSeal({
        flags: 0, phase: finished ? 3 : 2, n_players: 3, variant: 0,
        last_actor_seat: chosen!.seat, game_id: GAME_ID,
        parent8: env.digest.slice(0, 8), seed: env.seed, joins: env.joins,
    });
    const played = kernelMsgDecode(live1);   // validate = replay (§7.3): must accept cleanly
    assert.equal(played.n_players, 3);
    assert.ok(played.turn > 0, 'the move is now part of the chain');
    assert.equal(played.phase, finished ? 3 : 2);
});

// Rule P rule 3 — the double-Start fork (the shipped 4-player deadlock).
//
// Any joined player may Start, and Start deals at the tapped bubble's join
// count. Two players starting near-simultaneously — or one starting off a
// stale bubble that predates the last join — therefore seal TWO LIVE handoffs,
// both round 0 / turn 0, dealt from the SAME locked seed at DIFFERENT player
// counts: different games (different trump, different first attacker). Under
// the digest tiebreak the smaller fork won half the time, stranding the last
// joiner (their cached seat is out of range of the smaller game) and, when the
// full game's first attacker was the player stuck on the small fork's board,
// deadlocking every screen in the chat. Kernel rule 3 (msg_wire.h): at an
// equal (round, turn) the fuller roster wins, before the digest — on the phone
// AND here in the wasm the web replays through, or the two would fork.
// A 0-action envelope assembled byte-for-byte (msg_wire.h layout). The two
// racing Starts are exactly this — turn 0, round 0, empty body, the deal alone
// is the state — and building them by hand keeps this test independent of what
// the previous test left resident (kernelMsgSeal seals the RESIDENT game).
// kernelMsgDecode below validates every byte of them through the kernel.
function handoff0(phase: number, nPlayers: number, la: number, gid: bigint,
                  joins: { seat: number; name: string }[]): Uint8Array {
    const out = [0xf7, 2, 0, phase];                                   // magic, format, flags, phase
    for (let i = 0n; i < 8n; i++) out.push(Number((gid >> (8n * i)) & 0xffn));
    out.push(0, 0, la, nPlayers, 0, 0);                                // turn u16, last_actor, n_players, variant, round
    out.push(...ZERO8, ...SEED, joins.length);
    for (const j of joins) {
        const name = Array.from(new TextEncoder().encode(j.name));
        out.push(j.seat, name.length, ...name);
    }
    out.push(0, 0);                                                    // n_actions u16: the deal alone
    return Uint8Array.from(out);
}

test('two Starts race: the fuller roster wins Rule P everywhere, but never over real progress', () => {
    const gid = GAME_ID + 1n;
    const joins3 = [{ seat: 0, name: 'Alex' }, { seat: 1, name: 'Sveta' }, { seat: 2, name: 'Boris' }];
    const joins4 = [...joins3, { seat: 3, name: 'Dima' }];

    // Alex starts from the full 4-join lobby; Sveta from her stale 3-join view.
    const live4 = handoff0(2, 4, 0, gid, joins4);
    const live3 = handoff0(2, 3, 1, gid, joins3);
    const env4 = kernelMsgDecode(live4);
    const env3 = kernelMsgDecode(live3);
    assert.equal(env4.turn, 0); assert.equal(env3.turn, 0);   // the tie rule 3 must break

    assert.ok(kernelMsgRuleP(live4, live3) < 0, 'the full 4p game must beat the stale 3p start');
    assert.ok(kernelMsgRuleP(live3, live4) > 0, 'and the comparison must be symmetric');

    // But rule 3 sits BELOW turn: a chain someone actually played on is never
    // clobbered by a stale wider Start sealed after the fact.
    let chosen: { seat: number; move: any } | null = null;
    for (let s = 0; s < env3.n_players && !chosen; s++) {
        const m = kernelMsgLegalMoves(s).find(x => x.type !== 'wait');
        if (m) chosen = { seat: s, move: m };
    }
    assert.ok(chosen, 'the 3p deal must have a first attacker with a legal move');
    assert.equal(kernelMsgRebase(env3.round, chosen!.seat, toWire(chosen!.move)), MSG_REBASE_REAPPLY);
    const played3 = kernelMsgSeal({
        flags: 0, phase: kernelMsgPublicView().view.gameOver >= 0 ? 3 : 2, n_players: 3, variant: 0,
        last_actor_seat: chosen!.seat, game_id: gid,
        parent8: env3.digest.slice(0, 8), seed: env3.seed, joins: env3.joins,
    });
    assert.ok(kernelMsgRuleP(played3, live4) < 0,
              'a played-on chain out-ranks a wider turn-0 start (turn dominates joins)');
});
