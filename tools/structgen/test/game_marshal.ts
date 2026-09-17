// The GENERATED path: the same marshal/parse as legacy_marshal.ts, written
// straight into / read straight out of the kernel's Game through the accessors
// structgen emitted. No byte offset or width is spelled here; what remains is
// the semantic mapping (status strings, player ids -> seats, card canonicalisation).
import {
    type Mem, memOf,
    GAME_STATUS_WAITING, GAME_STATUS_PLAYING, GAME_STATUS_GAME_OVER,
    PLAYER_STATUS_IDLE, PLAYER_STATUS_READY, PLAYER_STATUS_IN, PLAYER_STATUS_OUT,
    Game_set_status, Game_set_num_players, Game_set_power_suit, Game_set_first_attacker, Game_set_defender,
    Game_set_discard_pile_length, Game_set_has_flipped, Game_set_deterministic_deck, Game_flipped_at, Game_set_good_players_mask,
    Game_set_has_good_timestamp, Game_set_deck_count, Game_deck_at, Game_deck_LEN, Game_set_num_battles,
    Game_table_battles_at, Game_table_battles_LEN, Game_players_at, Game_players_LEN, Game_set_num_eliminated,
    Game_set_elimination_order, Game_elimination_order_LEN,
    Game_get_status, Game_get_num_players, Game_get_power_suit, Game_get_first_attacker, Game_get_defender,
    Game_get_discard_pile_length, Game_get_has_flipped, Game_get_good_players_mask, Game_get_has_good_timestamp,
    Game_get_deck_count, Game_get_num_battles, Game_get_num_eliminated, Game_get_elimination_order,
    Battle_attack_at, Battle_defense_at,
    Player_set_status, Player_set_awaiting_attack, Player_set_hand_count, Player_set_strategy_key, Player_hand_at, Player_hand_LEN,
    Player_get_status, Player_get_awaiting_attack, Player_get_hand_count,
    Card_raw_get, Card_raw_set, Card_pack, Card_unpack_suit, Card_unpack_value,
} from '../build/harness/game_layout.ts';
import { LAYOUT_HASH } from '../build/harness/layout_hash.ts';
import { type Card, type AnyGame, type Kernel, type KernelState, CARD_POOL, HIDDEN_CARD, wireStateCard } from './legacy_marshal.ts';

const G_TO: Record<string, number> = { waiting: GAME_STATUS_WAITING, playing: GAME_STATUS_PLAYING, game_over: GAME_STATUS_GAME_OVER };
const P_TO: Record<string, number> = { idle: PLAYER_STATUS_IDLE, ready: PLAYER_STATUS_READY, in: PLAYER_STATUS_IN, out: PLAYER_STATUS_OUT };

// Card semantics as tables over the kernel's raw Card byte, built from the
// generated pack/unpack (so no bit position is spelled here either).
// Wire card (0..51, 0xFE hidden, 0xFF none) -> raw Card, with state_get's clamp.
const RAW_OF_WIRE = new Uint8Array(256);
for (let w = 0; w < 256; w++) {
    const id = w > 51 ? 51 : w;
    RAW_OF_WIRE[w] = w === 0xff ? Card_pack(-2, -2) : Card_pack((id / 13) | 0, (id % 13) + 1);
}
// raw Card -> the TS Card the old export+parse produced (CARD_NONE -> card 51, not-a-card -> hidden).
const CARD_OF_RAW: Card[] = [];
const RAW_NONE = Card_pack(-2, -2);
for (let r = 0; r < 256; r++) {
    const s = Card_unpack_suit(r), v = Card_unpack_value(r);
    if (s === -2 && v === -2) CARD_OF_RAW[r] = CARD_POOL[51];
    else if (s < 0 || v < 1) CARD_OF_RAW[r] = HIDDEN_CARD;
    else { const id = s * 13 + v - 1; CARD_OF_RAW[r] = CARD_POOL[id <= 51 ? id : 51]; }
}

let mv: Mem | null = null;
export function gmem(ex: Kernel): Mem {
    if (mv === null || mv.u8.buffer !== ex.memory.buffer) {   // memory.grow detaches the old views
        const h = ex.k_layout_hash() >>> 0;
        if (h !== LAYOUT_HASH) throw new Error(`kernel Game layout ${h.toString(16)} != generated accessors ${LAYOUT_HASH.toString(16)}`);
        mv = memOf(ex.memory.buffer);
    }
    return mv;
}

const seat = (game: AnyGame, pid: string) => {
    const ps = game.players;
    for (let i = 0; i < ps.length; i++) if (ps[i].player_id === pid) return i;
    return -1;
};

export function marshal(ex: Kernel, game: AnyGame): void {
    const m = gmem(ex), g = ex.k_game();
    const players = game.players;
    Game_set_status(m, g, G_TO[game.status] ?? 0);
    Game_set_num_players(m, g, players.length);
    Game_set_power_suit(m, g, game.power_suit);
    Game_set_first_attacker(m, g, game.first_attacker);
    Game_set_defender(m, g, game.defender);
    Game_set_discard_pile_length(m, g, game.discard_pile_length);
    Game_set_has_flipped(m, g, !!game.flipped);
    if (game.flipped) Card_raw_set(m, Game_flipped_at(g), RAW_OF_WIRE[wireStateCard(game.flipped)]);
    let mask = 0;
    for (const pid of game.good_players ?? []) { const s = seat(game, pid); if (s >= 0) mask |= 1 << s; }
    Game_set_good_players_mask(m, g, mask);
    Game_set_has_good_timestamp(m, g, game.good_timestamp !== null && game.good_timestamp !== undefined);
    const deck = game.deck;
    Game_set_deck_count(m, g, deck.length);
    for (let i = 0, n = Math.min(deck.length, Game_deck_LEN); i < n; i++) Card_raw_set(m, Game_deck_at(g, i), RAW_OF_WIRE[wireStateCard(deck[i])]);
    const battles = game.table_battles;
    Game_set_num_battles(m, g, battles.length);
    for (let i = 0, n = Math.min(battles.length, Game_table_battles_LEN); i < n; i++) {
        const b = Game_table_battles_at(g, i), d = battles[i].defense;
        Card_raw_set(m, Battle_attack_at(b), RAW_OF_WIRE[wireStateCard(battles[i].attack)]);
        Card_raw_set(m, Battle_defense_at(b), d ? RAW_OF_WIRE[wireStateCard(d)] : RAW_NONE);
    }
    for (let i = 0, n = Math.min(players.length, Game_players_LEN); i < n; i++) {
        const p = players[i], a = Game_players_at(g, i), hand = p.hand;
        Player_set_status(m, a, P_TO[p.status] ?? 0);
        Player_set_awaiting_attack(m, a, !!p.awaiting_attack);
        Player_set_hand_count(m, a, hand.length);
        Player_set_strategy_key(m, a, p.is_ai ? 0 : -1);
        for (let j = 0, k = Math.min(hand.length, Player_hand_LEN); j < k; j++) Card_raw_set(m, Player_hand_at(a, j), RAW_OF_WIRE[wireStateCard(hand[j])]);
    }
    const elim = game.elimination_order;
    Game_set_num_eliminated(m, g, elim.length);
    for (let i = 0, n = Math.min(elim.length, Game_elimination_order_LEN); i < n; i++) Game_set_elimination_order(m, g, i, seat(game, elim[i]));
    Game_set_deterministic_deck(m, g, !!game.deterministic_deck);
    ex.k_adopt();
}

export function readState(ex: Kernel, g: number): KernelState {
    const m = gmem(ex);
    const numPlayers = Game_get_num_players(m, g) & 0xff;
    const deckN = Game_get_deck_count(m, g) & 0xffff;
    const deck: Card[] = new Array(deckN);
    for (let i = 0; i < deckN; i++) deck[i] = CARD_OF_RAW[Card_raw_get(m, Game_deck_at(g, i))];
    const nBattles = Game_get_num_battles(m, g) & 0xff;
    const battles: KernelState['battles'] = [];
    for (let i = 0; i < nBattles; i++) {
        const b = Game_table_battles_at(g, i), d = Card_raw_get(m, Battle_defense_at(b));
        battles.push({ attack: CARD_OF_RAW[Card_raw_get(m, Battle_attack_at(b))], defense: d === RAW_NONE ? null : CARD_OF_RAW[d] });
    }
    const players: KernelState['players'] = [];
    for (let i = 0; i < numPlayers; i++) {
        const a = Game_players_at(g, i), handN = Player_get_hand_count(m, a) & 0xff;
        const hand: Card[] = new Array(handN);
        for (let j = 0; j < handN; j++) hand[j] = CARD_OF_RAW[Card_raw_get(m, Player_hand_at(a, j))];
        players.push({ status: Player_get_status(m, a) & 0xff, awaiting: Player_get_awaiting_attack(m, a), hand });
    }
    const elimN = Game_get_num_eliminated(m, g) & 0xff;
    const elimination: number[] = [];
    for (let i = 0; i < elimN; i++) elimination.push(Game_get_elimination_order(m, g, i));
    return {
        status: Game_get_status(m, g) & 0xff, numPlayers,
        powerSuit: Game_get_power_suit(m, g), firstAttacker: Game_get_first_attacker(m, g), defender: Game_get_defender(m, g),
        discard: Game_get_discard_pile_length(m, g) & 0xffff,
        flipped: Game_get_has_flipped(m, g) ? CARD_OF_RAW[Card_raw_get(m, Game_flipped_at(g))] : null,
        goodMask: Game_get_good_players_mask(m, g) | 0, hasGoodTs: Game_get_has_good_timestamp(m, g),
        deck, battles, players, elimination,
    };
}
