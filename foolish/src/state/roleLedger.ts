// roleLedger.ts - WHAT THE BADGES ARE WEARING, which is not the live board.
//
// THE MARKS LAG THE GAME STATE. A sequence freezes them and walks them forward
// beat by beat, so a bout end does not re-cast every sword on the table a beat
// before the sequence that earns it has played. c/src/anim_plan.h says this of
// its own role rules, and says it of the web in as many words:
//
//   "`shown` is what the badges are WEARING, which is not the live board: a
//    sequence freezes the marks and walks them forward, so a rule that read the
//    resident game would answer about a position nobody is looking at."
//
// The website had no such ledger, so every mark it drew came off whatever board
// the last animation step committed - "whenever the step that carried the board
// landed", which is the defect docs/ANIM_TIMING_AUDIT.md section 12 names. This
// file is the value the kernel's three role rules are asked ABOUT
// (anim_goods_opening / anim_goods_cleared / anim_pass_hand_off, reached through
// sdk/ts/wasm/bots.ts), plus the two host questions iMessage also answers in its
// host: which mark a seat wears, and which marks travel when the roles change
// hands.
//
// It is a port of ios/FoolishKit/Boards/MessageTableView+Roles.swift and of
// `RoleMarkKind.worn` in ios/FoolishKit/Boards/FRoleMotion.swift. Where the two
// could drift, the Swift wins.
//
// Nothing here decides WHEN a mark changes. That is the kernel's, and
// src/contexts/AnimationContext.tsx is where the three answers are fired.

import type { AnimRoles } from '@sdk/ts/wasm/bots.ts';
import type { RoleMarkKind } from '../components/RoleMark';
import type { RoleFlight, Spot } from './roleMotion';
import { PLAYER_STATUS, rulesOf, type TableView, type ViewSeat } from './view';

/** What the badges are wearing, as a value: the three facts a role mark is
 *  drawn from, so comparing two of them answers "did anything about the roles
 *  change" without a whole board diff. The kernel's `AnimRoles`, which is also
 *  what the three role rules take and return. iMessage's `RoleMarks`. */
export type ShownRoles = AnimRoles;

/** WHAT THE BOARD IS SHOWING, frozen together: the roles, and whether the grid
 *  on screen has cards on it.
 *
 *  The table's state is in here for one reason, and it is iMessage's: a seat's
 *  sword is up only while a bout is open, and a bout end empties the LIVE board
 *  a beat before the sweep that takes the cards away has played. iMessage keeps
 *  the pre-bout grid for the length of the sweep (`sweepBattles`) precisely so
 *  every attacker's sword stays up until the hand-off turns it out - "if the
 *  grid were still standing when the roles change, the seats that just said good
 *  would each flash a sword" is the same rule read from the other end
 *  (MessageTableView+Sequence.swift's closing beat). Freezing it beside the
 *  roles gives the web the same answer without a second grid: both advance in
 *  the one write, at the closing beat. */
export interface ShownBoard {
    roles: ShownRoles;
    /** A bout is open: the grid the board is painting has cards on it. */
    tableOpen: boolean;
}

/** The roles a board holds. `defender` and `first_attacker` are raw fields of
 *  the view, but WHETHER A SEAT IS MARKED AT ALL is the kernel's
 *  (`client_view_rules`: the deal lays the stock out before it turns the trump,
 *  and until then nobody leads or defends anything). `defenderBadge` is that
 *  answer, so a board that is still dealing hands back no roles and no seat
 *  wears anything - which is also what keeps a cold first paint from flying a
 *  mark out of a seat that never held one. */
export function rolesOf(view: TableView): ShownRoles {
    const dealt = rulesOf(view).defenderBadge >= 0;
    if (!dealt) return { defender: -1, firstAttacker: -1, goodMask: 0 };
    return { defender: view.defender, firstAttacker: view.firstAttacker, goodMask: view.goodMask };
}

/** The whole shown board a view holds, for a write that is not inside a
 *  sequence. */
export const shownBoardOf = (view: TableView): ShownBoard => ({
    roles: rolesOf(view),
    tableOpen: view.battles.length > 0,
});

/** Two role states naming the same thing. */
export const sameRoles = (a: ShownRoles, b: ShownRoles): boolean =>
    a.defender === b.defender && a.firstAttacker === b.firstAttacker && a.goodMask === b.goodMask;

export const sameShownBoard = (a: ShownBoard, b: ShownBoard): boolean =>
    a.tableOpen === b.tableOpen && sameRoles(a.roles, b.roles);

/** DOES THIS SEAT WEAR THE SWORD?
 *
 *  A port of `showsSword` (MessageTableView+Roles.swift:39-48), which is a HOST
 *  function on iMessage too, and deliberately not the kernel's `turn_may_act`:
 *
 *    - an attacker keeps the sword until THEY say good, even once every attack
 *      on the table is covered. `turn_may_act` stands a seat down the moment
 *      nothing is left uncovered, which erased every sword the instant the table
 *      filled up;
 *    - it is asked of the roles the board is SHOWING, not the ones the kernel
 *      has moved on to, so a bout end does not re-cast every sword a beat before
 *      the sequence that earns it has played;
 *    - on an EMPTY table only ONE seat can act at all, so only the opener is
 *      marked. The owner: "when no cards are on the table, and only the first
 *      attacker can move, ONLY the first attacker gets the sword. Everyone else
 *      has no icon." Once the bout is open, throw-ins make the others attackers
 *      for real and they get one.
 *
 *  None of those three is a fact about a board, which is why this is not a field
 *  of one: the first differs from the kernel's own rule on purpose, and the
 *  other two are about what is on screen. */
export function showsSword(seat: number, isOut: boolean, shown: ShownBoard): boolean {
    const { roles, tableOpen } = shown;
    if (seat < 0 || seat === roles.defender || isOut) return false;
    if ((roles.goodMask >> seat) & 1) return false;
    return tableOpen ? true : seat === roles.firstAttacker;
}

/** THE ONE MARK a seat wears, from the facts a screen carries about it.
 *
 *  NEVER TWO, and that is the point of ranking them in one function rather than
 *  letting two components each decide. A port of `RoleMarkKind.worn`
 *  (FRoleMotion.swift), which is a host function on iMessage too: "a seat is
 *  never two of these at once" - the kernel rejects a defender's good (game.c
 *  handle_good) and `showsSword` stands the sword down for a seat that has said
 *  it, so shield, sword and check are mutually exclusive in every state the
 *  engine can produce, which is what lets them be one coin with three faces.
 *
 *  An out seat wears nothing (iMessage turns an out seat's badge edge-on, and
 *  `selfRoleMark` is `isOut ? nil`) - but its seat keeps its landing pad, so a
 *  flight still crossing it lands where it was aimed. */
export function markWorn(shown: ShownBoard, seat: number, player: ViewSeat | undefined): RoleMarkKind | null {
    const isOut = player?.status === PLAYER_STATUS.OUT;
    if (isOut) return null;
    if ((shown.roles.goodMask >> seat) & 1) return 'check';
    if (shown.roles.defender === seat) return 'shield';
    if (showsSword(seat, isOut, shown)) {
        return shown.roles.firstAttacker === seat ? 'leadSword' : 'sword';
    }
    return null;
}

/** WHICH MARKS TRAVEL between two role states, and from where to where.
 *
 *  A defender's shield and the first attacker's sword are the only marks that
 *  BELONG to a seat and then belong to another one, so they are the only two
 *  that fly. Everything else a role change does - a sword becoming a check, a
 *  check clearing at the end of a bout, an attacker who may no longer attack -
 *  is a gesture the mark makes where it stands, which is the owner's "most
 *  swords can fade": a mark that flies is a mark that went somewhere, and nobody
 *  took those.
 *
 *  The SWORD leaves the seat that opened the bout that just ended, even when
 *  that seat is currently wearing a check for having said good. What travels is
 *  the right to open, not the glyph that happened to be on screen.
 *
 *  A flight needs BOTH pads to have published. When one has not - a cold first
 *  layout, a seat that just went out and stopped drawing a mark - that mark
 *  simply changes in place. Never a reason to withhold the state.
 *
 *  A port of `MessageTableView.roleFlights`. */
export function roleFlightsBetween(old: ShownRoles, target: ShownRoles,
                                   pads: ReadonlyMap<number, Spot>): RoleFlight[] {
    const pad = (seat: number): Spot | null => {
        const p = seat >= 0 ? pads.get(seat) : undefined;
        return p && (p.x !== 0 || p.y !== 0) ? p : null;
    };
    const flights: RoleFlight[] = [];
    // The shield goes to whoever is defending now - including a PASS (perevod),
    // which is the same hand-off happening inside a bout.
    if (old.defender !== target.defender) {
        const from = pad(old.defender);
        const to = pad(target.defender);
        // ROUND 21, the owner: "the first attacker sword fully spins around, but
        // the shield kinda turns a little bit then turns back. Make the shield
        // spin all the way around too." The lean was 24 degrees, and that last
        // clause is the bug in it: a ghost that ends at 24 degrees is replaced by
        // a real shield drawn upright, so the mark visibly snapped back on
        // landing. A WHOLE turn is the only lean that ends where the badge draws
        // it, because 360 and 0 are the same angle.
        if (from && to) {
            flights.push({
                id: `shield-${old.defender}-${target.defender}`, kind: 'shield',
                from, to, fromSeat: old.defender, toSeat: target.defender, spin: 360,
            });
        }
    }
    if (old.firstAttacker !== target.firstAttacker) {
        const from = pad(old.firstAttacker);
        const to = pad(target.firstAttacker);
        // Round 20: what flies is the OPENER's sword, so the ghost wears the
        // opener's tint - the whole point of the tint is that you can follow this
        // one across the table and see which seat it settles on. A full turn: it
        // is being thrown to the next player to swing.
        if (from && to) {
            flights.push({
                id: `sword-${old.firstAttacker}-${target.firstAttacker}`, kind: 'leadSword',
                from, to, fromSeat: old.firstAttacker, toSeat: target.firstAttacker, spin: 360,
            });
        }
    }
    return flights;
}
