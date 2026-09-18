// LobbyControls — WHICH control a lobby offers, as a pure function of
// (mySeat, joined, capacity, who sent the newest bubble).
//
// Pulled out of LobbyView long ago and out of MessagesRootView.swift here: it
// is the lobby's decision layer, it answers through GateWire (the kernel), and
// LobbyActionOrderTests enumerates it. The screens that render its answer are
// in LobbyScreens.swift.

import Foundation

/// WHICH control a lobby offers, pulled out of `LobbyView` as a pure function
/// for one reason: the view had a state with NO control at all — joined, but
/// fewer than two players, so no Start (needs 2), no Join (already in), no
/// invite (notes 14/16 removed that button as redundant with the auto-stage).
/// A lobby listing one player and offering nothing is a dead end, and it is
/// invisible in a `if/else if/else` chain until someone lands in it. As an enum
/// there is always exactly one answer, and a test can enumerate every
/// (mySeat, joined, capacity) and assert so.
public enum LobbyControls {
    /// Start the game at the joined count. Round-5 M9 narrows this further —
    /// it is withheld from whoever sent the newest bubble while the lobby
    /// still has room (see `offered`'s doc): "2+ have joined" is no longer
    /// sufficient on its own.
    case start
    /// Re-stage the WAITING chain so the human can send the invite (I'm in,
    /// nobody else is yet, and the newest invite is NOT mine).
    case invite
    /// Nothing to do but wait. Two distinct situations render this way
    /// (round-5 M9 added the second one):
    ///  1. I'm in, nobody else is yet, and the invite sitting at the head of
    ///     this chain is the one I put there — unchanged from before M9.
    ///  2. I'm in, 2+ have joined, the lobby still has room, and the newest
    ///     bubble on the chain is mine (I just joined, or just re-staged the
    ///     invite) — M9's whole point: I cannot also be the one who starts.
    /// Both read the same "Waiting for the others" text; the owner explicitly
    /// ruled out a capacity/"N of M" line to tell them apart (M9 — "no
    /// capacity text, too confusing").
    case waiting
    /// Claim a seat (I'm not in, and there is room).
    case join
    /// Nothing to do but wait (I'm not in, and there is no room).
    case full

    /// `iSentTheInvite`: is the newest bubble on this chain one I staged or
    /// sent (`lastActorSeat == mySeat`)? Kept its round-4 name even though
    /// round-5 widens what it gates (below): it is public API and
    /// `Round4Tests.swift` already binds this exact argument label, so
    /// renaming it would break that file's BUILD, not just one of its
    /// assertions, over a docstring nicety.
    ///
    /// Round-4 note 1 — "if you were the last one to send an invite,
    /// shouldn't have the Send invite pop up." Offering it then asks the
    /// human to send a second copy of the invite already sitting in the
    /// thread (or in the compose field, freshly auto-staged), which is the
    /// state a creator lands in every single time.
    ///
    /// The trade this makes, deliberately and with the owner's call on it: the
    /// `.invite` button exists as the recovery path for a lobby whose
    /// auto-staged bubble is gone (sent, deleted from the compose field, or
    /// the extension reopened later). Gating it on authorship means a creator
    /// who deletes their own draft has no in-lobby way to re-stage it and must
    /// use New game. That is the cost of not nagging everyone else.
    ///
    /// Round-5 M9 — "if you were the last to send one of those join texts,
    /// you can't send a start text... that will make it a bit more difficult
    /// to lock people out." Extends the SAME authorship check to `.start`:
    /// once 2+ have joined, whoever sent the newest bubble (the last joiner,
    /// or whoever last re-staged the invite) is withheld from Start too, as
    /// long as the lobby still has room — so whoever is currently able to act
    /// is never the same person who could instead invite one more player in.
    ///
    /// EXEMPTION: a FULL lobby (`joined == capacity`) always offers Start to
    /// its last joiner regardless of authorship. Nobody else could join
    /// instead, so withholding Start there would just strand a full lobby
    /// with no way forward — and in a 2-player DM (capacity 2) it would force
    /// an extra, pointless round-trip into every single game: the joiner
    /// filling the last seat immediately starting is the designed "join and
    /// start" flow (note 2), not the lockout M9 is guarding against.
    /// `iChangedTheRules`: the newest bubble on this chain is MINE and it moved
    /// the passing checkbox (see `rulesChanged`). The owner's rule for the
    /// variant, in their words: "whoever changes the checkbox value cannot
    /// start the game, similar to how last joined cannot start the game."
    ///
    /// It is the M9 gate WITHOUT the full-lobby exemption, and the exemption's
    /// own reasoning is why. That exemption exists so a full lobby is never
    /// stranded: nobody else could join, so withholding Start from its last
    /// joiner would leave a table with no way forward. A rules change strands
    /// nothing - the reseal is sendable, and whoever opens it can start
    /// immediately - so the exemption has no work to do here, while the thing
    /// it would allow is exactly what the rule forbids: in a two-player DM
    /// (capacity 2, full the moment both are in) the changer could otherwise
    /// flip the rules and start in the same breath, and their opponent would
    /// first learn of it from a board that will not let them pass.
    ///
    /// THE RULE ITSELF IS THE KERNEL'S (msg_wire.c `msg_lobby_offered`), and has
    /// been since the beats work: this enum is a forwarder that maps the kernel's
    /// answer onto its own cases. It kept its shape - and every argument label,
    /// including `iSentTheInvite`, whose round-4 name outlived what it gates -
    /// because `Round4Tests` and `MessageLobbyTests` bind them, and because a
    /// SwiftUI `switch` wants an enum rather than an Int.
    ///
    /// Moving the decision down was the owner's standing test, "can it be done in
    /// C? If yes, do so", and it bought something concrete: the whole scenario
    /// table - every legal lobby text and every impossible one, in a 1:1 and in a
    /// group - is now asserted in C alongside the beats each one produces
    /// (c/tests/anim_plan_test.c), which is not expressible while the rule and
    /// the beats live in different languages.
    public static func offered(mySeat: Int?, joined: Int, capacity: Int,
                               iSentTheInvite: Bool = false,
                               iChangedTheRules: Bool = false) -> LobbyControls {
        switch GateWire.lobbyOffered(mySeat: mySeat ?? GateWire.noSeat,
                                     joined: joined, capacity: capacity,
                                     iSentTheNewest: iSentTheInvite,
                                     iChangedTheRules: iChangedTheRules) {
        case GateWire.lobbyStart:  return .start
        case GateWire.lobbyInvite: return .invite
        case GateWire.lobbyJoin:   return .join
        case GateWire.lobbyFull:   return .full
        default:                   return .waiting
        }
    }

    /// Did THIS device change the rules on the lobby it is showing?
    ///
    /// `baseline` is the passing rule as of the last bubble somebody ELSE put
    /// on this chain (nil if there has been none - a lobby this device
    /// created), `current` is what the lobby says now, and `mine` is whether
    /// the newest bubble is this device's.
    ///
    /// Asked this way, and not as a "I tapped the box" flag, for two reasons.
    /// It is SELF-CANCELLING: a player who ticks the box and thinks better of
    /// it lands back on the rules everyone else already has, and there is
    /// nothing left to withhold Start for. And it is answered by the CHAIN
    /// rather than by a memory of a tap, so it survives the extension being
    /// closed and reopened mid-lobby, which a flag would not.
    public static func rulesChanged(baseline: Bool?, current: Bool, mine: Bool) -> Bool {
        GateWire.lobbyRulesChanged(baseline: baseline, current: current, mine: mine)
    }

    /// May I LEAVE this lobby? A seated player may, once somebody else is
    /// seated too.
    ///
    /// Orthogonal to `offered` on purpose, rather than a sixth case of it:
    /// leaving is available alongside Start (both, side by side) and alongside
    /// Waiting (exit alone), and folding two independent answers into one enum
    /// would need a case per combination. The owner's shape is exactly this -
    /// "start game and the exit game buttons side by side WHEN BOTH ARE
    /// POSSIBLE. Currently start game is not possible for the last player that
    /// joined. Thus they can only exit."
    ///
    /// THE 2+ FLOOR is the wire's, not a preference: a WAITING envelope must
    /// carry at least one join (MSG_EJOINS), so the last player standing has no
    /// bubble to leave INTO. A lone creator's exit is New game, which replaces
    /// the invite outright.
    public static func canExit(mySeat: Int?, joined: Int) -> Bool {
        GateWire.lobbyCanExit(mySeat: mySeat ?? GateWire.noSeat, joined: joined)
    }

    /// May I move the passing checkbox? Only from a seat - so a spectator sees
    /// the rules and cannot change them, and a player who has just left is a
    /// spectator again. The owner, on the leaver: "as soon as they leave, the
    /// extension view should be as if they haven't joined, and only show the
    /// join button." Both halves of that fall out of the kernel without a
    /// leaver-shaped state anywhere: the seat is gone, so this is false and
    /// `offered` returns `.join`.
    public static func canSetRules(mySeat: Int?) -> Bool {
        GateWire.lobbyCanSetRules(mySeat: mySeat ?? GateWire.noSeat)
    }
}
