#!/usr/bin/env tsx
import {
    initializeGame,
    applyMove,
    isTerminal,
    getCurrentPlayer,
    getLegalMoves
} from '../../supabase/functions/_shared/durakai/gameEngine';
import { GPTBotStrategy } from '../../supabase/functions/_shared/durakai/gptBotStrategy';
import { HandwrittenBotStrategy } from '../../supabase/functions/_shared/handwritten_strategy';
import { calculateLegalMoves } from '../../supabase/functions/_shared/bot_strategy';

const TEST_GAMES = 10; // Start with fewer games since GPT API is slow
const MAX_MOVES = 500;

/**
 * Play a game between two strategies and return winner index
 */
async function playGame(strategy1: any, strategy2: any): Promise<number> {
    const game = initializeGame(2);
    const strategies = [strategy1, strategy2];
    
    let moveCount = 0;
    
    while (!isTerminal(game) && moveCount < MAX_MOVES) {
        const currentPlayerIndex = getCurrentPlayer(game);
        if (currentPlayerIndex === -1) break;
        
        const player = game.players[currentPlayerIndex];
        const strategy = strategies[currentPlayerIndex];
        
        const legalMoves = calculateLegalMoves(game, player.player_id);
        if (legalMoves.length === 0) break;
        
        const chosenMove = await strategy.chooseMove(game, player.player_id, legalMoves);
        
        // Convert to simple move
        let simpleMove: any;
        if (chosenMove.type === 'attack' && chosenMove.cards && chosenMove.cards.length > 0) {
            simpleMove = { type: 'attack', cards: chosenMove.cards };
        } else if (chosenMove.type === 'cover' && chosenMove.cards && chosenMove.cards.length > 0) {
            simpleMove = { type: 'defend', cards: chosenMove.cards };
        } else if (chosenMove.type === 'pass' && chosenMove.cards && chosenMove.cards.length > 0) {
            simpleMove = { type: 'pass', card: chosenMove.cards[0] };
        } else if (chosenMove.type === 'pickup') {
            simpleMove = { type: 'pickup' };
        } else if (chosenMove.type === 'good') {
            simpleMove = { type: 'good' };
        } else if (chosenMove.type === 'wait') {
            simpleMove = { type: 'wait' };
        } else {
            const fallback = getLegalMoves(game, currentPlayerIndex);
            if (fallback.length === 0) break;
            simpleMove = fallback[0];
        }
        
        applyMove(game, currentPlayerIndex, simpleMove);
        moveCount++;
    }
    
    // Check winner
    if (isTerminal(game)) {
        const finishingOrder = game.logs
            .filter(log => log.log_type === 'player_out')
            .map(log => game.players.findIndex(p => p.player_id === log.player_id));
        
        if (finishingOrder.length > 0) {
            return finishingOrder[0];
        }
    }
    
    return -1; // Draw/timeout
}

/**
 * Test GPT bot against handwritten strategy
 */
async function testGPTBot(): Promise<void> {
    console.log('='.repeat(60));
    console.log('GPT BOT EVALUATION');
    console.log('='.repeat(60));
    console.log('');
    
    // Check API key
    if (!process.env.OPENAI_API_KEY) {
        console.error('❌ OPENAI_API_KEY environment variable not set!');
        console.error('   Set it with: export OPENAI_API_KEY="your-key-here"');
        return;
    }
    
    console.log('Loading GPT bot...');
    const model = process.env.OPENAI_MODEL || 'gpt-5.2';
    const gptBot = new GPTBotStrategy(process.env.OPENAI_API_KEY, model);
    const handwrittenBot = new HandwrittenBotStrategy();
    
    console.log(`\nTesting ${TEST_GAMES} games against handwritten strategy...\n`);
    console.log('⚠️  This will be slow (~30-60 seconds per game due to API calls)\n');
    
    let gptWins = 0;
    let handwrittenWins = 0;
    let draws = 0;
    
    for (let i = 0; i < TEST_GAMES; i++) {
        const startTime = Date.now();
        
        // Alternate who goes first
        const gptFirst = i % 2 === 0;
        const winner = gptFirst 
            ? await playGame(gptBot, handwrittenBot)
            : await playGame(handwrittenBot, gptBot);
        
        const duration = ((Date.now() - startTime) / 1000).toFixed(1);
        
        if (winner === (gptFirst ? 0 : 1)) {
            gptWins++;
        } else if (winner === (gptFirst ? 1 : 0)) {
            handwrittenWins++;
        } else {
            draws++;
        }
        
        const winRate = (gptWins / (i + 1)) * 100;
        console.log(`Game ${i + 1}/${TEST_GAMES} (${duration}s) - Win rate: ${winRate.toFixed(1)}% (${gptWins}W ${handwrittenWins}L ${draws}D)`);
    }
    
    console.log('\n' + '='.repeat(60));
    console.log('RESULTS');
    console.log('='.repeat(60));
    console.log('');
    console.log(`Total Games: ${TEST_GAMES}`);
    console.log(`GPT Bot Wins: ${gptWins} (${(gptWins / TEST_GAMES * 100).toFixed(1)}%)`);
    console.log(`Handwritten Bot Wins: ${handwrittenWins} (${(handwrittenWins / TEST_GAMES * 100).toFixed(1)}%)`);
    console.log(`Draws: ${draws} (${(draws / TEST_GAMES * 100).toFixed(1)}%)`);
    console.log('');
    
    if (gptWins > handwrittenWins) {
        console.log('🎉 GPT bot is winning!');
    } else if (gptWins < handwrittenWins) {
        console.log('📊 Handwritten bot is better.');
    } else {
        console.log('🤝 Even match!');
    }
    
    console.log('\n' + '='.repeat(60));
}

// Run test
testGPTBot().catch(console.error);

