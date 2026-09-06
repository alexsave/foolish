// THE BOARD THE GESTURE RULES READ, cached per committed state.
//
// c/src/legal.h's play_* rules take a PUBLISHED PAIR - the menu the kernel
// enumerated for a seat, and the table it was enumerated on - and read nothing
// else. That is what lets a drag frame call them. But the web never computed a
// menu client-side, so this module produces one.
//
// WHY THE CACHE IS THE DESIGN, not an optimisation. Producing the menu means
// kernelMenuWire -> marshal the game into bots.wasm -> calculate_legal_moves,
// which enumerates every cover pairing and is not something to do 60 times a
// second. determineGameAction runs on every drag frame. So the menu is built
// once per POSITION and reused: the key is the game OBJECT, which ServerContext
// replaces on every broadcast and every optimistic patch, so object identity is
// exactly "this position" and a drag frame is a WeakMap hit. A WeakMap means a
// superseded position's menu is collected with it.
//
// REDACTION. The client knows its own hand and only the COUNTS of everyone
// else's. That is enough for its own menu and no more: an attack needs my hand,
// the table's values and the defender's card count; a cover needs my hand and
// the table; a pass needs my hand and the next seat's count. None of them reads
// an opponent's card identity, so the placeholder cards below cannot change the
// answer - the same argument clientGuards.ts already relies on.
import { Card, PersonalGame, PublicPlayer, Game, PLAYER_STATUS } from '@api/core/types.ts';
import { kernelMenuWire, PlayBoard } from '@sdk/ts/wasm/bots.ts';

// Any real card; content is irrelevant for a redacted seat or deck.
const PLACEHOLDER: Card = { suit: 0, value: 2 };

const boards = new WeakMap<object, PlayBoard | null>();

function seatOfSelf(g: PersonalGame): number {
    return g.players.findIndex((p) => p.player_id === g.self?.player_id);
}

/** The redacted PersonalGame as the Game shape the kernel marshal wants. */
function asKernelGame(g: PersonalGame, selfSeat: number): Game {
    return {
        ...g,
        deck: Array.from({ length: g.deck_length ?? 0 }, () => PLACEHOLDER),
        players: g.players.map((p: PublicPlayer, s: number) => ({
            ...p,
            hand: s === selfSeat && g.self?.hand
                ? g.self.hand
                : Array.from({ length: p.hand_length ?? 0 }, () => PLACEHOLDER),
        })),
    } as unknown as Game;
}

/**
 * The PlayBoard for this client's own seat, or null when there is no seat to
 * act for (spectating, or already out). Cached per game object.
 */
export function playBoardFor(game: PersonalGame | null | undefined): PlayBoard | null {
    if (!game || !game.self) return null;
    const hit = boards.get(game);
    if (hit !== undefined) return hit;

    let board: PlayBoard | null = null;
    const seat = seatOfSelf(game);
    if (seat >= 0 && game.self.status !== PLAYER_STATUS.OUT) {
        // Synchronous from marshal to copy-out, with no await inside: nothing
        // else can marshal a different game into the shared resident slot in
        // between. See kernelMenuWire.
        board = {
            menu: kernelMenuWire(asKernelGame(game, seat), seat),
            battles: game.table_battles,
            powerSuit: game.power_suit,
            isDefender: game.defender === seat,
        };
    }
    boards.set(game, board);
    return board;
}
