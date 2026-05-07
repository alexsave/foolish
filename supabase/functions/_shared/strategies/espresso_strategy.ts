import { Card, Game, PrivatePlayer, LOG_TYPE, PLAYER_STATUS } from '../types.ts';
import { BotStrategy, LegalMove } from '../bot_interfaces.ts';
import { canCover } from '../common_utils.ts';
import { HandwrittenBotStrategy } from './handwritten_strategy.ts';

// Espresso — perfect-info strategy.
// Branches by current IN-player count. 1v1 uses pass-avoidance + 1-ply
// rollout. 3+ players use position-bias + leader-blocking + (vs random)
// aggressive trump / mixed attacks. Discard memory reconstructs the
// deck from game.logs to enable trump-exhaustion and future-utility checks.
//
// Headline numbers vs handwritten ("coffee"), 5000-game runs:
//   1v1 vs coffee:   espresso 53–54% first
//   1v1 vs random:   espresso 98% first
//   N-player vs N coffees: above uniform across all 3..8 player counts
//   N-player vs N randoms: solidly beats coffee-vs-randoms baseline at all N
interface MemoryState {
    seenCards: Set<string>;
}

const cardKey = (c: Card) => `${c.value}-${c.suit}`;

export class EspressoStrategy implements BotStrategy {
    readonly name = 'espresso';
    private memory = new Map<string, MemoryState>();
    private hw = new HandwrittenBotStrategy();

    private getMem(gameId: string): MemoryState {
        let m = this.memory.get(gameId);
        if (!m) {
            m = { seenCards: new Set() };
            this.memory.set(gameId, m);
        }
        return m;
    }

    private updateDiscardMemory(game: Game) {
        const mem = this.getMem(game.id);
        for (const log of game.logs) {
            if (log.log_type === LOG_TYPE.DISCARD) {
                for (const pair of log.card_pairs) {
                    if (pair.primary) mem.seenCards.add(cardKey(pair.primary));
                }
            }
        }
    }

    private cardScore(card: Card, powerSuit: number): number {
        return card.value + (card.suit === powerSuit ? 1000 : 0);
    }

    private computeTotalCardCount(game: Game): number {
        const tableCount = game.table_battles.reduce((sum, b) => sum + 1 + (b.defense ? 1 : 0), 0);
        const handsCount = game.players.reduce((sum, p) => sum + p.hand.length, 0);
        return game.deck.length + game.discard_pile_length + tableCount + handsCount + (game.flipped ? 1 : 0);
    }

    private getTrumpAttackProbability(game: Game): number {
        if (game.deck.length > 0 || game.flipped !== null) return 0.02;
        const totalCards = Math.max(1, this.computeTotalCardCount(game));
        const discardRatio = Math.max(0, Math.min(1, game.discard_pile_length / totalCards));
        return Math.max(0.5, Math.min(0.95, 0.65 + 0.35 * discardRatio));
    }

    private getOpponent(game: Game, botPlayerId: string): PrivatePlayer | null {
        const inPlayers = game.players.filter(p => p.player_id !== botPlayerId && p.status === PLAYER_STATUS.IN);
        if (inPlayers.length === 0) return null;
        const defender = game.players[game.defender];
        if (defender && defender.player_id !== botPlayerId && defender.status === PLAYER_STATUS.IN) {
            return defender;
        }
        const firstAttacker = game.players[game.first_attacker];
        if (firstAttacker && firstAttacker.player_id !== botPlayerId && firstAttacker.status === PLAYER_STATUS.IN) {
            return firstAttacker;
        }
        return inPlayers[0];
    }

    private predictCover(attack: Card[], oppHand: Card[], powerSuit: number): { coverCards: Card[]; pickup: boolean } {
        const remaining = [...oppHand];
        const coverCards: Card[] = [];
        for (const a of attack) {
            let bestIdx = -1;
            let bestScore = Infinity;
            for (let i = 0; i < remaining.length; i++) {
                if (canCover(a, remaining[i], powerSuit)) {
                    const s = this.cardScore(remaining[i], powerSuit);
                    if (s < bestScore) { bestScore = s; bestIdx = i; }
                }
            }
            if (bestIdx === -1) return { coverCards: [], pickup: true };
            coverCards.push(remaining[bestIdx]);
            remaining.splice(bestIdx, 1);
        }
        return { coverCards, pickup: false };
    }

    private rolloutRound(
        firstAttack: Card[],
        myHand: Card[],
        oppHand: Card[],
        powerSuit: number,
        maxIters: number = 5
    ): { myHand: Card[]; oppHand: Card[]; pickup: boolean } {
        let myH = myHand.slice();
        let oppH = oppHand.slice();
        const tableValues = new Set<number>();
        let attackCards = firstAttack;

        for (let iter = 0; iter < maxIters; iter++) {
            const attackKeys = new Set(attackCards.map(cardKey));
            myH = myH.filter(c => !attackKeys.has(cardKey(c)));
            for (const c of attackCards) tableValues.add(c.value);

            const resp = this.predictCover(attackCards, oppH, powerSuit);
            if (resp.pickup) {
                oppH = [...oppH, ...attackCards];
                return { myHand: myH, oppHand: oppH, pickup: true };
            }
            const coverKeys = new Set(resp.coverCards.map(cardKey));
            oppH = oppH.filter(c => !coverKeys.has(cardKey(c)));
            for (const c of resp.coverCards) tableValues.add(c.value);

            const matching = myH.filter(c => tableValues.has(c.value) && c.suit !== powerSuit);
            if (matching.length === 0) {
                return { myHand: myH, oppHand: oppH, pickup: false };
            }
            const groups = new Map<number, Card[]>();
            for (const c of matching) {
                if (!groups.has(c.value)) groups.set(c.value, []);
                groups.get(c.value)!.push(c);
            }
            let bestGroup: Card[] = [];
            let bestSum = Infinity;
            let bestCount = 0;
            for (const cards of groups.values()) {
                const sum = cards.reduce((s, c) => s + c.value, 0);
                if (cards.length > bestCount || (cards.length === bestCount && sum < bestSum)) {
                    bestCount = cards.length;
                    bestSum = sum;
                    bestGroup = cards;
                }
            }
            attackCards = bestGroup.slice(0, oppH.length);
            if (attackCards.length === 0) {
                return { myHand: myH, oppHand: oppH, pickup: false };
            }
        }
        return { myHand: myH, oppHand: oppH, pickup: false };
    }

    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        if (legalMoves.length === 0) throw new Error('No legal moves available');
        this.updateDiscardMemory(game);

        const opps = game.players.filter(p =>
            p.player_id !== botPlayerId && p.status === PLAYER_STATUS.IN
        );
        const inCount = opps.length + 1;
        const anyRandom = opps.some(p => p.strategy_key === 'random');

        switch (inCount) {
            case 2:  return this.choose2P(game, botPlayerId, legalMoves);
            case 3:  return this.chooseNP(game, botPlayerId, legalMoves, 3, anyRandom);
            case 4:  return this.chooseNP(game, botPlayerId, legalMoves, 4, anyRandom);
            case 5:  return this.chooseNP(game, botPlayerId, legalMoves, 5, anyRandom);
            case 6:  return this.chooseNP(game, botPlayerId, legalMoves, 6, anyRandom);
            case 7:  return this.chooseNP(game, botPlayerId, legalMoves, 7, anyRandom);
            case 8:  return this.chooseNP(game, botPlayerId, legalMoves, 8, anyRandom);
            default: return this.choose2P(game, botPlayerId, legalMoves);
        }
    }

    private async choose2P(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        return this.choose1v1Logic(game, botPlayerId, legalMoves);
    }

    private async chooseNP(game: Game, botPlayerId: string, legalMoves: LegalMove[], inCount: number, anyRandom: boolean): Promise<LegalMove> {
        const bias = this.tryPositionBias(game, botPlayerId, legalMoves);
        if (bias) return bias;
        if (anyRandom) {
            const tweak = this.tryRandomTweaks(game, botPlayerId, legalMoves, inCount);
            if (tweak) return tweak;
            return this.hw.chooseMove(game, botPlayerId, legalMoves);
        }
        return this.choose1v1Logic(game, botPlayerId, legalMoves);
    }

    private async choose1v1Logic(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        const bot = game.players.find(p => p.player_id === botPlayerId)!;
        const opponent = this.getOpponent(game, botPlayerId);

        const continueAttackMoves = legalMoves.filter(m => m.type === 'attack');
        if (continueAttackMoves.length > 0) {
            const nonTrumpAttacks = continueAttackMoves.filter(m => m.cards && m.cards.every(c => c.suit !== game.power_suit));
            const trumpAttacks = continueAttackMoves.filter(m => m.cards && m.cards.some(c => c.suit === game.power_suit));

            let candidateMoves: LegalMove[] = [];
            if (nonTrumpAttacks.length > 0) {
                candidateMoves = nonTrumpAttacks;
            } else if (trumpAttacks.length > 0) {
                let allowTrumpAttack = false;
                if (opponent) {
                    const myLowestTrumpInAttack = Math.min(
                        ...trumpAttacks.flatMap(m => m.cards!).filter(c => c.suit === game.power_suit).map(c => c.value)
                    );
                    const oppCanCoverTrump = opponent.hand.some(c =>
                        c.suit === game.power_suit && c.value > myLowestTrumpInAttack
                    );
                    if (oppCanCoverTrump) allowTrumpAttack = true;
                }
                if (allowTrumpAttack || Math.random() < this.getTrumpAttackProbability(game)) {
                    candidateMoves = trumpAttacks;
                } else {
                    const goodMoves = legalMoves.filter(m => m.type === 'good');
                    if (goodMoves.length > 0) return goodMoves[0];
                }
            }

            if (candidateMoves.length > 0) {
                const passWindow = opponent && game.table_battles.every(b => b.defense === null);
                const inOpps = game.players.filter(p =>
                    p.player_id !== botPlayerId && p.status === PLAYER_STATUS.IN
                );
                const minOppHand = inOpps.length > 0 ? Math.min(...inOpps.map(o => o.hand.length)) : Infinity;
                const defenderIsLeader = game.players[game.defender]?.hand.length === minOppHand
                    && game.players[game.defender]?.status === PLAYER_STATUS.IN
                    && game.players[game.defender]?.player_id !== botPlayerId;
                const leader = inOpps.find(o => o.hand.length === minOppHand) ?? null;
                const leaderIsAttacker = leader && game.players.indexOf(leader) !== game.defender;
                const evalMove = (move: LegalMove): { eval: number; passable: boolean } => {
                    if (!move.cards || !opponent) return { eval: 0, passable: false };
                    const v = move.cards[0].value;
                    const passable = passWindow ? opponent.hand.some(c => c.value === v) : false;

                    const result = this.rolloutRound(move.cards, bot.hand, opponent.hand, game.power_suit);
                    const mySize = result.myHand.length;
                    const oppSize = result.oppHand.length;
                    const myTrumps = result.myHand.filter(c => c.suit === game.power_suit).length;
                    const oppTrumps = result.oppHand.filter(c => c.suit === game.power_suit).length;
                    const deckActive = game.deck.length > 0 || game.flipped !== null;
                    const sizeWeight = (result.pickup || !deckActive) ? 1.0 : 0.0;
                    const pickupBonus = result.pickup ? 3.0 : 0.0;
                    const blockLeaderBonus = (result.pickup && defenderIsLeader) ? 4.0 : 0.0;
                    let leaderPileOnPenalty = 0;
                    if (leaderIsAttacker && leader) {
                        const myV = move.cards[0].value;
                        const leaderMatches = leader.hand.filter(c => c.value === myV).length;
                        leaderPileOnPenalty = leaderMatches * 0.7;
                    }
                    const e = sizeWeight * (oppSize - mySize)
                        + 1.5 * (myTrumps - oppTrumps)
                        + pickupBonus
                        + blockLeaderBonus
                        - leaderPileOnPenalty;
                    return { eval: e, passable };
                };

                const evals = candidateMoves.map(m => {
                    const r = evalMove(m);
                    return { move: m, eval: r.passable ? r.eval - 1000 : r.eval, passable: r.passable };
                });

                let best = evals[0].move;
                let bestEval = evals[0].eval;
                let bestCount = evals[0].move.cards?.length || 0;
                let bestSum = evals[0].move.cards?.reduce((s, c) => s + this.cardScore(c, game.power_suit), 0) ?? Infinity;
                for (const e of evals) {
                    const cnt = e.move.cards?.length || 0;
                    const sum = e.move.cards?.reduce((s, c) => s + this.cardScore(c, game.power_suit), 0) ?? Infinity;
                    const better =
                        e.eval > bestEval ||
                        (e.eval === bestEval && cnt > bestCount) ||
                        (e.eval === bestEval && cnt === bestCount && sum < bestSum);
                    if (better) {
                        bestEval = e.eval;
                        bestCount = cnt;
                        bestSum = sum;
                        best = e.move;
                    }
                }
                return best;
            }
        }

        const passMoves = legalMoves.filter(m => m.type === 'pass');
        if (passMoves.length > 0 && opponent) {
            let best = passMoves[0];
            let bestEval = -Infinity;
            for (const move of passMoves) {
                if (!move.cards) continue;
                const tableAttacks = game.table_battles.map(b => b.attack);
                const allAttacks = [...tableAttacks, ...move.cards];
                const resp = this.predictCover(allAttacks, opponent.hand, game.power_suit);
                const passKeys = new Set(move.cards.map(cardKey));
                const myH = bot.hand.filter(c => !passKeys.has(cardKey(c)));
                let oppH: Card[];
                if (resp.pickup) {
                    oppH = [...opponent.hand, ...allAttacks];
                } else {
                    const coverKeys = new Set(resp.coverCards.map(cardKey));
                    oppH = opponent.hand.filter(c => !coverKeys.has(cardKey(c)));
                }
                const myTrumps = myH.filter(c => c.suit === game.power_suit).length;
                const oppTrumps = oppH.filter(c => c.suit === game.power_suit).length;
                const deckActive = game.deck.length > 0 || game.flipped !== null;
                const sizeWeight = (resp.pickup || !deckActive) ? 1.0 : 0.0;
                const pickupBonus = resp.pickup ? 3.0 : 0.0;
                const e = sizeWeight * (oppH.length - myH.length)
                    + 1.5 * (myTrumps - oppTrumps)
                    + pickupBonus;
                if (e > bestEval) { bestEval = e; best = move; }
            }
            return best;
        }
        if (passMoves.length > 0) return passMoves[0];

        const coverMoves = legalMoves.filter(m => m.type === 'cover');
        if (coverMoves.length > 0) {
            const uncovered = game.table_battles.filter(b => b.defense === null);
            const fullCovers = coverMoves.filter(m => m.attack_cards && m.attack_cards.length === uncovered.length);
            if (fullCovers.length > 0) {
                let best: LegalMove = fullCovers[0];
                let bestEval = -Infinity;
                let bestMax = Infinity;
                let bestSum = Infinity;
                const stillInPlay = [...this.getDeckCards(game)];
                for (const p of game.players) {
                    if (p.player_id === botPlayerId) continue;
                    if (p.status !== PLAYER_STATUS.IN) continue;
                    for (const c of p.hand) stillInPlay.push(c);
                }
                const cardFutureUtility = (c: Card): number => {
                    let n = 0;
                    for (const t of stillInPlay) {
                        if (canCover(t, c, game.power_suit)) n++;
                    }
                    return n;
                };
                const tableValues = new Set<number>();
                for (const b of game.table_battles) {
                    tableValues.add(b.attack.value);
                    if (b.defense) tableValues.add(b.defense.value);
                }
                const otherAttackers = game.players.filter(p =>
                    p.player_id !== botPlayerId &&
                    p.status === PLAYER_STATUS.IN &&
                    game.players.indexOf(p) !== game.defender
                );
                const pileOnRisk = (cover: Card): number => {
                    if (tableValues.has(cover.value)) return 0;
                    let n = 0;
                    for (const a of otherAttackers) {
                        for (const c of a.hand) if (c.value === cover.value) n++;
                    }
                    return n;
                };
                const allOppTrumps = game.players
                    .filter(p => p.player_id !== botPlayerId && p.status === PLAYER_STATUS.IN)
                    .reduce((s, p) => s + p.hand.filter(c => c.suit === game.power_suit).length, 0);
                for (const move of fullCovers) {
                    if (!move.cards) continue;
                    const usedKeys = new Set(move.cards.map(cardKey));
                    const remainingHand = bot.hand.filter(c => !usedKeys.has(cardKey(c)));
                    const myTrumpsAfter = remainingHand.filter(c => c.suit === game.power_suit).length;
                    let defendable = 0;
                    for (const p of game.players) {
                        if (p.player_id === botPlayerId) continue;
                        if (p.status !== PLAYER_STATUS.IN) continue;
                        for (const oc of p.hand) {
                            if (remainingHand.some(d => canCover(oc, d, game.power_suit))) defendable++;
                        }
                    }
                    const disposedUtility = move.cards.reduce((s, c) => s + cardFutureUtility(c), 0);
                    const pileOn = move.cards.reduce((s, c) => s + pileOnRisk(c), 0);
                    const e = defendable * 0.5
                        + 1.5 * (myTrumpsAfter - allOppTrumps)
                        - 0.3 * disposedUtility
                        - 1.0 * pileOn;
                    const scores = move.cards.map(c => this.cardScore(c, game.power_suit));
                    const mx = Math.max(...scores);
                    const sm = scores.reduce((a, b) => a + b, 0);
                    const better =
                        e > bestEval ||
                        (e === bestEval && mx < bestMax) ||
                        (e === bestEval && mx === bestMax && sm < bestSum);
                    if (better) {
                        bestEval = e;
                        bestMax = mx;
                        bestSum = sm;
                        best = move;
                    }
                }
                return best;
            }
        }

        const goodMoves = legalMoves.filter(m => m.type === 'good');
        if (goodMoves.length > 0) return goodMoves[0];

        const doneAttacks = legalMoves.filter(m => m.type === 'attack');
        if (doneAttacks.length > 0) {
            doneAttacks.sort((a, b) => {
                const cd = (b.cards?.length || 0) - (a.cards?.length || 0);
                if (cd !== 0) return cd;
                const aS = (a.cards || []).reduce((s, c) => s + this.cardScore(c, game.power_suit), 0);
                const bS = (b.cards || []).reduce((s, c) => s + this.cardScore(c, game.power_suit), 0);
                return aS - bS;
            });
            return doneAttacks[0];
        }

        const pickupMoves = legalMoves.filter(m => m.type === 'pickup');
        if (pickupMoves.length > 0) return pickupMoves[0];

        return legalMoves[Math.floor(Math.random() * legalMoves.length)];
    }

    private tryPositionBias(game: Game, botPlayerId: string, legalMoves: LegalMove[]): LegalMove | null {
        const inCount = game.players.filter(p => p.status === PLAYER_STATUS.IN).length;
        if (inCount < 3) return null;
        const bot = game.players.find(p => p.player_id === botPlayerId)!;
        const botIndex = game.players.indexOf(bot);
        if (botIndex === game.defender) return null;

        const N = game.players.length;
        const seatsAwayInPlayers = (from: number, to: number): number => {
            let count = 0;
            let i = from;
            for (let safety = 0; safety < N * 2; safety++) {
                i = (i + 1) % N;
                if (i === to) return count + 1;
                if (game.players[i].status === PLAYER_STATUS.IN) count++;
            }
            return -1;
        };
        const seatsFromDefender = seatsAwayInPlayers(game.defender, botIndex);
        const becomesDefenderOnGood = seatsFromDefender === 1;
        const becomesDefenderOnPickup = seatsFromDefender === 2;
        if (!becomesDefenderOnGood && !becomesDefenderOnPickup) return null;

        const continueAttacks = legalMoves.filter(m => m.type === 'attack');
        if (continueAttacks.length === 0) return null;

        const defender = game.players[game.defender];
        const oppHand = defender?.hand ?? [];
        const ps = game.power_suit;
        const canCoverByDefender = (a: Card) => oppHand.some(c => canCover(a, c, ps));

        const filter = becomesDefenderOnPickup
            ? continueAttacks.filter(m => m.cards && m.cards.every(canCoverByDefender))
            : continueAttacks.filter(m => m.cards && m.cards.some(c => !canCoverByDefender(c)));
        if (filter.length === 0) return null;

        filter.sort((a, b) => (b.cards?.length || 0) - (a.cards?.length || 0));
        const maxCt = filter[0].cards?.length || 0;
        const top = filter.filter(m => (m.cards?.length || 0) === maxCt);
        let best = top[0]; let bestSum = Infinity;
        for (const m of top) {
            const s = m.cards!.reduce((a, c) => a + this.cardScore(c, ps), 0);
            if (s < bestSum) { bestSum = s; best = m; }
        }
        return best;
    }

    private tryRandomTweaks(game: Game, botPlayerId: string, legalMoves: LegalMove[], inCount: number): LegalMove | null {
        const bot = game.players.find(p => p.player_id === botPlayerId)!;
        const botIndex = game.players.indexOf(bot);
        const isDefender = botIndex === game.defender;

        const continueAttacks = legalMoves.filter(m => m.type === 'attack');
        if (continueAttacks.length > 0 && !isDefender) {
            const mixed = continueAttacks.filter(m =>
                m.cards && m.cards.length > 1 &&
                m.cards.some(c => c.suit === game.power_suit) &&
                m.cards.some(c => c.suit !== game.power_suit)
            );
            if (mixed.length > 0) {
                mixed.sort((a, b) => (b.cards?.length || 0) - (a.cards?.length || 0));
                const maxCt = mixed[0].cards?.length || 0;
                const top = mixed.filter(m => (m.cards?.length || 0) === maxCt);
                let best = top[0];
                let bestSum = Infinity;
                for (const m of top) {
                    const s = m.cards!.reduce((a, c) => a + c.value, 0);
                    if (s < bestSum) { bestSum = s; best = m; }
                }
                return best;
            }

            const nonTrump = continueAttacks.filter(m => m.cards && m.cards.every(c => c.suit !== game.power_suit));
            if (nonTrump.length === 0) {
                const trumpAttacks = continueAttacks.filter(m => m.cards && m.cards.some(c => c.suit === game.power_suit));
                if (trumpAttacks.length > 0) {
                    let aggressiveProb = Math.min(0.80, 0.20 * (inCount - 1));
                    const totalTrumps = inCount > 4 ? 14 : 10;
                    const seen = this.countSeenTrumps(game);
                    const myTrumps = bot.hand.filter(c => c.suit === game.power_suit).length;
                    const oppVisibleTrumps = game.players
                        .filter(p => p.player_id !== botPlayerId)
                        .reduce((s, p) => s + p.hand.filter(c => c.suit === game.power_suit).length, 0);
                    const flippedTrump = game.flipped && game.flipped.suit === game.power_suit ? 1 : 0;
                    const trumpsInDeck = totalTrumps - seen - myTrumps - oppVisibleTrumps - flippedTrump;
                    if (trumpsInDeck <= 0) aggressiveProb = 0.95;
                    if (Math.random() < aggressiveProb) {
                        trumpAttacks.sort((a, b) => (b.cards?.length || 0) - (a.cards?.length || 0));
                        const maxCt = trumpAttacks[0].cards?.length || 0;
                        const top = trumpAttacks.filter(m => (m.cards?.length || 0) === maxCt);
                        let best = top[0];
                        let bestSum = Infinity;
                        for (const m of top) {
                            const s = m.cards!.reduce((a, c) => a + c.value, 0);
                            if (s < bestSum) { bestSum = s; best = m; }
                        }
                        return best;
                    }
                }
            }
        }

        return null;
    }

    private countSeenTrumps(game: Game): number {
        const mem = this.getMem(game.id);
        let count = 0;
        for (const k of mem.seenCards) {
            const [, suit] = k.split('-');
            if (Number(suit) === game.power_suit) count++;
        }
        return count;
    }

    private getDeckCards(game: Game): Card[] {
        const startValue = game.players.length > 4 ? 1 : 5;
        const known = new Set<string>();
        for (const p of game.players) for (const c of p.hand) known.add(cardKey(c));
        for (const b of game.table_battles) {
            known.add(cardKey(b.attack));
            if (b.defense) known.add(cardKey(b.defense));
        }
        if (game.flipped) known.add(cardKey(game.flipped));
        const mem = this.getMem(game.id);
        for (const k of mem.seenCards) known.add(k);

        const deck: Card[] = [];
        for (let suit = 0; suit < 4; suit++) {
            for (let value = startValue; value <= 14; value++) {
                const k = `${value}-${suit}`;
                if (!known.has(k)) deck.push({ suit, value });
            }
        }
        return deck;
    }
}
