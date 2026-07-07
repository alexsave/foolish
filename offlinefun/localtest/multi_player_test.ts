import { calculateLegalMoves, registerBotStrategy } from '../../supabase/functions/_shared/bot_strategy.ts';
import { shouldBotActCore, processBotAction } from '../../supabase/functions/_shared/pure_bot_actions.ts';
import { game_done } from '../../supabase/functions/_shared/common_utils.ts';
import { start_game } from '../../supabase/functions/_shared/game_lifecycle.ts';
import { Game, PrivatePlayer, GAME_STATUS, PLAYER_STATUS, STRATEGY_KEY, StrategyKey } from '../../supabase/functions/_shared/types.ts';
import { EspressoStrategy } from './frozen/espresso_strategy.ts';
import { setRandomSeed } from './frozen/random_strategy.ts';

const ESPRESSO = 'espresso' as StrategyKey;
registerBotStrategy(ESPRESSO, new EspressoStrategy());

// Seed Math.random globally so deals/shuffles are deterministic.
let _seed = 1;
const seededRandom = () => {
    _seed = (_seed * 1664525 + 1013904223) % 4294967296;
    return _seed / 4294967296;
};
Math.random = seededRandom;

// Silence
const noop = () => { };
const saved = { log: console.log, warn: console.warn, error: console.error, info: console.info };
console.log = noop;
console.warn = noop;
console.error = noop;
console.info = noop;
const print = saved.log.bind(console);

const createPlayer = (strategy: StrategyKey, index: number): PrivatePlayer => ({
    player_id: `bot_${index}_${strategy}`,
    name: `${strategy} Bot ${index}`,
    status: PLAYER_STATUS.READY,
    is_ai: true,
    hand: [],
    awaiting_attack: false,
    hand_length: 0,
    strategy_key: strategy
});

const createFreshGame = (heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number): Game => {
    const players: PrivatePlayer[] = [createPlayer(heroStrat, 0)];
    for (let i = 0; i < numOpps; i++) {
        players.push(createPlayer(oppStrat, i + 1));
    }
    return {
        players,
        deck: [],
        logs: [],
        id: 'game_1',
        name: 'Game 1',
        status: GAME_STATUS.PLAYING,
        deck_length: 0,
        discard_pile_length: 0,
        flipped: null,
        power_suit: 0,
        first_attacker: 0,
        defender: 0,
        table_battles: [],
        elimination_order: [],
        good_timestamp: null,
        good_players: [],
    };
};

// Hard cap on iterations. Empirically all games of handwritten/random
// finish under 700 iters even at 7-player. 2000 is safe headroom.
const MAX_ITERS = 2000;

// Detect duplicate elimination_order entries (same player marked OUT twice).
function findDuplicateElims(game: Game): string | null {
    const seen = new Set<string>();
    for (const id of game.elimination_order) {
        if (seen.has(id)) return id;
        seen.add(id);
    }
    return null;
}

// Detect duplicate cards across all visible card containers. Returns the offending key, or null if clean.
function findDuplicateCards(game: Game): string | null {
    const seen = new Map<string, string>();
    const key = (c: { suit: number; value: number }) => `${c.value}-${c.suit}`;
    const check = (where: string, cards: { suit: number; value: number }[]) => {
        for (const c of cards) {
            if (c.suit < 0 || c.value < 0) continue; // unknown card sentinel
            const k = key(c);
            if (seen.has(k)) return `dup ${k} in ${seen.get(k)} and ${where}`;
            seen.set(k, where);
        }
        return null;
    };
    for (const p of game.players) {
        const r = check(`hand:${p.name}`, p.hand);
        if (r) return r;
    }
    const tableCards: any[] = [];
    for (const b of game.table_battles) { tableCards.push(b.attack); if (b.defense) tableCards.push(b.defense); }
    let r = check('table', tableCards);
    if (r) return r;
    r = check('deck', game.deck);
    if (r) return r;
    if (game.flipped) {
        r = check('flipped', [game.flipped]);
        if (r) return r;
    }
    return null;
}

// Returns the full elimination_order so the matchup can score by finishing position.
async function runSingleGame(heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number, gameNum?: number): Promise<{ eliminationOrder: string[]; finished: boolean }> {
    const game = createFreshGame(heroStrat, oppStrat, numOpps);
    start_game(game);

    const t0 = Date.now();
    let iters = 0;
    while (game_done(game) === null && iters++ < MAX_ITERS) {
        // Duplicate-card sanity check — fires once per game on first detection.
        const dup = findDuplicateCards(game);
        if (dup) {
            print(`  [DUP CARDS] ${heroStrat} vs ${numOpps} ${oppStrat} game=${gameNum ?? '?'} iter=${iters}: ${dup}`);
            return { eliminationOrder: [], finished: false };
        }
        const dupElim = findDuplicateElims(game);
        if (dupElim) {
            print(`  [DUP ELIM] ${heroStrat} vs ${numOpps} ${oppStrat} game=${gameNum ?? '?'} iter=${iters}: ${dupElim} elim=${JSON.stringify(game.elimination_order)}`);
            return { eliminationOrder: [], finished: false };
        }
        if (Date.now() - t0 > 1000) {
            print(`  [GAME TIMEOUT @ iter=${iters}] ${heroStrat} vs ${numOpps} ${oppStrat} hands=${game.players.map(p => `${p.name}:${p.hand.length}`).join(',')} table=${game.table_battles.length} good=${JSON.stringify(game.good_players)} status=${game.players.map(p => p.status).join(',')}`);
            return { eliminationOrder: [], finished: false };
        }
        const eligibleBots: { bot: PrivatePlayer; index: number }[] = [];
        for (let index = 0; index < game.players.length; index++) {
            const player = game.players[index];
            const shouldAct = shouldBotActCore(game, player, index);
            if (shouldAct) {
                const legalMoves = calculateLegalMoves(game, player.player_id);
                if (legalMoves.length > 0) {
                    eligibleBots.push({ bot: player, index });
                }
            }
        }

        if (eligibleBots.length === 0) break;
        // Fisher-Yates: comparator-based shuffle is non-transitive and can hang V8 sort.
        const shuffledBots = [...eligibleBots];
        for (let i = shuffledBots.length - 1; i > 0; i--) {
            const j = Math.floor(Math.random() * (i + 1));
            [shuffledBots[i], shuffledBots[j]] = [shuffledBots[j], shuffledBots[i]];
        }
        let acted = false;
        for (const selectedBot of shuffledBots) {
            const botActionEvents = await processBotAction(game, selectedBot.bot);
            if (botActionEvents) { acted = true; break; }
        }
        if (!acted) break; // nobody could actually execute — bail
    }

    const loserId = game_done(game);
    if (!loserId) return { eliminationOrder: [], finished: false };
    return { eliminationOrder: [...game.elimination_order], finished: true };
}

// Score: weight = 4^(-position). 1st out = 1.0, 2nd = 0.25, 3rd = 0.0625, ...
function positionScore(eliminationOrder: string[], heroPlayerId: string): number {
    const idx = eliminationOrder.indexOf(heroPlayerId);
    if (idx === -1) return 0; // hero is the fool
    return Math.pow(4, -idx);
}

async function runMatchup(heroStrat: StrategyKey, oppStrat: StrategyKey, numOpps: number, numGames: number): Promise<{ heroScore: number; firstRate: number; runtime: number; unfinished: number }> {
    let totalScore = 0;
    let firstWins = 0;
    let unfinished = 0;
    const heroId = `bot_0_${heroStrat}`;
    const start = Date.now();
    for (let i = 0; i < numGames; i++) {
        _seed = i + 1;
        setRandomSeed(i + 1);
        const t0 = Date.now();
        const result = await runSingleGame(heroStrat, oppStrat, numOpps, i);
        const dt = Date.now() - t0;
        if (dt > 200) print(`    [SLOW] ${heroStrat} vs ${numOpps} ${oppStrat}: game ${i} took ${dt}ms`);
        if (!result.finished) { unfinished++; continue; }
        totalScore += positionScore(result.eliminationOrder, heroId);
        if (result.eliminationOrder[0] === heroId) firstWins++;
    }
    const runtime = (Date.now() - start) / 1000;
    return {
        heroScore: totalScore / numGames,
        firstRate: firstWins / numGames,
        runtime,
        unfinished
    };
}

function fmt(r: { heroScore: number; firstRate: number; runtime: number; unfinished: number }): string {
    return `score=${r.heroScore.toFixed(3)}  first=${(r.firstRate * 100).toFixed(1)}%  (${r.runtime.toFixed(1)}s${r.unfinished ? `, ${r.unfinished} unfinished` : ''})`;
}

(async () => {
    const playerCounts = [1, 2, 3, 4, 5, 6, 7];
    const NUM_GAMES = 500;
    const totalStart = Date.now();

    print('\n========== ESPRESSO vs N RANDOMs ==========');
    for (const n of playerCounts) {
        const r = await runMatchup(ESPRESSO, STRATEGY_KEY.RANDOM, n, NUM_GAMES);
        print(`  vs ${n} random:   espresso ${fmt(r)}`);
    }

    print('\n========== Headline: ESPRESSO vs N COFFEEs ==========');
    for (const n of playerCounts) {
        const r = await runMatchup(ESPRESSO, STRATEGY_KEY.HANDWRITTEN, n, NUM_GAMES);
        print(`  vs ${n} coffee:   espresso ${fmt(r)}`);
    }

    print(`\nTotal time: ${((Date.now() - totalStart) / 1000).toFixed(1)}s`);
})();
