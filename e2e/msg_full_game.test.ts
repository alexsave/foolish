// FMSG — a WHOLE game through the send/accept leg (B4).
//
// The other msg_* suites prove the codec and the Rule P/R races in isolation;
// this drives a COMPLETE 2-player Durak game seat-by-seat through the envelope
// wire — the exact loop the iMessage extension runs, minus Apple's transport and
// the SwiftUI board. Each turn: decode the bubble (validate = replay), assert the
// bubble's PUBLIC snapshot hides every hand (design §5 — a bubble image never
// carries a hand), play a legal move for whichever seat can act, rebase it onto
// the resident game (REAPPLY), and reseal. The game must reach a fool, and the
// terminal must seal as FINISHED (a finished chain sealed as LIVE is refused,
// which is how this test found its own bug the first time it ran).
//
// Run: npx tsx --test e2e/msg_full_game.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import { kernelMsgDecode, kernelMsgSeal, kernelMsgRebase, kernelMsgLegalMoves, kernelMsgPublicView, kernelResidentReplayCodeV6, MSG_REBASE_REAPPLY, kernelB32Encode } from '../sdk/ts/wasm/bots.ts';
import { classifyPathSegment } from '../server/api/common/replay/codec.ts';

const AWIRE = { attack: 0, cover: 1, pass: 2, pickup: 3, good: 4 } as const;
const wireCard = (c: { suit: number; value: number }) => c.suit * 13 + (c.value - 1);
const hex = (h: string) => Uint8Array.from(h.match(/../g)!.map(b => parseInt(b, 16)));

function toWire(m: { type: string; cards?: any[]; attack_cards?: any[] }): Uint8Array {
    const kind = AWIRE[m.type as keyof typeof AWIRE];
    if (kind === AWIRE.pickup || kind === AWIRE.good) return Uint8Array.from([kind, 0]);
    const cards = m.cards ?? [];
    const out = [kind, cards.length, ...cards.map(wireCard)];
    if (kind === AWIRE.cover) out.push(...(m.attack_cards ?? []).map(wireCard));
    return Uint8Array.from(out);
}

// A native-sealed mid-game 2p turn bubble (shared with e2e/msg_wire.test.ts).
const START_2P = 'f7020002efcdab89674523010800000200020000000000000000ae15293755bd748b2919627cd0591ffb42d7f9b2e9b57da5c2839ed47bd7ced7020004416e6e300104416e6e310800f72719e90cb7ee031bd6af74a3a23a';

// A priority that always drives a round to a close: cover/attack while cards
// remain, then good/pass shut the round, pickup only when nothing else is legal.
const PRIORITY = ['cover', 'attack', 'good', 'pass', 'pickup'];
const CAP = 3000;

test('a full 2p game plays to a fool through the FMSG send/accept leg, and no public bubble ever leaks a hand', () => {
    let bubble = hex(START_2P);
    let steps = 0, sealBytesMax = 0, fool = -1;

    for (; steps < CAP; steps++) {
        const p = kernelMsgDecode(bubble);              // adopt the chain

        // The bubble IMAGE is the spectator snapshot: no seat's hand may appear.
        for (const pl of kernelMsgPublicView().view.players) {
            assert.equal(pl.hand, null, `step ${steps}: seat ${pl.seat} hand exposed in the public bubble`);
        }
        const over = kernelMsgPublicView().view.gameOver;
        if (over >= 0) { fool = over; break; }

        // Whichever seat can act, by the closing priority above.
        let chosen: { seat: number; move: any } | null = null;
        scan: for (const type of PRIORITY) {
            for (let s = 0; s < p.n_players; s++) {
                const m = kernelMsgLegalMoves(s).find(x => x.type === type);
                if (m) { chosen = { seat: s, move: m }; break scan; }
            }
        }
        assert.ok(chosen, `step ${steps}: no legal move for any seat (stuck short of game over)`);

        const verdict = kernelMsgRebase(p.round, chosen!.seat, toWire(chosen!.move));
        assert.equal(verdict, MSG_REBASE_REAPPLY, `step ${steps}: ${chosen!.move.type} by seat ${chosen!.seat} should REAPPLY`);

        // A finished chain seals as FINISHED (phase 3), carrying the replay
        // funnel; anything mid-game is LIVE (phase 2). Sealing a finished game as
        // LIVE is refused by the kernel — so this branch is load-bearing.
        const finished = kernelMsgPublicView().view.gameOver >= 0;
        bubble = kernelMsgSeal({
            flags: 0, phase: finished ? 3 : 2, n_players: p.n_players, variant: 0,
            last_actor_seat: chosen!.seat, game_id: p.game_id,
            parent8: p.digest.slice(0, 8), seed: p.seed, joins: p.joins,
        });
        sealBytesMax = Math.max(sealBytesMax, bubble.length);
        kernelMsgDecode(bubble);                        // every sealed bubble must re-accept
    }

    assert.ok(steps < CAP, 'game did not terminate within the step cap');
    assert.ok(fool >= 0, 'game ended without a fool');
    assert.ok(sealBytesMax < 240, `a turn bubble grew to ${sealBytesMax} B — past the MSMessage.url budget`);
});

// batch 6 item B: the FINISHED bubble's own URL is a normal /m/ payload link
// now (MessageEnvelope.link, decodable by ANY receiver — MessagesViewController.
// stage's doc explains why the old bare replay-code link broke for receivers),
// and the replay funnel moved one hop out to the web /m/ page: it decodes that
// SAME payload and derives the replay code from what it just decoded
// (kernelResidentReplayCodeV6, sdk/ts/wasm/bots.ts). This drives the same
// fixture to a fool and proves that derivation actually produces a working
// replay code from a FINISHED envelope's own (decoded) seed — the exact thing
// src/app/m/[payload]/page.tsx now does for its "Watch the replay" CTA.
test('a FINISHED envelope\'s own seed derives a real replay code — the /m/ page funnel (batch 6 item B)', () => {
    let bubble = hex(START_2P);
    let finishedEnv: ReturnType<typeof kernelMsgDecode> | null = null;

    for (let steps = 0; steps < CAP; steps++) {
        const p = kernelMsgDecode(bubble);
        if (kernelMsgPublicView().view.gameOver >= 0) { finishedEnv = p; break; }

        let chosen: { seat: number; move: any } | null = null;
        scan: for (const type of PRIORITY) {
            for (let s = 0; s < p.n_players; s++) {
                const m = kernelMsgLegalMoves(s).find(x => x.type === type);
                if (m) { chosen = { seat: s, move: m }; break scan; }
            }
        }
        if (!chosen) break;
        kernelMsgRebase(p.round, chosen.seat, toWire(chosen.move));
        const finished = kernelMsgPublicView().view.gameOver >= 0;
        bubble = kernelMsgSeal({
            flags: 0, phase: finished ? 3 : 2, n_players: p.n_players, variant: 0,
            last_actor_seat: chosen.seat, game_id: p.game_id,
            parent8: p.digest.slice(0, 8), seed: p.seed, joins: p.joins,
        });
    }

    assert.ok(finishedEnv, 'the fixture must reach a fool within the step cap');
    assert.equal(finishedEnv!.phase, 3, 'FINISHED');

    // The page's exact call: derive the code from the decoded envelope's own
    // seed, with nothing re-marshalled (kernelMsgDecode already left the whole
    // session log resident).
    const code = kernelResidentReplayCodeV6(finishedEnv!.seed);
    assert.ok(code.length > 0, 'a finished game must produce a replay code');

    // The resulting URL (https://foolish.cards/<b32>) must actually route to
    // the replay screen, not the legacy authenticated shortcode path — the
    // same classifier the site's own [game_id] page uses.
    const b32 = kernelB32Encode(code);
    assert.equal(classifyPathSegment(b32), 'replay',
                'the derived code must be long enough to route to ReplayScreen');
});
