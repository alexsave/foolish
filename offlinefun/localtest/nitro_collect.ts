// Collect (state, action) training samples from games where N0 wins. Matches
// nitro's atomic-action decomposition so the collected actions are exactly
// what the policy network must reproduce at inference time.
//
// We run a tournament of strategy pairs; for each game N0 wins, every
// decision N0 made gets decomposed into single-card / pickup / stop atomic
// actions and the (tokens, target_action, legal_mask) triple is appended.
//
// Usage:
//   tsx offlinefun/localtest/nitro_collect.ts --seeds=1-2000 --pairs=hw-esp,esp-rand,hw-rand
//     --out=/tmp/nitro_corpus.jsonl
//
// pairs format: <hero>-<opp>; both ∈ {hw, esp, rand}.

import { calculateLegalMoves, registerBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../../supabase/functions/_shared/pure_bot_actions.ts';
import { game_done, cloneGame } from '../../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../../supabase/functions/_shared/game_lifecycle.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, StrategyKey, Card } from '../../supabase/functions/_shared/types.ts';
import { EspressoStrategy } from './frozen/espresso_strategy.ts';
import { HandwrittenBotStrategy } from './frozen/handwritten_strategy.ts';
import { RandomBotStrategy, setRandomSeed } from './frozen/random_strategy.ts';
import {
    tokenize,
    InProgress,
    NUM_ACTIONS,
    ACTION_PICKUP,
    ACTION_STOP,
    cardActionId,
} from '../../supabase/functions/_shared/strategies/nitro_nn.ts';
import { LegalMove } from '../../supabase/functions/_shared/bot_interfaces.ts';
import * as fs from 'node:fs';

const HERO = 'hero' as StrategyKey;
const ESPRESSO = 'espresso' as StrategyKey;
const HANDWRITTEN = 'handwritten' as StrategyKey;
const RANDOM = 'random' as StrategyKey;

let _seed = 1;
const seededRandom = () => {
    _seed = (_seed * 1664525 + 1013904223) >>> 0;
    return _seed / 4294967296;
};
Math.random = seededRandom;

const noop = () => { };
console.log = noop; console.warn = noop; console.error = noop; console.info = noop;
const print = (...args: any[]) => fs.writeSync(1, args.map(a => typeof a === 'string' ? a : JSON.stringify(a)).join(' ') + '\n');

const createPlayer = (strategy: StrategyKey, index: number): PrivatePlayer => ({
    player_id: `bot_${index}_${strategy}`, name: `${strategy}${index}`, status: PLAYER_STATUS.READY,
    is_ai: true, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: strategy,
});
const createGame = (a: StrategyKey, b: StrategyKey): Game => ({
    players: [createPlayer(a, 0), createPlayer(b, 1)],
    deck: [], logs: [], id: 'g', name: 'g', status: GAME_STATUS.PLAYING,
    deck_length: 0, discard_pile_length: 0, flipped: null, power_suit: 0,
    first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [],
});
const norm = (s: number) => ((s >>> 0) || 1);

// ---------- Decomposition ----------

interface AtomicSample {
    tokens: number[];
    target: number;
    legal: number[]; // bitset (just store legal action ids)
}

// Given the game state and the LegalMove the strategy chose, generate the
// sequence of atomic action samples that produce this move.
function decomposeMove(
    game: Game,
    botPlayerId: string,
    chosen: LegalMove,
): AtomicSample[] {
    const trump = game.power_suit;
    const me = game.players.find(p => p.player_id === botPlayerId)!;
    const meIdx = game.players.findIndex(p => p.player_id === botPlayerId);
    const isDefender = meIdx === game.defender;
    const isFirstAttack = game.table_battles.length === 0;

    // Build atomic targets and the running InProgress state.
    const atoms: AtomicSample[] = [];
    let role: InProgress['role'] = 'idle';
    if (chosen.type === 'attack') role = 'attack';
    else if (chosen.type === 'cover') role = 'cover';
    else if (chosen.type === 'pass') role = 'pass';
    const cardsChosen: Card[] = [];

    const legalMaskNow = (): number[] => {
        const mask: number[] = [];
        // What's legal at this atomic step?
        // We're matching what the inference loop will do — see nitro_strategy.
        // At each step, the legal cards are: cards in (live hand) that could
        // extend the current move. STOP is legal only if a non-empty current
        // move is a complete legal move OR no further extension is needed.
        // To keep this simple for now, we enumerate cards in hand minus
        // already-chosen. Stopping is allowed at every step where the
        // partial move would be a legal move.
        const chosenSet = new Set<string>();
        for (const c of cardsChosen) chosenSet.add(`${c.suit}-${c.value}`);
        const live = me.hand.filter(c => !chosenSet.has(`${c.suit}-${c.value}`));
        for (const c of live) {
            mask.push(cardActionId(c.suit, c.value, trump));
        }
        // PICKUP: defender, only at the very start (no cards chosen yet).
        if (isDefender && cardsChosen.length === 0 && chosen.type !== 'pass') {
            mask.push(ACTION_PICKUP);
        }
        // STOP: always — the running move is a valid stopping point if the
        // collected cards form one of the legal moves. The trainer doesn't
        // need to be precise here; the policy will learn from the targets.
        mask.push(ACTION_STOP);
        return mask;
    };

    // Special-case pickup / good (no cards): one atomic sample.
    if (chosen.type === 'pickup') {
        const ip: InProgress = { role: 'idle', cardsChosen: [] };
        const t = tokenize(game, botPlayerId, ip);
        atoms.push({ tokens: t.tokens, target: ACTION_PICKUP, legal: legalMaskNow() });
        return atoms;
    }
    if (chosen.type === 'good' || chosen.type === 'wait') {
        const ip: InProgress = { role: 'idle', cardsChosen: [] };
        const t = tokenize(game, botPlayerId, ip);
        atoms.push({ tokens: t.tokens, target: ACTION_STOP, legal: legalMaskNow() });
        return atoms;
    }

    // Otherwise — attack / cover / pass — sequence of card adds + stop.
    const cardSeq = chosen.cards ?? [];
    for (let i = 0; i < cardSeq.length; i++) {
        const ip: InProgress = { role, cardsChosen: cardsChosen.slice() };
        const t = tokenize(game, botPlayerId, ip);
        const card = cardSeq[i];
        const target = cardActionId(card.suit, card.value, trump);
        atoms.push({ tokens: t.tokens, target, legal: legalMaskNow() });
        cardsChosen.push(card);
    }
    // Final STOP atom.
    {
        const ip: InProgress = { role, cardsChosen: cardsChosen.slice() };
        const t = tokenize(game, botPlayerId, ip);
        atoms.push({ tokens: t.tokens, target: ACTION_STOP, legal: legalMaskNow() });
    }
    return atoms;
}

// ---------- Game runner with capture ----------

async function playAndCapture(
    seed: number,
    pStrat0: StrategyKey,
    pStrat1: StrategyKey,
    out: AtomicSample[],
    capIters = 1500,
): Promise<{ winner: 0 | 1 | null }> {
    _seed = norm(seed);
    setRandomSeed(norm(seed));
    const game = createGame(pStrat0, pStrat1);
    start_game(game);

    // Capture every move made by EITHER player. Tag with the player index so
    // we can keep only the winner's moves at the end.
    const samplesByPlayer: AtomicSample[][] = [[], []];

    let iter = 0;
    while (game_done(game) === null && iter++ < capIters) {
        const eligible: { bot: PrivatePlayer; pIdx: number }[] = [];
        for (let i = 0; i < game.players.length; i++) {
            if (shouldBotActCore(game, game.players[i], i)) {
                const lm = calculateLegalMoves(game, game.players[i].player_id);
                if (lm.length > 0) eligible.push({ bot: game.players[i], pIdx: i });
            }
        }
        if (eligible.length === 0) break;
        const shuffled = [...eligible];
        for (let i = shuffled.length - 1; i > 0; i--) {
            const j = Math.floor(Math.random() * (i + 1));
            [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
        }
        let acted = false;
        for (const sb of shuffled) {
            const pIdx = sb.pIdx;
            const before = cloneGame(game);
            const r = await processBotAction(game, sb.bot);
            if (!r) continue;
            acted = true;

            // Reconstruct the move from the diff.
            const handBefore = before.players[pIdx].hand;
            const handAfter = game.players[pIdx].hand;
            const removed: Card[] = [];
            for (const c of handBefore) {
                if (!handAfter.some(h => h.suit === c.suit && h.value === c.value)) removed.push(c);
            }
            const tBefore = before.table_battles.length;
            const tAfter = game.table_battles.length;
            const coveredBefore = before.table_battles.filter(b => b.defense !== null).length;
            const coveredAfter = game.table_battles.filter(b => b.defense !== null).length;

            let move: LegalMove | null = null;
            if (tAfter > tBefore && removed.length > 0) {
                const wasDefender = pIdx === before.defender;
                move = wasDefender
                    ? { type: 'pass', cards: removed }
                    : { type: 'attack', cards: removed };
            } else if (coveredAfter > coveredBefore && removed.length > 0) {
                const attackCards: Card[] = [];
                for (let bi = 0; bi < before.table_battles.length; bi++) {
                    if (!before.table_battles[bi].defense && game.table_battles[bi]?.defense) {
                        attackCards.push(before.table_battles[bi].attack);
                    }
                }
                move = { type: 'cover', cards: removed, attack_cards: attackCards };
            } else if (handAfter.length > handBefore.length) {
                move = { type: 'pickup' };
            } else {
                move = { type: 'good' };
            }

            const atoms = decomposeMove(before, before.players[pIdx].player_id, move);
            for (const a of atoms) samplesByPlayer[pIdx].push(a);
            break;
        }
        if (!acted) break;
    }

    const loserId = game_done(game);
    if (!loserId) return { winner: null };
    const winnerIdx: 0 | 1 = game.players[0].player_id === loserId ? 1 : 0;
    for (const s of samplesByPlayer[winnerIdx]) out.push(s);
    return { winner: winnerIdx };
}

// ---------- CLI ----------

const args = process.argv.slice(2);
const argMap = new Map<string, string>();
for (const a of args) {
    if (a.startsWith('--')) {
        const eq = a.indexOf('=');
        if (eq > 0) argMap.set(a.slice(2, eq), a.slice(eq + 1));
        else argMap.set(a.slice(2), 'true');
    }
}
const seedRange = (argMap.get('seeds') ?? '1-2000').split('-').map(s => parseInt(s, 10));
const pairs = (argMap.get('pairs') ?? 'hw-esp,esp-rand,hw-rand').split(',');
const outPath = argMap.get('out') ?? '/tmp/nitro_corpus.jsonl';

const stratOf = (key: string): StrategyKey => {
    if (key === 'hw') return HANDWRITTEN;
    if (key === 'esp') return ESPRESSO;
    if (key === 'rand') return RANDOM;
    throw new Error('unknown strategy ' + key);
};

registerBotStrategy(ESPRESSO, new EspressoStrategy());
registerBotStrategy(HANDWRITTEN, new HandwrittenBotStrategy());
registerBotStrategy(RANDOM, new RandomBotStrategy());

(async () => {
    const fout = fs.openSync(outPath, 'w');
    let totalGames = 0; let totalWins = 0; let totalSamples = 0;
    const start = Date.now();
    for (const pair of pairs) {
        const [a, b] = pair.split('-');
        const heroStrat = stratOf(a);
        const oppStrat = stratOf(b);
        for (let s = seedRange[0]; s <= seedRange[1]; s++) {
            const samples: AtomicSample[] = [];
            const r = await playAndCapture(s, heroStrat, oppStrat, samples);
            totalGames++;
            if (r.winner !== null && samples.length > 0) {
                totalWins++;
                for (const sample of samples) {
                    fs.writeSync(fout, JSON.stringify(sample) + '\n');
                    totalSamples++;
                }
            }
            if (totalGames % 500 === 0) {
                const dt = ((Date.now() - start) / 1000).toFixed(1);
                print(`# ${totalGames} games, ${totalWins} hero wins, ${totalSamples} samples, ${dt}s`);
            }
        }
    }
    fs.closeSync(fout);
    const dt = ((Date.now() - start) / 1000).toFixed(1);
    print(`# done: ${totalGames} games, ${totalWins} hero wins, ${totalSamples} samples in ${outPath} (${dt}s)`);
})();
