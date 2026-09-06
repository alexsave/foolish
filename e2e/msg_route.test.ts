// The /m/ route's pipeline, minus React: base32 → decode → PUBLIC view → the
// game object the board renders. The page is JSX around exactly this.
//
// The claim under test is the invariant, not the layout: a stranger with the
// link sees the table and NO hand. That must hold because view.c masked it, not
// because the page forgot to render one.
import test from 'node:test';
import assert from 'node:assert/strict';
import { kernelMsgDecode, kernelMsgPublicView, kernelB32Encode, kernelB32Decode } from '../sdk/ts/wasm/bots.ts';

import { viewToGame } from '../sdk/ts/wire/view.ts';

const FIXTURE = 'f7020002efcdab89674523010a0000030001000000000000000079d87206410d37d302c19dfb6cacbc8bebf879d242622082315709cc0f183788030004416e6e300104416e6e310204416e6e320a00012951da5bef3096f9f7bf2cfb58d013f6d7fa';
const bytes = Uint8Array.from(FIXTURE.match(/../g)!.map(b => parseInt(b, 16)));

test('the URL round-trips through base32 (the codec the whole product shares)', () => {
    const link = '1' + kernelB32Encode(bytes);
    assert.equal(link[0], '1', 'the text-level version dispatches before any binary');
    assert.deepEqual(kernelB32Decode(link.slice(1)), bytes);
    // QR-alphanumeric-safe and URL-safe: no padding, no case games.
    assert.match(link.slice(1), /^[A-Z2-7]+$/);
});

test('the page fits well inside MSMessage.url (5,000 chars)', () => {
    const link = `https://foolish.cards/m/1${kernelB32Encode(bytes)}`;
    assert.ok(link.length < 5000, `${link.length} chars`);
});

test('a stranger with the link sees the table and NO hand', () => {
    const env = kernelMsgDecode(kernelB32Decode(kernelB32Encode(bytes)));
    const { view } = kernelMsgPublicView();
    const roster = {
        id: 'imessage', name: 'iMessage game',
        players: Array.from({ length: env.n_players }, (_, i) => ({
            player_id: `p${i}`,
            name: env.joins.find(j => j.seat === i)?.name || `Seat ${i + 1}`,
            is_ai: false,
        })),
    };
    const game = viewToGame(view, roster, -1, { preGood: [], prevGoodTs: null }) as any;

    // The public table is all there — this is a real page, not a bounce.
    assert.equal(game.players.length, env.n_players);
    assert.ok(game.power_suit >= 0, 'trump is public');
    assert.ok(typeof game.deck_length === 'number');
    assert.ok(Array.isArray(game.table_battles));
    assert.deepEqual(game.players.map((p: any) => p.name), ['Ann0', 'Ann1', 'Ann2']);

    // ...and no hand crossed. A spectator view has no `self`, and every seat is
    // a count only. This appears on lock screens; a leak here is the payload
    // handing someone else's cards to a stranger.
    assert.equal(game.self, undefined, 'a spectator has no hand');
    for (const p of game.players) {
        assert.equal((p as any).hand, undefined, `seat ${p.player_id} leaked a hand`);
        assert.ok(p.hand_length > 0, 'counts are public, identities are not');
    }
});

test('a damaged link is refused, or renders a whole game — never half of one', () => {
    // A corrupted body is usually rejected outright. The body starts after the
    // 59-byte header, the joins, and the u16 count — derive it, never guess: at
    // a guessed offset this flipped a NICKNAME byte, which is legitimately valid
    // and made the test pass for the wrong reason.
    const nJoins = bytes[58];
    let bodyAt = 59;
    for (let i = 0; i < nJoins; i++) bodyAt += 2 + bytes[bodyAt + 1];
    bodyAt += 2;
    const broken = bytes.slice();
    broken[bodyAt + 1] ^= 0xff;
    assert.throws(() => kernelMsgDecode(broken), /iMessage payload/);
    // ...and the header, not the codec, is what catches most tampering.
    const badHeader = bytes.slice();
    badHeader[17] = 99;   // round: a lie the chain cannot back
    assert.throws(() => kernelMsgDecode(badHeader), /round does not match/);

    // But NOT every corruption can be caught, and pretending otherwise would be
    // the bug. An entropy-coded body has no framing: a truncated code is simply
    // a SHORTER code, and if it still yields this header's atom count and round
    // it is a valid chain — of a different game. That is not a leak, and the
    // page cannot half-render it: what binds a chain to its identity is the
    // digest a receiver checks against parent8, not decode refusing to read.
    // The invariant is all-or-nothing, so assert THAT.
    for (let cut = 1; cut <= 6; cut++) {
        const short = bytes.slice(0, bytes.length - cut);
        let env;
        try { env = kernelMsgDecode(short); } catch { continue; }   // refused: fine
        assert.equal(env.turn, kernelMsgDecode(short).turn, 'decode is deterministic');
        const { view } = kernelMsgPublicView();
        assert.equal(view.players.length, env.n_players, 'a whole game, or nothing');
    }
});
