// Determinism guard for the seed-dealt game: with a PINNED 32-byte deal seed,
// a whole game must replay byte-for-byte through the packed bot path — the path
// a bot-vs-bot game actually takes (marshalGame -> wasm_import_state). This is
// the regression guard for the bug where wasm_import_state dropped the
// deterministic-deck flag, so mid-game refills drew a RANDOM card and the game
// stopped being reproducible from its deal seed after the opening.
//
// simple_heuristic is the workhorse: it consumes NO RNG and keeps NO per-game
// memory, so any divergence is the DECK (draws), not the bot. octogen (which
// samples worlds seeded from game state) is checked too — it must be a pure
// function of the reproducible state.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { start_game_packed } from '../supabase/functions/_shared/game_lifecycle.ts';
import { processBotActionPacked } from '../supabase/functions/_shared/pure_bot_actions.ts';
import { game_done } from '../supabase/functions/_shared/common_utils.ts';
import { __setDealSeedOverride, kernelShouldAct } from '../supabase/functions/_shared/sdk/ts/wasm/engine.ts';
import { __ensureBots } from '../supabase/functions/_shared/sdk/ts/wasm/bots.ts';
import {
  Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS, STRATEGY_KEY,
} from '../supabase/functions/_shared/types.ts';

__ensureBots();

const mkGame = (strat: string, id: string): Game => ({
  players: [0, 1].map((i): PrivatePlayer => ({
    player_id: `p${i}`, name: `P${i}`, status: PLAYER_STATUS.READY, is_ai: true,
    hand: [], awaiting_attack: false, hand_length: 0,
    strategy_key: (STRATEGY_KEY as Record<string, string>)[strat],
  })),
  deck: [], logs: [], id, name: id, status: GAME_STATUS.PLAYING,
  deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
  first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
  good_timestamp: null, good_players: [],
} as Game);

async function playHash(strat: string, seed: Uint8Array, id: string, withholdSeed = false): Promise<string> {
  __setDealSeedOverride(seed);
  const g = mkGame(strat, id);
  start_game_packed(g);
  assert.equal(g.deterministic_deck, true, 'seed-dealt game must set deterministic_deck');
  // Attacker model: an adversary who knows the public board (and thus the deck,
  // via pop-order) but NOT the server-only game_seed. Null it after the deal so
  // the deck is unchanged (deterministic_deck already set) but the bots seed
  // from base 0 instead of the real seed.
  if (withholdSeed) g.game_seed = null;
  const h = createHash('sha256');
  h.update(`flip=${g.flipped!.suit}.${g.flipped!.value} fa=${g.first_attacker}\n`);
  let guard = 0;
  while (game_done(g) === null && ++guard < 4000) {
    const actor = g.players.find(p => p.is_ai && kernelShouldAct(g, p.player_id));
    if (!actor) break;
    const res = await processBotActionPacked(g, actor);
    if (!res || !(res as { run?: unknown }).run) break;
    const deck = g.deck.map(c => `${c.suit}.${c.value}`).join(',');
    const hands = g.players.map(p => p.hand.map(c => `${c.suit}.${c.value}`).join('|')).join('/');
    h.update(`${(res as { moveType: string }).moveType}:def${g.defender}:deck=${deck}:h=${hands}\n`);
  }
  h.update(`elim=${g.elimination_order.join(',')}\n`);
  return h.digest('hex');
}

// A fixed seed (the reviewed replay's) plus a couple of arbitrary ones.
const SEEDS = [
  'da645ff515777b2c47d1c59937c7dbd637372ef1f2e440cf9867ea9cd2327d5f',
  '0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20',
].map(hex => Uint8Array.from(hex.match(/../g)!.map(b => parseInt(b, 16))));

for (const [si, seed] of SEEDS.entries()) {
  test(`simple_heuristic game replays identically from a pinned deal seed (#${si})`, async () => {
    const a = await playHash('SIMPLE_HEURISTIC', seed, `det-sh-a-${si}`);
    const b = await playHash('SIMPLE_HEURISTIC', seed, `det-sh-b-${si}`);
    assert.equal(a, b, 'same deal seed must produce the same game (draws are deterministic)');
  });

  test(`octogen game replays identically from a pinned deal seed (#${si})`, async () => {
    const a = await playHash('OCTOGEN', seed, `det-og-a-${si}`);
    const b = await playHash('OCTOGEN', seed, `det-og-b-${si}`);
    assert.equal(a, b, 'octogen must be a pure function of the reproducible deal-seeded state');
  });
}

// SECURITY: octogen's play must depend on the SERVER-ONLY game_seed, so an
// attacker who can see the public board (and the deck via pop-order) but not the
// seed cannot reproduce/predict it. Same deal, seed known vs withheld → the
// games must diverge. If they matched, the bot RNG would be a pure function of
// public state and a source-code holder could predict every octogen move.
for (const [si, seed] of SEEDS.entries()) {
  test(`octogen is UNPREDICTABLE without the server-only game_seed (#${si})`, async () => {
    const known    = await playHash('OCTOGEN', seed, `sec-known-${si}`, false);
    const withheld = await playHash('OCTOGEN', seed, `sec-withheld-${si}`, true);
    assert.notEqual(known, withheld,
      'octogen played identically with the seed withheld — its RNG is predictable from public state');
  });
}

test('two DIFFERENT deal seeds produce different games (guard against a stuck deal)', async () => {
  const a = await playHash('SIMPLE_HEURISTIC', SEEDS[0], 'det-diff-a');
  const b = await playHash('SIMPLE_HEURISTIC', SEEDS[1], 'det-diff-b');
  assert.notEqual(a, b);
});
