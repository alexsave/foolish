import { Player, Game } from './common';
// Start with
let startTime = performance.now();
// The engine of fools
// I had a whole paper on this but let's just get started

// Interfaces
export interface Card {
    suit: number;
    value: number;
}

/*export interface Player {
    id: string;
    name: string;
    hand: Card[];
    // TODO IMPORTANT: when we get status, we need to map done_attacking to in to avoid revealing values
    status: 'idle' | 'ready' | 'in' | 'awaiting_attack' |'out';
}*/
interface CardListMapping {
    [key: string]: Card[];
}
interface CardMap {
    [key: string]: Card;
}
// First check if all can be covered
// This is actually a bit tricky becuase there can be mulitple options, some better than others
interface Move {
    type: 'pass' | 'throw' | 'pickup' | 'cover' | 'success';
    player: string;
    card?: Card;
    coverMap?: Map<Card, Card>;
}
interface Battle {
    attack: Card;
    defense: Card | null;
}

// Constants
export const CARDS_PER_PLAYER = 6;
const [SPADES, HEARTS, CLUBS, DIAMONDS] = [0, 1, 2, 3];
const SUITS = [SPADES, HEARTS, CLUBS, DIAMONDS];
const SUIT_MAP = ['Spades', 'Hearts', 'Clubs', 'Diamonds'];
const VALUE_MAP = [
    null, //0
    '2', //1
    '3', //2
    '4', //3
    '5', //4
    '6', //5
    '7', //6
    '8', //7
    '9', //8
    '10', //9
    'J', //10
    'Q', //11
    'K', //12
    'A', //13
];
const NAMES = ['Rando', 'Smarty', 'John', 'Blake', 'William', 'Zach', 'Alex', 'Ben', 'Caleb', 'Dylan', 'Ethan', 'Finn', 'Gavin', 'Hunter', 'Isaiah', 'Jack', 'Kyle', 'Landon', 'Mason', 'Nathan', 'Oliver', 'Parker', 'Quinn', 'Ryan', 'Samuel', 'Thomas', 'Ulysses', 'Vance', 'Wesley', 'Xavier', 'Yusuf', 'Zane']
export const ACE_VALUE = 13;
const CARDS_PER_SUIT = 9;
const START_VALUE = ACE_VALUE - CARDS_PER_SUIT + 1;
const PLAYER_COUNT = 7;

// State
let deck: Card[] = [];
let players: Player[] = [];

// Functions
let currentSeed = 4 + 20;
// Constants for the LCG algorithm (often chosen to be prime numbers)
const a = 1664525;
const c = 1013904223;
const m = 4294967296; // 2^32
const seededRand = () => {
    // Math.random()
    currentSeed = (a * currentSeed + c) % m;
    return currentSeed / m; // Normalize to a value between 0 (inclusive) and 1 (exclusive)
};
export const cardDisplay = (card: Card) => `${VALUE_MAP[card.value]} of ${SUIT_MAP[card.suit]}`;



const cardSorter = (a: Card, b: Card) => {
    if (a.suit === powerSuit && b.suit !== powerSuit) {
        return true;
    } else if (a.suit !== powerSuit && b.suit === powerSuit) {
        return false;
    } else {
        return a.value > b.value;
    }
};

export const canCover = (attack: Card, defense: Card, powerSuit: number) => {
    if (defense.suit !== attack.suit) {
        // only different suit scenario that works
        return defense.suit === powerSuit && attack.suit !== powerSuit;
    }
    return defense.value > attack.value;
};

const chooseAttack = () => {
    // "AI"
    let hand = players[firstAttacker].hand;
    let cardChoice = hand[0];
    // descend
    for (let i = 1; i < hand.length; i++) {
        const curCard = hand[i];
        if (cardChoice.suit === powerSuit && curCard.suit !== powerSuit) {
            cardChoice = curCard;
        } else if (cardChoice.value > curCard.value) {
            cardChoice = curCard;
        }
    }
    // Also for fun, let's do this
    if (firstAttacker === 0) {
        cardChoice = hand[Math.floor(seededRand() * hand.length)];
    }

    hand.splice(hand.indexOf(cardChoice), 1);
    table.push({ attack: cardChoice, defense: null });
    tableValues.add(cardChoice.value);
    console.log(
        `Player ${players[firstAttacker].name} attacks ${players[currentlyAttacked].name} with a ${cardDisplay(cardChoice)}`,
    );
    checkWin(firstAttacker);
};

const checkWin = (playerId: number) => {
    // However, if they are the last one, they are the fool
    if (players.filter(p => p.status === 'in').length === 1) {
        console.log(`Player ${players[playerId].name} is the fool`);
        gameDone = true;
        //players[playerId].status = 'out';
    } else if (players[playerId].hand.length === 0 && deck.length === 0) {
        console.log(`Player ${players[playerId].name} leaves the game`);
        players[playerId].status = 'out';
    }
}

const allowAttacks = () => {
    for (let i = 0; i < PLAYER_COUNT; i++) {
        let p = (firstAttacker + i) % PLAYER_COUNT;
        if (p === currentlyAttacked) {
            continue;
        }
        const pHand = players[p].hand;
        const requestedAttacks: Card[] = [];
        for (let j = 0; j < pHand.length; j++) {
            if (tableValues.has(pHand[j].value)) {
                requestedAttacks.push(pHand[j]);
            }
        }
        for (let card of requestedAttacks) {
            // in reality, this will be calculated on each API call
            const uncovered = table.filter(
                (b) => b.attack !== null && b.defense === null,
            );
            if (uncovered.length >= players[currentlyAttacked].hand.length) {
                console.log('Max amount of cards placed');
                return;
            }
            pHand.splice(pHand.indexOf(card), 1);
            table.push({ attack: card, defense: null });
            console.log(
                `Player ${players[p].name} attacks ${players[currentlyAttacked].name} with a ${cardDisplay(card)}`,
            );
            checkWin(p);
        }
    }
};

const cardScore = (card: Card) => {
    // This can be tweaked a bit becuase the lowest value trump would be a few more than 13
    return card.value + (card.suit === powerSuit ? ACE_VALUE : 0);
};

// now defend
const recursiveCoverCheck = (
    toCover: Card[],
    toCoverIndex: number,
    defenseMap: Map<Card, Card[]>,
    handCopy: Card[],
): Map<Map<Card, Card>, number> => {
    const indent = '  '.repeat(toCoverIndex);
    //console.log(
        //indent + 
        //`Looking for defenses for ${cardDisplay(toCover[toCoverIndex])}`,
    //);
    const key = toCover[toCoverIndex];
    const defCards = defenseMap.get(key)!;
    const allSolutions = new Map<Map<Card, Card>, number>();
    
    for (let i = 0; i < defCards.length; i++) {
        const defCard = defCards[i];
        const defCardScore = cardScore(defCard);
        ////console.log(
            //indent + '  ' +
            //`Trying ${cardDisplay(defCard)} (score: ${defCardScore})`,
        //);
        if (!handCopy.includes(defCard)) {
            //console.log(indent + '    Already played');
            // already played previous
            continue;
        }
        // not chosen yet, we can play this one
        if (toCoverIndex === toCover.length - 1) {
            // done base case
            const map = new Map<Card, Card>();
            map.set(key, defCard);
            const score = cardScore(defCard);
            //console.log(indent + '    ' + `Found solution with score: ${score}`);
            allSolutions.set(map, score);
        } else {
            // not done, recurse
            // first check continuing possibilities
            const subSolutions = recursiveCoverCheck(
                toCover,
                toCoverIndex + 1, // increment index
                defenseMap,
                handCopy.filter((c) => c !== defCard), // remove the card
            );
            // Add all sub-solutions to our solutions
            for (const [subMap, subScore] of subSolutions) {
                const map = new Map<Card, Card>(subMap);
                map.set(key, defCard);
                const totalScore = subScore * cardScore(defCard);
                //console.log(indent + '    ' + `Found solution with total score: ${totalScore}`);
                allSolutions.set(map, totalScore);
            }
        }
    }
    
    if (allSolutions.size === 0) {
        //console.log(indent + 'Can\'t cover with remaining cards');
    } else {
        //console.log(indent + `Found ${allSolutions.size} possible solution(s)`);
    }
    
    return allSolutions;
};

const aiDefend = (): Move => {
    const defense = players[currentlyAttacked].hand;
    if (table.length === 0) throw new Error('idk');
    // check if we even need to defend
    const done = table.every((battle) => battle.defense !== null);
    if (done) {
        return {
            player: players[currentlyAttacked].name,
            type: 'success',
        };
    }
    // First try to pass the buck
    // only possible if no cover + all same value
    // might be easier to keep track of if any defense has been played
    const passValue = table[0].attack.value;
    const passable = table.every(
        (battle) => battle.defense === null && battle.attack.value === passValue,
    );

    // also we need the next player to have enough cards to cover
    // find next target
    let nextTarget = (currentlyAttacked + 1) % PLAYER_COUNT;
    while (players[nextTarget].status === 'out') {
        nextTarget = (nextTarget + 1) % PLAYER_COUNT;
    }
    const nextHasCapacity = players[nextTarget].hand.length >= table.length + 1;

    if (passable && nextHasCapacity) {
        for (let i = 0; i < defense.length; i++) {
            if (defense[i].value === passValue) {
                console.log('pass chosen with ' + cardDisplay(defense[i]));
                return {
                    player: players[currentlyAttacked].name,
                    type: 'pass',
                    card: defense[i],
                };
            }
        }
    }
    //unpassable or no pass value cards.
    // Try defending
    //uhhh
    // get all cards in the hand that CAN defend, and try all combinations of thoes
    // ohhh better yet make a mapping of attack card -> possible defensees

    const possibleDefenses = new Map<Card, Card[]>();
    for (let i = 0; i < table.length; i++) {
        if (table[i].defense != null) {
            continue;
        }
        const attack = table[i].attack;
        // dumb but oh well
        possibleDefenses.set(attack, []);
        for (let j = 0; j < defense.length; j++) {
            const defCard = defense[j];
            if (canCover(attack, defCard, powerSuit)) {
                possibleDefenses.get(attack)!.push(defCard);
            }
        }
        // A card can't be covered, just pick up
        if (possibleDefenses.get(attack)!.length === 0) {
            console.log(
                'card can"t be covered, choosing pickup bc of ' +
                cardDisplay(attack),
            );
            return {
                player: players[currentlyAttacked].name,
                type: 'pickup',
            };
        }
    }
    console.log('after thinking, here are possible defenses');
    console.log(possibleDefenses);
    // At this point we know that every card can be covered. Not necessarily all at once though
    // Basic strategy: always cover at any cost.
    // We now have something like {'A of Spades': [card, card], 'K of spades': [card]}
    // Now try all combinations. Not optimal, but this will be done by users ideally so whatev
    // Slowest case scenario: 6 power cards in defense, all 6 cover all the attacks
    // 6*
    // we need recursive method. find all solutions and pick the best one
    const cardsToCover = Array.from(possibleDefenses.keys());
    const handCopy = [...defense];
    const allSolutions: Map<Map<Card, Card>, number> = recursiveCoverCheck(
        cardsToCover,
        0,
        possibleDefenses,
        handCopy,
    );
    if (allSolutions.size === 0) {
        console.log('no cover combintation ');
        return {
            player: players[currentlyAttacked].name,
            type: 'pickup',
        };
    }
    // find the solution with the lowest score
    let bestCoverMap: Map<Card, Card> | null = null;
    let bestScore = Infinity;
    for (const [coverMap, score] of allSolutions) {
        if (score < bestScore) {
            bestScore = score;
            bestCoverMap = coverMap;
        }
    }
    if (currentlyAttacked === 0) {
        //worst move for testing
        bestCoverMap = new Map();
        bestScore = 0;
        for (const [coverMap, score] of allSolutions) {
            if (score > bestScore) {
                bestScore = score;
                bestCoverMap = coverMap;
            }
        }
    }
    // there is a way forward
    console.log('chosen defense with score', bestScore);
    console.log(bestCoverMap);
    return {
        player: players[currentlyAttacked].name,
        type: 'cover',
        coverMap: bestCoverMap!,
    };
};


// at the start of a "battle", only the first attacker can attack
// then anything can happen
// then once a certain amount of cards are on the table, the defender can only defend, no one else can do anything
// first attackers is when every card on the table is covered, but we need confirmation from attackers to continue


export const draw = (game: Game): Card | null => {
    if (game.deck.length === 0) {
        if (game.flipped === null) {
            return null;
        }
        const copy: Card = game.flipped;
        game.flipped = null;
        return copy;
    }
    // Make this more secure
    const index = Math.floor(seededRand() * game.deck.length);
    const card = game.deck.splice(index, 1)[0];
    return card;
};

const refill = () => {
    // most importantly, check if currently Attacked cleared their hand
    let defenseHand = players[previousCurrentlyAttacked].hand;
    if (defenseHand.length === 0) {
        // they draw first
        while (defenseHand.length < CARDS_PER_PLAYER) {
            const c = draw(getGameState());
            if (c === null) {
                console.log('Deck ran out');
                return;
            }
            console.log(
                `Player ${players[previousCurrentlyAttacked].name} draws ${cardDisplay(c)}`,
            );
            defenseHand.push(c);
        }
    }
    // Then go around starting from firstAttacker
    let pIndex = previousFirstAttacker;
    do {
        const hand = players[pIndex].hand;
        while (hand.length < CARDS_PER_PLAYER) {
            const c = draw(getGameState());
            if (c === null) {
                console.log('Deck ran out');
                return;
            }
            console.log(`Player ${players[pIndex].name} draws ${cardDisplay(c)}`);
            hand.push(c);
        }
        pIndex = (pIndex + 1) % PLAYER_COUNT;
    } while (pIndex !== previousFirstAttacker);
};

// true= continue same battle (cover), false = new battle
const handleChoice = (choice: Move): boolean => {
    if (choice.type === 'success') {
        console.log(
            `Player ${players[currentlyAttacked].name} successfully covered`,
        );

        previousFirstAttacker = firstAttacker;
        previousCurrentlyAttacked = currentlyAttacked;
        // Send table to garbage (delete for good)
        firstAttacker = currentlyAttacked % PLAYER_COUNT;
        console.log(`firstAttacker changed to: ${players[firstAttacker].name}`);
        // quick check
        while (players[firstAttacker].status === 'out') {
            firstAttacker = (firstAttacker + 1) % PLAYER_COUNT;
        console.log(`firstAttacker changed to: ${players[firstAttacker].name}`);
        }
        currentlyAttacked = (firstAttacker + 1) % PLAYER_COUNT;
        while (players[currentlyAttacked].status === 'out') {
            currentlyAttacked = (currentlyAttacked + 1) % PLAYER_COUNT;
        }
        return false;
    } else if (choice.type === 'pass') {
        // More interesting somewhat
        let defenseHand = players[currentlyAttacked].hand;
        const attackCard = choice.card!;
        defenseHand.splice(defenseHand.indexOf(attackCard), 1);
        table.push({ attack: attackCard, defense: null });

        previousCurrentlyAttacked = currentlyAttacked;
        let nextAttacked = (currentlyAttacked + 1) % PLAYER_COUNT;
        while (players[nextAttacked].status === 'out') {
            nextAttacked = (nextAttacked + 1) % PLAYER_COUNT;
        }
        console.log(
            `Player ${players[currentlyAttacked].name} passes to ${players[nextAttacked].name} with ${cardDisplay(attackCard)}`,
        );
        // move attacker

        currentlyAttacked = nextAttacked;
        checkWin(previousCurrentlyAttacked);
        // but continue attack
        return true;
    } else if (choice.type === 'cover' && choice.coverMap) {
        let defenseHand = players[currentlyAttacked].hand;
        for (const [att, def] of choice.coverMap) {
            console.log(
                `Player ${players[currentlyAttacked].name} puts a ${cardDisplay(def)} on ${cardDisplay(att)}`,
            );
            defenseHand.splice(defenseHand.indexOf(def), 1);
            const tIndex = table.findIndex(
                (battle) => battle.attack === att,
            );
            if (tIndex !== -1) {
                table[tIndex].defense = def;
            }
            //table.push({attack: cardChoice, defense: null});
            tableValues.add(def.value);
        }
        checkWin(currentlyAttacked);
        return true;
    } else if (choice.type === 'pickup' || true /**default move**/) {
        // Full table moves to defenseHand
        let defenseHand = players[currentlyAttacked].hand;
        for (let i = 0; i < table.length; i++) {
            const battle = table[i];
            defenseHand.push(battle.attack);
            console.log(
                `Player ${players[currentlyAttacked].name} picks up ${cardDisplay(battle.attack)}`,
            );
            if (battle.defense) {
                console.log(
                    `Player ${players[currentlyAttacked].name} picks up ${cardDisplay(battle.defense)}`,
                );
                defenseHand.push(battle.defense);
            }
        }
        previousFirstAttacker = firstAttacker;
        previousCurrentlyAttacked = currentlyAttacked;
        // increment
        firstAttacker = (currentlyAttacked + 1) % PLAYER_COUNT;
        console.log(`firstAttacker changed to: ${players[firstAttacker].name}`);
        // quick check
        while (players[firstAttacker].status === 'out') {
            firstAttacker = (firstAttacker + 1) % PLAYER_COUNT;
        console.log(`firstAttacker changed to: ${players[firstAttacker].name}`);
        }
        currentlyAttacked = (firstAttacker + 1) % PLAYER_COUNT;
        while (players[currentlyAttacked].status === 'out') {
            currentlyAttacked = (currentlyAttacked + 1) % PLAYER_COUNT;
        }
        // move along
        return false;
    }
};

//shared mutable state lol
let powerSuit: number = -1;
let previousFirstAttacker: number = -1;
let previousCurrentlyAttacked: number = -1;
let flipped: Card | null = null;
let currentlyAttacked: number = -1;
let firstAttacker: number = -1;
let table: Battle[] = [];
let tableValues: Set<number> = new Set();
let gameDone: boolean = false;
let attackCount: number = 0;

// Server-used methods
export const refill_deck = (): Card[] => {
    const deck: Card[] = [];
    for (let i = 0; i < SUITS.length; i++) {
        for (let j = START_VALUE; j <= ACE_VALUE; j++) {
            deck.push({ suit: SUITS[i], value: j });
        }
    }
    return deck;
}

export const initialize_hands = (game: Game): Card[][] => {
    
    const result: Card[][] = [];
    for (let j = 0; j < game.players.length; j++) {
        result.push([]);
    }
    for (let i = 0; i < CARDS_PER_PLAYER; i++) {
        result.push([]);
        for (let j = 0; j < game.players.length; j++) {
            //const name = result[j].name;
            const c = draw(game)!;
            //console.log(`Player ${name} draws ${cardDisplay(c)}`);
            result[j].push(c);
        }
    }
    return result;
}

const getGameState = (): Game => {
    return {
        deck: deck,
        flipped: flipped,
        players: players,
        status: 'playing',
        powerSuit: powerSuit,
        firstAttacker: firstAttacker,
        currentlyAttacked: currentlyAttacked,
        previousFirstAttacker: previousFirstAttacker,
        previousCurrentlyAttacked: previousCurrentlyAttacked,
        table: table
    }
}

export const determine_lowest_power_index = (game: Game): number => {
    // Whoever has lowest power
    // With 2 players it's possible no one has it. Also with 4.
    // With 5 it's guaranteed
    // This is actually kind interesting
    // In the 36 card case (6+), you have 9 power cards
    // only 8 max can be distributed to players at dealing because of the flipped card
    // there are still non power 27 cards that can be distributed
    // at most, with 4 players, 4*6=24 cards are all non-power
    // but odds are 27/35 * 26/34 * 25/33 ...
    // but with 5, there must be 3 power cards in the hand
    // with 52, there are 13 power cards and 39 nonpower
    // max of 6 it's possible, 7 it's impossible cuz 42 cards are out
    // Because
    let lowestPowerValue = ACE_VALUE + 1;
    let lowestPowerPlayer = -1;
    for (let i = 0; i < game.players.length; i++) {
        let hand = game.players[i].hand;
        for (let j = 0; j < hand.length; j++) {
            let card = hand[j];
            if (card.suit === game.powerSuit) {
                if (card.value < lowestPowerValue) {
                    lowestPowerValue = card.value;
                    lowestPowerPlayer = i;
                }
            }
        }
    }
    if (lowestPowerPlayer === -1) {
        lowestPowerPlayer = Math.floor(Math.random() * game.players.length);
    }
    return lowestPowerPlayer;
}

export const set_positions = (game: Game) => {
    game.firstAttacker = game.firstAttacker;
    game.currentlyAttacked = (game.firstAttacker + 1) % game.players.length;
    game.previousFirstAttacker = game.firstAttacker;
    game.previousCurrentlyAttacked = game.currentlyAttacked;
}

const game = () => {
    deck = refill_deck();

    // Deal. Probalby more effecient to generate all cards than randomly take from it, but this is more secure
    for (let i = 0; i < PLAYER_COUNT; i++) {
        players.push({
            id: 'idk',
            name: NAMES[i],
            hand: [],
            status: 'in',
        });
    }

    const hands = initialize_hands(
        getGameState()
    );
    for (let i = 0; i < PLAYER_COUNT; i++) {
        players[i].hand = hands[i];
    }

    // The flipped card
    flipped = draw(getGameState());
    powerSuit = flipped!.suit;

    console.log(`The flipped card is ${cardDisplay(flipped!)}`);
    console.log(`The power suit is set to ${SUIT_MAP[flipped!.suit]}`);

    // Checkpoint
    // Start with lowestPowerPlayer, go down the index
    let lowestPowerPlayer = determine_lowest_power_index(getGameState());
    firstAttacker = lowestPowerPlayer;
    set_positions(getGameState());
    //currentlyAttacked = (lowestPowerPlayer + 1) % PLAYER_COUNT;
    //previousFirstAttacker = firstAttacker;
    //previousCurrentlyAttacked = currentlyAttacked;
    // true if a > b
    table = [];
    tableValues = new Set();
    let choice;
    let continueBattle;

    let playersInGame = PLAYER_COUNT;

    while (playersInGame > 1 && attackCount < 1000) {

        console.log('');
        console.log('//////////////////');
        chooseAttack();
        if (gameDone) {
            break;
        }
        allowAttacks();
        if (gameDone) {
            break;
        }
        // after this it's possible everything is covered lol
        choice = aiDefend();
        continueBattle = handleChoice(choice);
        while (continueBattle === true) {
            // Allow attacking again
            allowAttacks();
            choice = aiDefend();
            continueBattle = handleChoice(choice);
        }
        refill();
        table = [];
        tableValues = new Set();



        playersInGame = 0;
        for (let i = 0; i < players.length; i++) {
            if (players[i].status === 'in')
                playersInGame++;
        }


        console.log('//////////////////');
        attackCount++;
    }


}

const fools = new Map<string, number>();
for (let i = 0; i < 0; i++) {
    game();
    for (let i = 0; i < players.length; i++) {
        if (players[i].status === 'in') {
            console.log(`Player ${players[i].name} is the fool`);
            fools.set(players[i].name, (fools.get(players[i].name) || 0) + 1);
        }
    }
    gameDone = false;
    players = [];
    deck = [];
    flipped = null;
    powerSuit = -1;
    firstAttacker = -1;
    currentlyAttacked = -1;
    previousFirstAttacker = -1;
    previousCurrentlyAttacked = -1;
    table = [];
    tableValues = new Set();
    attackCount = 0;
    if (i % 100 === 0) {
        console.log(`${i} games`);
    }
}

for (let [key, value] of fools) {
    console.log(`Player ${key} is the fool ${value} times`);
}




// Place card against currentlyAttacked
// Also this will be async ofc
// carnage time
// now we move on
// Code to time
let endTime = performance.now();

let duration = endTime - startTime;
console.log(`Execution time: ${duration} milliseconds`);
