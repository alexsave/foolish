import { Card, Game, PrivatePlayer, PLAYER_STATUS } from '../types.ts';
import { BotStrategy, LegalMove } from '../bot_interfaces.ts';
import { canCover } from '../common_utils.ts';

// Nitro — built from scratch, growing one principle at a time. Every change
// is validated against the full previously-passing range before advancing
// the frontier.

// ---- card-level costs ----------------------------------------------------

// Cost we want to MINIMIZE when committing cards in different contexts.
// Attacks / passes — the card is being thrown into the round. Low-value
// non-trumps are cheapest; trumps get a heavy penalty so we don't fritter
// them on small exchanges.
const offensiveCardCost = (card: Card, powerSuit: number): number =>
    card.suit === powerSuit ? 100 + card.value : card.value;

// Cover cost: prefer the TIGHTEST card. Same-suit minimum-gap, else
// lowest trump.
const coverPairCost = (attack: Card, defense: Card, powerSuit: number): number => {
    if (defense.suit === powerSuit && attack.suit !== powerSuit) {
        return 100 + defense.value;
    }
    return defense.value - attack.value;
};

const offensiveMoveCost = (move: LegalMove, powerSuit: number): number => {
    if (!move.cards || move.cards.length === 0) return 0;
    let s = 0;
    for (const c of move.cards) s += offensiveCardCost(c, powerSuit);
    return s;
};

const coverMoveCost = (move: LegalMove, powerSuit: number): number => {
    if (!move.cards || move.cards.length === 0) return 0;
    if (!move.attack_cards || move.attack_cards.length !== move.cards.length) {
        return offensiveMoveCost(move, powerSuit);
    }
    let s = 0;
    for (let i = 0; i < move.cards.length; i++) {
        s += coverPairCost(move.attack_cards[i], move.cards[i], powerSuit);
    }
    return s;
};

const cheapest = <T>(items: T[], cost: (t: T) => number): T =>
    items.reduce((best, m) => cost(m) < cost(best) ? m : best);

// Greedy check: can `hand` cover EVERY attack in `attacks`? Cover with
// cheapest available card per attack (lowest same-suit higher, else
// lowest trump). No backtracking — good enough to tell pickup-vs-cover.
const canFullyCover = (attacks: Card[], hand: Card[], powerSuit: number): boolean => {
    const remaining = [...hand];
    for (const a of attacks) {
        let bestIdx = -1;
        let bestCost = Infinity;
        for (let i = 0; i < remaining.length; i++) {
            const c = remaining[i];
            if (!canCover(a, c, powerSuit)) continue;
            const cost = c.suit === powerSuit && a.suit !== powerSuit
                ? 100 + c.value
                : c.value - a.value;
            if (cost < bestCost) {
                bestCost = cost;
                bestIdx = i;
            }
        }
        if (bestIdx === -1) return false;
        remaining.splice(bestIdx, 1);
    }
    return true;
};

const findOpponent = (game: Game, botPlayerId: string): PrivatePlayer | null => {
    for (const p of game.players) {
        if (p.player_id !== botPlayerId && p.status === PLAYER_STATUS.IN) return p;
    }
    return null;
};

const trumpCount = (hand: Card[], powerSuit: number): number => {
    let c = 0;
    for (const card of hand) if (card.suit === powerSuit) c++;
    return c;
};

// ---- decision logic ------------------------------------------------------

export class NitroStrategy implements BotStrategy {
    readonly name = 'nitro';

    async chooseMove(game: Game, botPlayerId: string, legalMoves: LegalMove[]): Promise<LegalMove> {
        const trump = game.power_suit;
        const opp = findOpponent(game, botPlayerId);
        const me = game.players.find(p => p.player_id === botPlayerId)!;

        // Game-progression variables. The same move can be right or wrong
        // depending on these, so the rules below branch on them.
        const deckLeft = game.deck.length + (game.flipped ? 1 : 0);
        const deckEmpty = deckLeft === 0;
        const myTrumps = trumpCount(me.hand, trump);
        const oppTrumps = opp ? trumpCount(opp.hand, trump) : 0;
        const myHandSize = me.hand.length;
        const oppHandSize = opp ? opp.hand.length : 0;

        // ---------- DEFENDER ----------
        // Principle 14 (defender): pass beats cover whenever pass is legal.
        //   Why (general Durak): passing is the only way to flip the
        //   defender role in 1v1; the opp now has to handle the table
        //   AND the card we sent. If they pickup, our pass card lands
        //   in their hand. If they cover, they spent a card. Either way
        //   we gained tempo.
        //   Counter (opp can full-cover via lots of trumps): pass to a
        //   trump-rich opponent is wasteful — they'll absorb both
        //   attacks AND end up with our pass card discarded. We narrow
        //   the rule below to cases where the opp CAN'T fully cover the
        //   resulting table, OR we have multiple matching cards (the
        //   pass with multi-card hits hard).
        //   Counter (multi-opponent): pass goes to "next defender" not
        //   necessarily the same opponent — out of scope for now.
        const passMovesAsDefender = legalMoves.filter(m => m.type === 'pass');
        if (passMovesAsDefender.length > 0 && opp) {
            const passForcing = passMovesAsDefender.filter(m => {
                const tableAttacks = game.table_battles.map(b => b.attack);
                const allAttacks = [...tableAttacks, ...(m.cards ?? [])];
                return !canFullyCover(allAttacks, opp.hand, trump);
            });
            if (passForcing.length > 0) {
                return cheapest(passForcing, m => offensiveMoveCost(m, trump));
            }
        }

        // Principle 11 (defender, ENDGAME): in deck=0, prefer pass over
        // cover IF the opponent will be forced to pickup the resulting
        // table. With perfect info we check this directly.
        //   Why: pass transfers our card AND the burden. If the opp
        //   then has to pickup, our pass card joins their hand — they
        //   gain a card while we shed one. This is decisive in deck=0.
        //   Counter: when the opp can clean-cover the passed table
        //   (especially with trumps they were going to lose anyway),
        //   passing just helps them dump cards to discard. Tested
        //   against espresso: indiscriminate pass-over-cover leaks
        //   games where the opp has enough trumps to fully cover.
        //   Counter: in N>2 player games "next defender" isn't always
        //   the strategic target — out of scope for now.
        if (deckEmpty && opp) {
            const passInEndgame = legalMoves.filter(m => m.type === 'pass');
            const passForcing = passInEndgame.filter(m => {
                const tableAttacks = game.table_battles.map(b => b.attack);
                const allAttacks = [...tableAttacks, ...(m.cards ?? [])];
                return !canFullyCover(allAttacks, opp.hand, trump);
            });
            if (passForcing.length > 0) {
                return cheapest(passForcing, m => offensiveMoveCost(m, trump));
            }
        }

        const coverMoves = legalMoves.filter(m => m.type === 'cover');
        if (coverMoves.length > 0) {
            // Principle 15 (defender): never partial-cover. Either cover
            // EVERY uncovered attack or pickup.
            //   Why (general Durak): partial covers are usually a step
            //   toward pickup anyway. Each card you commit to a partial
            //   cover ends up back in your hand (along with everything
            //   else on the table) when you eventually pickup — except
            //   the round may have grown by then. Better to pickup now
            //   while the table is small.
            //   Counter: when you can fully cover, this rule is a no-op
            //   (full covers are still legal cover moves).
            //   Counter (multi-opp): in 3+ player games partial covers
            //   sometimes shift the round to a teammate. Out of scope.
            const uncoveredAttacks = game.table_battles.filter(b => b.defense === null).length;
            const fullCoverPool = uncoveredAttacks <= 1
                ? coverMoves
                : coverMoves.filter(m => (m.cards?.length ?? 0) === uncoveredAttacks);
            if (fullCoverPool.length === 0) {
                const pickup = legalMoves.find(m => m.type === 'pickup');
                if (pickup) return pickup;
            }
            const candidatePool = fullCoverPool.length > 0 ? fullCoverPool : coverMoves;
            // Principle 1: cover beats pickup.
            // Principle 3: pick the TIGHTEST cover (cover-cost function).
            //
            // Principle 9 (defender, perfect info): among similarly-priced
            // covers, prefer cards whose VALUE the opponent does NOT hold.
            //   Why: covering with a card adds that value to the table.
            //   The attacker can immediately follow up with any matching-
            //   value card from their hand. If those followups include a
            //   trump or another card we can't cover, our defense
            //   collapses next turn. Using a value the opponent doesn't
            //   hold defuses that vector.
            //   Counter (N>2 player games): findOpponent returns the
            //   first IN opponent — for 1v1 that's the unique opponent;
            //   for multi-opp games this is a noisy but still-useful
            //   signal. Multi-opponent-aware reasoning is future work.
            const oppMatchPenalty = (move: LegalMove): number => {
                if (!opp || !move.cards) return 0;
                let p = 0;
                for (const c of move.cards) {
                    for (const oc of opp.hand) {
                        if (oc.value === c.value) p++;
                    }
                }
                return 30 * p;
            };
            const bestCover = cheapest(
                candidatePool,
                m => coverMoveCost(m, trump) + oppMatchPenalty(m),
            );

            // Principle 10 (defender, perfect info): if the best cover
            // burns a trump AND the opponent holds enough same-value
            // cards to launch a follow-up attack we can't cover,
            // pickup instead.
            //   Why: covering with T♥ (trump) on J♣ teaches the table
            //   the value 10. If opp has T♦ and T♣, they pile both
            //   onto the next attack — and if our remaining hand has
            //   no trump, T♣ becomes uncoverable. Pickup now is
            //   smaller (just current table cards) than the disaster
            //   we'd face two moves later.
            //   Counter (deck still has cards): refill softens early
            //   pickups; the trade is worse before deck=0. Restrict
            //   to deck.length small.
            //   Counter (no pickup option): only triggers when pickup
            //   is legal.
            //   Counter (cover doesn't burn trump): we only act when
            //   the cover commits a trump that would be otherwise
            //   useful for future defenses.
            const pickupMove = legalMoves.find(m => m.type === 'pickup');
            if (pickupMove && opp) {
                const cards = bestCover.cards ?? [];
                const trumpInCover = cards.find(c => c.suit === trump);
                if (trumpInCover) {
                    // Predict follow-up: after cover, table values include
                    // attacks + cover cards. Opp can attack any matching
                    // value. If any such attack has no cover left in hand,
                    // we'd be forced to pickup later anyway — and it'd
                    // be a bigger pickup.
                    const tableValues = new Set<number>();
                    for (const b of game.table_battles) {
                        tableValues.add(b.attack.value);
                        if (b.defense) tableValues.add(b.defense.value);
                    }
                    for (const c of cards) tableValues.add(c.value);
                    const trumpsLeft = myTrumps - cards.filter(c => c.suit === trump).length;
                    const handAfter = me.hand.filter(h => !cards.some(c => c.value === h.value && c.suit === h.suit));

                    // A "savable" threat: an opp followup we can't cover
                    // AFTER spending the trump, but COULD have covered if
                    // we kept it. If the threat survives even with the
                    // trump in hand, pickup doesn't help us — cover.
                    let savableThreat = false;
                    for (const oc of opp.hand) {
                        if (!tableValues.has(oc.value)) continue;
                        const canDefendAfter = handAfter.some(h => canCover(oc, h, trump));
                        if (canDefendAfter) continue;
                        const canDefendNow = me.hand.some(h => canCover(oc, h, trump));
                        if (canDefendNow) {
                            savableThreat = true;
                            break;
                        }
                    }

                    // Swap to pickup when:
                    //   (a) the threat is savable by keeping the trump,
                    //   (b) only ONE attack is currently uncovered — multi-
                    //       attack scenarios are usually fully coverable
                    //       and discarding the table is far better than
                    //       picking up multiple cards,
                    //   (c) we're not already trump-rich vs the opponent.
                    const uncoveredNow = game.table_battles.filter(b => b.defense === null).length;
                    if (savableThreat && uncoveredNow === 1 && trumpsLeft <= oppTrumps) {
                        return pickupMove;
                    }

                }
            }

            return bestCover;
        }

        // ---------- ATTACKER ----------
        const attackMoves = legalMoves.filter(m => m.type === 'attack');
        const goodMove = legalMoves.find(m => m.type === 'good');

        if (attackMoves.length > 0) {
            // Principle 16 (attacker): in early game (deck still has
            // cards), decline attacks that ONLY contain trumps — say
            // "good" instead.
            //   Why (general Durak): trumps are scarce; attacking with
            //   them means a defender either trumps higher (we lose
            //   tempo) or picks up the trump (they gain ammunition).
            //   Both bad early. Wait for the endgame.
            //   Counter (no good available — first attacker on round
            //   start, must attack): we have no choice; fall through.
            //   Counter (deck empty): trumps are now needed for offense
            //   and defense alike — endgame branches handle this below.
            if (!deckEmpty && goodMove) {
                const hasNonTrumpAttack = attackMoves.some(
                    m => (m.cards ?? []).every(c => c.suit !== trump),
                );
                if (!hasNonTrumpAttack) return goodMove;
            }

            // Principle 6 (attacker, perfect-info, ENDGAME only): prefer
            // attacks the defender CANNOT fully cover. Restrict to non-trump
            // forcings — pushing a trump into the defender's hand on pickup
            // arms them with a card we can't beat.
            //   Why: while the deck still feeds both players, a forced
            //   pickup just refills both back to 6 — gain is small. Once
            //   the deck is empty, the defender's hand is permanently
            //   bloated by what we push. That's the win condition.
            //   Counter: if no non-trump forcing exists, fall back below.
            if (deckEmpty && opp) {
                const uncoveredOnTable = game.table_battles
                    .filter(b => b.defense === null).map(b => b.attack);
                // Forcing attacks must use only NON-TRUMP, MID-TO-LOW value
                // cards. Reasons:
                //   - Trumps in our forcing attack go to opponent's hand on
                //     pickup, arming them with cards we can't beat.
                //   - HIGH cards (Q/K/A) on pickup boomerang back at us in
                //     the next round — opponent attacks with them and we
                //     can't cover. Tested numerically: capping forcing-
                //     attack cards at ≤ J (11) avoids the boomerang while
                //     still triggering on attacks the opponent really
                //     can't cover.
                const FORCING_VALUE_CAP = 11;
                const forcing = attackMoves.filter(m => {
                    const cards = m.cards ?? [];
                    if (cards.some(c => c.suit === trump)) return false;
                    if (cards.some(c => c.value > FORCING_VALUE_CAP)) return false;
                    const all = [...uncoveredOnTable, ...cards];
                    return !canFullyCover(all, opp.hand, trump);
                });
                if (forcing.length > 0) {
                    return cheapest(forcing, m => offensiveMoveCost(m, trump));
                }
            }

            // Principle 13 (attacker): among legal attacks, prefer the one
            // with the MOST cards (provided they're all non-trump, or we're
            // in an explicitly-allowed trump-attack window).
            //   Why (general Durak): every additional card pushed onto
            //   the table is one more cover the defender must resolve.
            //   Pressing hard with multiple same-value cards either drains
            //   trumps, forces a pickup, or speeds discard — all good.
            //   Counter (trump cards in the bundle): trumps spent in an
            //   attack tend to end up in the defender's hand on pickup
            //   AND can't be retrieved if discarded. We exclude trumps
            //   from "press hard" candidates and fall back to "good"
            //   if the only legal attacks include trumps.
            //   Counter (defender's hand small): in legal-move filtering
            //   the defender must have enough hand to receive — handled
            //   by the move generator, not us.
            const nonTrumpAttacks = attackMoves.filter(m => (m.cards ?? []).every(c => c.suit !== trump));
            const pressHardPool = nonTrumpAttacks.length > 0 ? nonTrumpAttacks : attackMoves;
            // Press hard, but only with LOW value cards (≤ J / 11).
            //   Why: pushing high cards onto the table creates a
            //   pickup-and-boomerang risk — they end up in the
            //   defender's hand and come back at us as attacks we
            //   can't cover. Multi-attacks with kings/queens are
            //   especially dangerous in 1v1.
            const HIGH_VALUE_CAP = 11;
            const lowMultiPool = pressHardPool.filter(m =>
                (m.cards ?? []).every(c => c.value <= HIGH_VALUE_CAP),
            );
            const finalPool = lowMultiPool.length > 0 ? lowMultiPool : pressHardPool;
            const byPress = [...finalPool].sort((a, b) => {
                const ca = (a.cards ?? []).length;
                const cb = (b.cards ?? []).length;
                if (cb !== ca) return cb - ca;
                return offensiveMoveCost(a, trump) - offensiveMoveCost(b, trump);
            });
            return byPress[0];
        }

        const passMoves = legalMoves.filter(m => m.type === 'pass');
        if (passMoves.length > 0) {
            return cheapest(passMoves, m => offensiveMoveCost(m, trump));
        }

        if (goodMove) return goodMove;

        const pickupMove = legalMoves.find(m => m.type === 'pickup');
        if (pickupMove) return pickupMove;

        // Suppress unused-variable warnings until a future iteration uses them.
        void deckLeft; void myHandSize; void oppHandSize;
        return legalMoves[legalMoves.length - 1];
    }
}
