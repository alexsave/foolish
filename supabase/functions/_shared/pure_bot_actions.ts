// version of bot_actions that doesn't use util.ts
// that has a whole lot of baggage in it
// Import shared action handlers with validation
import { Game, PrivatePlayer, Bot, GAME_STATUS, PLAYER_STATUS } from './types.ts';
import { calculateLegalMoves, getBotStrategy, LegalMove, WasmBotStrategy } from './bot_strategy.ts';
import { handleAttack } from './actions/attack.ts';
import { handleCover } from './actions/cover.ts';
import { handlePass } from './actions/pass.ts';
import { handlePickup } from './actions/pickup.ts';
import { handleGood } from './actions/good.ts';
import { AnimationEvent } from './types.ts';
import { cardDisplay } from './common_utils.ts';

// Process a single bot's action. Returns the chosen move too, so the caller
// can replay it cheaply if its CAS commit conflicts (see bot_actions.ts).
export const processBotAction = async (game: Game, bot: PrivatePlayer): Promise<false | { events: AnimationEvent[], moveType: string, move: LegalMove }> => {
    try {
        const botActionStartTime = Date.now();
        
        // Get bot's strategy
        const strategy = getBotStrategy(bot.strategy_key);

        let chosenMove: LegalMove;
        if (strategy instanceof WasmBotStrategy) {
            // Kernel-backed bots enumerate AND choose inside the C kernel in
            // one call; only the chosen move crosses back. Skips the full
            // enumerate-export-parse move-list round trip below, which the
            // pipeline profile showed costing more than the decisions
            // themselves for the cheap strategies.
            const direct = strategy.chooseMoveDirect(game, bot.player_id);
            if (!direct) {
                console.log(`No legal moves for bot ${bot.name}`);
                return false;
            }
            chosenMove = direct;
        } else {
            // Calculate legal moves for this bot
            const legalMoves = calculateLegalMoves(game, bot.player_id);

            if (legalMoves.length === 0) {
                console.log(`No legal moves for bot ${bot.name}`);
                return false;
            }

            // Let the strategy choose a move
            // This is ok to keep async, we might be calling LLMs later
            chosenMove = await strategy.chooseMove(game, bot.player_id, legalMoves);
        }

        // Execute the chosen move using shared actions (with validation to handle race conditions)
        const actionEvents = executeBotMove(game, bot, chosenMove);

        if (!actionEvents) {
            return false;
        }

        const totalBotActionTime = Date.now() - botActionStartTime;
        //console.log(`[TIMING] Total bot action time for ${bot.name}: ${totalBotActionTime}ms`);
        return { events: actionEvents, moveType: chosenMove.type, move: chosenMove };

    } catch (error) {
        console.error(`Error processing bot action for ${bot.name}:`, error);
        return false;
    }
}

// Execute a bot's chosen move using shared action handlers
export const executeBotMove = (game: Game, bot: PrivatePlayer, move: LegalMove): false | AnimationEvent[] => {
    try {
        let specialMessage: string | undefined;
        let actionEvents: AnimationEvent[] = [];

        try {
            const actionHandlerStart = Date.now();
            //console.log(`[TIMING] Executing ${move.type} action...`);
            
            switch (move.type) {
                case 'attack':
                    actionEvents = handleAttack(game, bot.player_id, move.cards!);
                    console.log(`⚔️  ${bot.name} attacked with ${move.cards!.map(c => cardDisplay(c)).join(', ')}`);
                    break;

                case 'cover':
                    actionEvents = handleCover(game, bot.player_id, move.cards!, move.attack_cards!);
                    console.log(`🛡️  ${bot.name} covered ${move.attack_cards!.map(c => cardDisplay(c)).join(', ')} with ${move.cards!.map(c => cardDisplay(c)).join(', ')}`);
                    // Check if this cover completed the round (all attacks covered)
                    // Note: every() returns true for empty arrays, but we should have battles after covering
                    const all_attacks_covered = game.table_battles.length > 0 && 
                        game.table_battles.every(battle => battle.defense !== null);
                    if (all_attacks_covered) {
                        // Add special message for successful cover that ends the round
                        specialMessage = `Bot ${bot.name} successfully covered and ended the round`;
                    }
                    break;

                case 'pass':
                    actionEvents = handlePass(game, bot.player_id, move.cards!);
                    console.log(`↪️  ${bot.name} passed with ${move.cards!.map(c => cardDisplay(c)).join(', ')}`);
                    break;

                case 'pickup':
                    actionEvents = handlePickup(game, bot.player_id);
                    console.log(`📥 ${bot.name} picked up`);
                    break;

                case 'good':
                    actionEvents = handleGood(game, bot.player_id);
                    // Log is already in handleGood with waiting conditions details
                    break;

                case 'wait':
                    // Bot chooses to wait - no action needed
                    break;
            }

            //console.log(`[TIMING] Action handler (${move.type}) completed in ${Date.now() - actionHandlerStart}ms`);
            //console.log(`Bot ${bot.name} performed ${move.type} action`);

        } catch (error) {
            // Handle validation failures gracefully (e.g., due to race conditions)
            console.log(`Bot ${bot.name} move validation failed: ${error.stack} - this can happen due to race conditions and is normal`);
            return false;
        }

        return actionEvents;

    } catch (error) {
        console.error(`Error executing bot move for ${bot.name}:`, error);
        return false;
    }
}
// ---------------------------------------------------------------------------
// Packed bot moves (docs/PACKED_WIRE_CUTOVER.md): the server loop's path.
// The chosen move becomes an awire buffer applied in ONE kernel session —
// validate + apply + win-finalize + per-viewer masked event streams + the
// masked log records — with no JS AnimationEvents, no appendLogs, and no
// second marshal for kernel-brained bots (wasmChooseMove leaves the resident
// state valid; runPackedGameAction's marshal consumes that skip).
// The legacy processBotAction/executeBotMove above stay for the offline
// harnesses and e2e dispatch, which drive whole games on JS objects.
// ---------------------------------------------------------------------------
import { AwireKindName, encodeAction } from './sdk/ts/wire/awire.ts';
import { applyKernelStateToGame, PackedRunOk, runPackedGameAction } from './sdk/ts/wasm/engine.ts';

export interface PackedBotMove {
    // null for 'wait' — no action to apply, nothing to commit beyond the
    // (unchanged) game, exactly like the legacy empty-events wait.
    run: PackedRunOk | null;
    moveType: string;
    move: LegalMove;
}

export const executeBotMovePacked = (game: Game, bot: PrivatePlayer, move: LegalMove): false | PackedBotMove => {
    if (move.type === 'wait') return { run: null, moveType: 'wait', move };
    try {
        const seat = game.players.findIndex(p => p.player_id === bot.player_id);
        if (seat < 0) return false;
        let aiMask = 0;
        const humanSeats: number[] = [];
        game.players.forEach((p, i) => { if (p.is_ai) aiMask |= 1 << i; else humanSeats.push(i); });
        const carriesCards = move.type === 'attack' || move.type === 'cover' || move.type === 'pass';
        const wire = encodeAction({
            kind: move.type as AwireKindName,
            cards: carriesCards ? move.cards : undefined,
            attack_cards: move.type === 'cover' ? move.attack_cards : undefined,
        });
        const run = runPackedGameAction(game, seat, wire, aiMask, humanSeats);
        if (!run.ok) {
            // Mirrors the legacy validation-failure path: races are normal.
            console.log(`Bot ${bot.name} ${move.type} rejected by kernel (reason ${run.reason}) - this can happen due to race conditions and is normal`);
            return false;
        }
        // The loop holds a reference to `game` across the cycle — apply the
        // post-action (post-finalize) kernel state onto it in place.
        applyKernelStateToGame(game, run.post, bot.player_id);
        console.log(`✓ ${bot.name} performed ${move.type} (packed)`);
        return { run, moveType: move.type, move };
    } catch (error) {
        console.error(`Error executing packed bot move for ${bot.name}:`, error);
        return false;
    }
};

// processBotAction's packed twin: choose (kernel-direct for wasm bots, JS
// strategy for gpt/console) then apply through the kernel session above.
export const processBotActionPacked = async (game: Game, bot: PrivatePlayer): Promise<false | PackedBotMove> => {
    try {
        const strategy = getBotStrategy(bot.strategy_key);
        let chosenMove: LegalMove;
        if (strategy instanceof WasmBotStrategy) {
            const direct = strategy.chooseMoveDirect(game, bot.player_id);
            if (!direct) {
                console.log(`No legal moves for bot ${bot.name}`);
                return false;
            }
            chosenMove = direct;
        } else {
            const legalMoves = calculateLegalMoves(game, bot.player_id);
            if (legalMoves.length === 0) {
                console.log(`No legal moves for bot ${bot.name}`);
                return false;
            }
            chosenMove = await strategy.chooseMove(game, bot.player_id, legalMoves);
        }
        return executeBotMovePacked(game, bot, chosenMove);
    } catch (error) {
        console.error(`Error processing packed bot action for ${bot.name}:`, error);
        return false;
    }
};

// Turn-eligibility projection: lives in common_utils.ts (a client-safe leaf
// module — this file pulls the action handlers and bot strategies, i.e. both
// wasm embeds, so the client must never import it). Re-exported here for the
// server bot loop and existing test imports.
export { shouldBotActCore } from './common_utils.ts';