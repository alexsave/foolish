// Writes a realistic mid-game 4-player TS Game (random bots) as JSON for bench.ts.
import { writeFileSync } from 'node:fs';
import { start_game } from '../../../server/api/common/game_lifecycle.ts';
import { Game, PrivatePlayer, PLAYER_STATUS, GAME_STATUS } from '../../../server/api/core/types.ts';
import { shouldBotActCore, processBotAction } from '../../../server/api/common/pure_bot_actions.ts';
import { calculateLegalMoves } from '../../../server/api/common/bot_strategy.ts';
console.log = () => {};
const mkGame = (np: number): Game => ({
    players: Array.from({ length: np }, (_, i) => ({ player_id: `bot_${i}`, name: `Bot ${i}`, status: PLAYER_STATUS.READY,
        is_ai: i !== 0, hand: [], awaiting_attack: false, hand_length: 0, strategy_key: 'random' } as PrivatePlayer)),
    deck: [], logs: [], id: 'bench', name: 'bench', status: GAME_STATUS.PLAYING, deck_length: 0, discard_pile_length: 0,
    flipped: null, power_suit: 0, first_attacker: 0, defender: 0, table_battles: [], elimination_order: [],
    good_timestamp: null, good_players: [],
} as unknown as Game);

async function midGame(): Promise<Game> {
    for (;;) {
        const g = mkGame(4);
        start_game(g);
        for (let guard = 0; guard < 500; guard++) {
            if (g.deck.length <= 12 && g.table_battles.length >= 2) return structuredClone(g);
            const eligible = g.players.filter((p, i) => shouldBotActCore(g, p, i) && calculateLegalMoves(g, p.player_id).length > 0);
            let acted = false;
            for (const p of eligible) if (await processBotAction(g, p)) { acted = true; break; }
            if (!acted) break;
        }
    }
}

midGame().then(g => writeFileSync(process.argv[2], JSON.stringify(g)));
