// Start with
let startTime = performance.now();
// The engine of fools
// I had a whole paper on this but let's just get started
const DISPLAY_MAP = [
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
interface Card {
    suit: number;
    value: number;
}
interface Player {
    name: string;
    hand: Card[];
    status: 'in' | 'out';
}
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
    coverMap?: CardMap;
}
interface Battle {
    attack: Card;
    defense: Card | null;
}
const CARDS_PER_PLAYER = 6;
const [SPADES, HEARTS, CLUBS, DIAMONDS] = [0, 1, 2, 3];
const SUITS = [SPADES, HEARTS, CLUBS, DIAMONDS];
const CARDS_PER_SUIT = 9;
const START_VALUE = 5;
const ACE_VALUE = 13;
const PLAYER_COUNT = 5;
let deck: Card[] = [];
let players: Player[] = [];
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
const cardSorter = (a: Card, b: Card) => {
    if (a.suit === POWER_SUIT && b.suit !== POWER_SUIT) {
        return true;
    } else if (a.suit !== POWER_SUIT && b.suit === POWER_SUIT) {
        return false;
    } else {
        return a.value > b.value;
    }
};
const canCover = (attack: Card, defense: Card) => {
    if (defense.suit !== attack.suit) {
        // only different suit scenario that works
        return defense.suit === POWER_SUIT && attack.suit !== POWER_SUIT;
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
        if (cardChoice.suit === POWER_SUIT && curCard.suit !== POWER_SUIT) {
            cardChoice = curCard;
        } else if (cardChoice.value > curCard.value) {
            cardChoice = curCard;
        }
    }
    hand.splice(hand.indexOf(cardChoice), 1);
    table.push({ attack: cardChoice, defense: null });
    tableValues.add(cardChoice.value);
    console.log(
        `Player ${players[firstAttacker].name} attacks ${players[currentlyAttacked].name} with a ${JSON.stringify(cardChoice)}`,
    );
    if (hand.length === 0 && deck.length === 0) {
        console.log(`Player ${players[firstAttacker].name} leaves the game`);
        players[firstAttacker].status = 'out';
    }
};
const allowAttacks = () => {
    for (let i = 0; i < PLAYER_COUNT; i++) {
        let p = (firstAttacker + i) % PLAYER_COUNT;
        if (p === currentlyAttacked) {
            continue;
        }
        const pHand = players[p].hand;
        const requestedAttacks = [];
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
                `Player ${players[p].name} attacks ${players[currentlyAttacked].name} with a ${JSON.stringify(card)}`,
            );
            // Players can win here
            if (pHand.length === 0 && deck.length === 0) {
                console.log(`Player ${players[p].name} leaves the game`);
                players[p].status = 'out';
            }
        }
    }
};
// now defend
const recursiveCoverCheck = (
    toCover: string[],
    toCoverIndex: number,
    defenseMap: CardListMapping,
    handCopy: Card[],
): CardMap | false => {
    console.log(
        '\t'.repeat(toCoverIndex) +
        'looking for defenses for ' +
        toCover[toCoverIndex],
    );
    const key = toCover[toCoverIndex];
    for (let i = 0; i < defenseMap[key].length; i++) {
        const defCard = defenseMap[key][i];
        console.log(
            '\t'.repeat(toCoverIndex + 1) +
            'seeing what happens if we choose' +
            JSON.stringify(defCard),
        );
        if (!handCopy.includes(defCard)) {
            console.log('\t'.repeat(toCoverIndex) + 'already played');
            // already played previous
            continue;
        }
        // not chosen yet, we can play this one
        if (toCoverIndex === toCover.length - 1) {
            // done base case
            return { [key]: defCard };
        } else {
            // not done, recurse
            // first check continuing possibilities
            const result = recursiveCoverCheck(
                toCover,
                toCoverIndex + 1, // increment index
                defenseMap,
                handCopy.filter((c) => c !== defCard), // remove the card
            );
            if (result === false) {
                continue;
            }
            // not false, we have a path
            return { ...result, [key]: defCard };
        }
    }
    // went through entire list of possibilities and all were played
    console.log('\t'.repeat(toCoverIndex) + 'can"t cover with remaining cards');
    return false;
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
    if (passable) {
        for (let i = 0; i < defense.length; i++) {
            if (defense[i].value === passValue) {
                console.log('pass chosen with ' + JSON.stringify(defense[i]));
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
    const possibleDefenses: CardListMapping = {};
    for (let i = 0; i < table.length; i++) {
        if (table[i].defense != null) {
            continue;
        }
        const attack = table[i].attack;
        // dumb but oh well
        possibleDefenses[JSON.stringify(attack)] = [];
        for (let j = 0; j < defense.length; j++) {
            const defCard = defense[j];
            if (canCover(attack, defCard)) {
                possibleDefenses[JSON.stringify(attack)].push(defCard);
            }
        }
        // A card can't be covered, just pick up
        if (possibleDefenses[JSON.stringify(attack)].length === 0) {
            console.log(
                'card can"t be covered, choosing pickup bc of ' +
                JSON.stringify(attack),
            );
            return {
                player: players[currentlyAttacked].name,
                type: 'pickup',
            };
        }
    }
    console.log('after thinking, here are possible defenses');
    console.log(JSON.stringify(possibleDefenses));
    // At this point we know that every card can be covered. Not necessarily all at once though
    // Basic strategy: always cover at any cost.
    // We now have something like {'A of Spades': [card, card], 'K of spades': [card]}
    // Now try all combinations. Not optimal, but this will be done by users ideally so whatev
    // Slowest case scenario: 6 power cards in defense, all 6 cover all the attacks
    // 6*
    // we need recursive method. let's go greedy for now
    const cardsToCover = Object.keys(possibleDefenses);
    const handCopy = [...defense];
    const chosenDefenses: CardMap | false = recursiveCoverCheck(
        cardsToCover,
        0,
        possibleDefenses,
        handCopy,
    );
    if (chosenDefenses === false) {
        console.log('no cover combintation ');
        return {
            player: players[currentlyAttacked].name,
            type: 'pickup',
        };
    }
    // there is a way forward
    console.log('chosen defense');
    console.log(JSON.stringify(chosenDefenses));
    return {
        player: players[currentlyAttacked].name,
        type: 'cover',
        coverMap: chosenDefenses,
    };
};
const draw = (): Card | null => {
    if (deck.length === 0) {
        if (flipped === null) {
            return null;
        }
        const copy: Card = flipped;
        flipped = null;
        return copy;
    }
    // Make this more secure
    const index = Math.floor(seededRand() * deck.length);
    const card = deck.splice(index, 1)[0];
    return card;
};
const refill = () => {
    // most importantly, check if currently Attacked cleared their hand
    let defenseHand = players[currentlyAttacked].hand;
    if (defenseHand.length === 0) {
        // they draw first
        while (defenseHand.length < CARDS_PER_PLAYER) {
            const c = draw();
            if (c === null) {
                console.log('Deck ran out');
                return;
            }
            console.log(
                `Player ${players[currentlyAttacked].name} draws ${JSON.stringify(c)}`,
            );
            defenseHand.push(c);
        }
    }
    // Then go around starting from firstAttacker
    let pIndex = firstAttacker;
    do {
        const hand = players[pIndex].hand;
        while (hand.length < CARDS_PER_PLAYER) {
            const c = draw();
            if (c === null) {
                console.log('Deck ran out');
                return;
            }
            console.log(`Player ${players[pIndex].name} draws ${JSON.stringify(c)}`);
            hand.push(c);
        }
        pIndex = (pIndex + 1) % PLAYER_COUNT;
    } while (pIndex !== firstAttacker);
};
// true= continue same battle (cover), false = new battle
const handleChoice = (choice: Move): boolean => {
    if (choice.type === 'success') {
        console.log(
            `Player ${players[currentlyAttacked].name} successfully covered`,
        );
        // Send table to garbage (delete for good)
        firstAttacker = currentlyAttacked % PLAYER_COUNT;
        // quick check
        while (players[firstAttacker].status === 'out') {
            firstAttacker = (firstAttacker + 1) % PLAYER_COUNT;
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
        let nextAttacked = (currentlyAttacked + 1) % PLAYER_COUNT;
        while (players[nextAttacked].status === 'out') {
            nextAttacked = (nextAttacked + 1) % PLAYER_COUNT;
        }
        console.log(
            `Player ${players[currentlyAttacked].name} passes to ${players[nextAttacked].name} with ${JSON.stringify(attackCard)}`,
        );
        // move attacker
        currentlyAttacked = nextAttacked;
        // but continue attack
        return true;
    } else if (choice.type === 'cover') {
        let defenseHand = players[currentlyAttacked].hand;
        for (const att in choice.coverMap) {
            if (!choice.coverMap.hasOwnProperty(att)) continue;
            const def = choice.coverMap[att];
            console.log(
                `Player ${players[currentlyAttacked].name} puts ${JSON.stringify(def)} on ${att}`,
            );
            defenseHand.splice(defenseHand.indexOf(def), 1);
            const tIndex = table.findIndex(
                (battle) => JSON.stringify(battle.attack) === att,
            );
            if (tIndex !== -1) {
                table[tIndex].defense = def;
            }
            //table.push({attack: cardChoice, defense: null});
            tableValues.add(def.value);
            //console.log('defense' + JSON.stringify(defenseHand));
        }
        // this could win for defender
        if (defenseHand.length === 0 && deck.length === 0) {
            console.log(`Player ${players[currentlyAttacked].name} leaves the game`);
            players[currentlyAttacked].status = 'out';
        }
        return true;
    } else if (choice.type === 'pickup' || true /**default move**/) {
        // Full table moves to defenseHand
        let defenseHand = players[currentlyAttacked].hand;
        for (let i = 0; i < table.length; i++) {
            const battle = table[i];
            defenseHand.push(battle.attack);
            console.log(
                `Player ${players[currentlyAttacked].name} picks up ${JSON.stringify(battle.attack)}`,
            );
            if (battle.defense) {
                console.log(
                    `Player ${players[currentlyAttacked].name} picks up ${JSON.stringify(battle.defense)}`,
                );
                defenseHand.push(battle.defense);
            }
        }
        // increment
        firstAttacker = (currentlyAttacked + 1) % PLAYER_COUNT;
        // quick check
        while (players[firstAttacker].status === 'out') {
            firstAttacker = (firstAttacker + 1) % PLAYER_COUNT;
        }
        currentlyAttacked = (firstAttacker + 1) % PLAYER_COUNT;
        while (players[currentlyAttacked].status === 'out') {
            currentlyAttacked = (currentlyAttacked + 1) % PLAYER_COUNT;
        }
        // move along
        return false;
    }
};
for (let j = 0; j < SUITS.length; j++) {
    for (let i = START_VALUE; i <= ACE_VALUE; i++) {
        deck.push({ suit: SUITS[j], value: i });
    }
}
// Deal. Probalby more effecient to generate all cards than randomly take from it
for (let i = 0; i < PLAYER_COUNT; i++) {
    players.push({
        name: Math.floor(seededRand() * 0xffff).toString(16),
        hand: [],
        status: 'in',
    });
}
for (let i = 0; i < CARDS_PER_PLAYER; i++) {
    for (let j = 0; j < PLAYER_COUNT; j++) {
        const c = draw();
        if (c !== null) players[j].hand.push(c);
    }
}
console.log(JSON.stringify(players));
// The flipped card
let flipped = draw();
const POWER_SUIT = flipped?.suit;
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
for (let i = 0; i < PLAYER_COUNT; i++) {
    let hand = players[i].hand;
    for (let j = 0; j < hand.length; j++) {
        let card = hand[j];
        if (card.suit === POWER_SUIT) {
            if (card.value < lowestPowerValue) {
                lowestPowerValue = card.value;
                lowestPowerPlayer = i;
            }
        }
    }
}
if (lowestPowerPlayer === -1) {
    lowestPowerPlayer = Math.floor(Math.random() * PLAYER_COUNT);
}
console.log('power suit is ' + POWER_SUIT);
console.log(JSON.stringify(players[lowestPowerPlayer]));
// Checkpoint
const state = { players, deck, flipped };
console.log(JSON.stringify(state).length);
// Start with lowestPowerPlayer, go down the index
let firstAttacker = lowestPowerPlayer;
let currentlyAttacked = (lowestPowerPlayer + 1) % PLAYER_COUNT;
// true if a > b
let table: Battle[] = [];
let tableValues = new Set();
let choice;
let continueBattle;

let playersInGame = PLAYER_COUNT;

while (playersInGame > 1) {
    console.log('');
    console.log('//////////////////');
    chooseAttack();
    allowAttacks();
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
}


    for (let i = 0; i < players.length; i++) {
        if (players[i].status === 'in'){

            console.log(`Player ${players[i].name} ends up the fool`);
            break;
        }
    }


// Place card against currentlyAttacked
// Also this will be async ofc
// carnage time
// now we move on
// Code to time
let endTime = performance.now();

let duration = endTime - startTime;
console.log(`Execution time: ${duration} milliseconds`);
