// FCheckbox.swift - a wooden tick box.
//
// The owner, on the podkidnoy variant: "we'll need a wooden style checkbox in
// the lobby to set this." There was no checkbox in this app at all until now,
// and a system Toggle would have been the wrong thing twice over: it is a glass
// iOS switch on a table made of wool and wood, and it is a control whose whole
// meaning here is "a rule of this game is on" rather than "a setting of your
// phone is on".
//
// So it is built from the pieces the board is already made of: a WoodFill plank
// with the same rim FButton's wood kind wears, and the SAME hand-drawn FCheck
// the seat badges use to say a player has spoken. The tick a player sees under
// a name and the tick in this box are one glyph, deliberately - a check means
// "yes, this" everywhere in this app.
//
// The whole row is the target, label included: a 24pt box is under the
// 44pt-touch minimum on its own, and reaching for the words is what everyone
// does anyway.
//
// ONE WORD, and the box centred on it (owner, 1.0(17)). The label carried a
// parenthetical and, when clear, a second line explaining the other rule; both
// are gone. A checkbox in a lobby names the thing it turns on and nothing else -
// what the two games ARE is the rulebook's job, and the lobby has a rulebook
// button two inches away.

import SwiftUI

// Not `public`: nothing outside FoolishKit names it, and a public View in a
// dynamic framework exports its whole SwiftUI generic tree as symbol names
// (see RulesView.swift for the measurement).
struct FCheckbox: View {
    private let title: String
    private let isOn: Bool
    private let enabled: Bool
    private let action: (Bool) -> Void
    private let turn: Turn?

    /// SOMEBODY ELSE MOVED THIS BOX, and the plan says to show that happening
    /// (1.1(56); owner: "lets do the 'rotate in' or out thing for the
    /// checkbox"). `token` is bumped once per rules BEAT and `seconds` is the
    /// beat's own duration - both the kernel's (SurfacePlan), so the box turns
    /// at the same pace a card flies.
    ///
    /// A LOCAL TAP TURNS THE BOX TOO, and this is the owner's ruling against an
    /// earlier cut of this file which said it should not: "our own toggle should
    /// still rotate." The reasoning that lost was that a turn would take
    /// instantness away from the one person who already knows what they did. It
    /// does not, because the turn CARRIES the tick (see `shownOn`): the box
    /// starts moving the instant the finger lifts, and what the human loses is
    /// not the response but a hard cut they were never watching for. What they
    /// gain is that the control behaves the same way whoever moved it, which is
    /// the whole reason the rule change has an idiom at all.
    ///
    /// So this `Turn` is the ARRIVING one - a beat off the plan, somebody else's
    /// text - and a tap raises its own through `runTurn` with the same kernel
    /// duration. Two sources, one motion.
    struct Turn: Equatable {
        let token: Int
        let seconds: TimeInterval
    }

    public init(_ title: String, isOn: Bool,
                enabled: Bool = true, turn: Turn? = nil,
                action: @escaping (Bool) -> Void) {
        self.title = title
        self.isOn = isOn
        self.enabled = enabled
        self.turn = turn
        self.action = action
    }

    private let box: CGFloat = 26

    /// HOW FAR THE BOX IS TURNED - 1 upright, edge-on at the halfway point.
    ///
    /// The SAME motion the seat badge makes when a fact about it stops being
    /// true (`FSeatBadge.badgeTurn`): an x-axis scale about the centre, 1 down
    /// to 0.001, which reads as a flat object turning away from you. Not a
    /// `rotation3DEffect`, deliberately - a real perspective rotation would be
    /// a second, differently-shaped turn standing next to the badge's, and the
    /// owner's note about the badge ("rotate only the badge, dim the name") is
    /// about there being ONE idiom here, not two that look nearly alike. 0.001
    /// rather than 0 for the badge's own reason: a view scaled to exactly zero
    /// can stop being laid out at all, and the row's height would twitch.
    static func boxTurn(edgeOn: Bool) -> CGFloat { edgeOn ? 0.001 : 1 }

    /// Half-way through the beat the box is edge-on; the tick it comes back
    /// with is the new one.
    @State private var edgeOn = false

    /// WHAT THE TICK IS DRAWING while a turn is running, or nil for "whatever
    /// `isOn` says" - which is the state it is in almost all the time.
    ///
    /// THE TURN HAS TO CARRY ITS CONTENT, and this is what the first two cuts
    /// got wrong. The rule changes in the same update that starts the turn, so
    /// the tick vanished at the top of the motion and the box then rotated out
    /// and back around nothing: the owner's read, off the film, was "it looks
    /// like the checkbox just toggles out", and he was describing exactly what
    /// was on screen. A toggle wearing a rotation's timing is not a rotation.
    ///
    /// So the box keeps the OLD tick through the first half, swaps at the
    /// midpoint - where it is edge-on and the swap cannot be seen - and comes
    /// back carrying the new one. That is what `FSeatBadge` does and why it
    /// reads as an object turning: the badge's CONTENT turns with it.
    ///
    /// The old value is `!isOn` exactly, and not a guess: `anim_surface_plan`
    /// emits a rules beat only when the rule actually moved (`passing_after !=
    /// passing_before`), so a turn landing at all means the box has just flipped.
    @State private var shownOn: Bool?

    /// Which turn owns the box right now. Bumped by every `runTurn`, and read
    /// back at the midpoint: two quick taps would otherwise have the FIRST
    /// turn's second half fire in the middle of the second turn, snapping the
    /// box upright and swapping the tick early.
    @State private var turnToken = 0

    public var body: some View {
        Button(action: {
            Haptics.fire(.drop)
            // The turn starts BEFORE the action, so the box is already moving
            // while the reseal is still in the kernel. `isOn` has not moved yet
            // at this point, so the tick turning away is `isOn` itself - the
            // mirror of the arriving case below.
            runTurn(seconds: SurfacePlan.beatSeconds, outgoing: isOn)
            action(!isOn)
        }) {
            // CENTRES, not baselines: the label is one word, so there is no
            // block of text for a baseline to belong to - the box and the word
            // are two objects of a size, and the eye lines up their middles.
            HStack(alignment: .center, spacing: FSpace.s) {
                plank
                Text(title)
                    .font(FType.body(15))
                    .onTableText(dimmed: !enabled)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            // The row, not the box, is what a finger has to find.
            .contentShape(Rectangle())
            .frame(minHeight: 44)
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(title))
        .accessibilityValue(Text(isOn ? FStrings.t("ios.a11y.on") : FStrings.t("ios.a11y.off")))
        .accessibilityAddTraits(isOn ? [.isButton, .isSelected] : .isButton)
        // The beat landed. Turn edge-on over the first half of it and back over
        // the second, so the whole motion measures exactly what the plan said.
        //
        // WATCHING THE WHOLE VALUE, not `turn?.token`. An `onChange` closure
        // captures the properties of the body that BUILT it - the previous one -
        // so a closure keyed on the token read `turn` back as the value it had
        // BEFORE the change, which for the first turn of a session is nil: the
        // token moved, the guard below saw no duration and returned, and the
        // box swapped its tick with no motion at all. Filmed at 20fps and
        // logged (`secs=-1.0`), which is the only way that reads as anything
        // other than "the animation is too fast to see".
        // `.task(id:)` AND NOT `.onChange`, which is the 1.1(57) fix.
        //
        // An arriving rules beat is set in the SAME update that adopts the
        // chain, and anything that rebuilds this view in that update - a reload
        // racing the arrival, the still overlay coming off, a branch swapping -
        // leaves a FRESHLY BUILT checkbox holding a turn it never saw arrive.
        // `onChange` fires on a CHANGE observed by a view that already existed,
        // so a new view with the value already in place runs nothing, and the
        // turn is silently dropped. The owner, on 1.1(57): "when it did [update],
        // it only ever snapped, no rotate."
        //
        // `.task(id:)` has no such hole - it runs on first appearance as well as
        // on every change of the id - so the box turns whether it was here to
        // watch the value land or was built around it. The nil case is the
        // ordinary one and costs a guard.
        .task(id: turn) {
            let landed = turn
            AnimLog.say("checkbox turn secs=\(landed?.seconds ?? -1)")
            guard let landed, landed.seconds > 0 else { return }
            // An arriving beat lands in the SAME update that adopts the chain,
            // so `isOn` has already moved to the new rule by the time this runs
            // and the tick turning away is `!isOn`. A local tap is the other way
            // round - see the Button above - which is why the outgoing value is
            // an argument rather than being re-derived here.
            runTurn(seconds: landed.seconds, outgoing: !isOn)
        }
    }

    /// TURN THE BOX, over `seconds`, carrying `outgoing` until the midpoint.
    ///
    /// One routine for both sources - an arriving rules beat and a local tap -
    /// because they are the same motion and a second copy is a second thing to
    /// get out of step.
    ///
    /// A RUNLOOP TURN LATER, BOTH TIMES. When this is driven by an arrival it
    /// fires from inside the update that adopts the chain - the tick, the roster
    /// and the controls all change in it - and a `withAnimation` raised inside
    /// an ambient transaction that carries no animation is applied WITH that
    /// transaction, i.e. instantly. Filmed at 20fps the box swapped its tick in
    /// one frame and never turned, while the log said the beat had landed with a
    /// 500ms duration: the animation was not too fast to see, it was not an
    /// animation.
    private func runTurn(seconds: TimeInterval, outgoing: Bool) {
        guard seconds > 0 else { return }
        turnToken += 1
        let mine = turnToken
        let half = seconds / 2
        shownOn = outgoing                  // keep the tick that is turning away
        DispatchQueue.main.async {
            withAnimation(.easeIn(duration: half)) { edgeOn = true }
            DispatchQueue.main.asyncAfter(deadline: .now() + half) {
                guard mine == turnToken else { return }   // a newer turn owns the box
                shownOn = nil                   // edge-on: the swap is invisible
                withAnimation(.easeOut(duration: half)) { edgeOn = false }
            }
        }
    }

    /// The box itself: a small wooden plank, sharp-cornered like every other
    /// wooden control here, dimmed the same way FButton dims (a black tint
    /// composited INTO the fill, never opacity on the whole view - see round-6
    /// #19: a translucent control lets the weave show through and reads as a
    /// ghost rather than as something switched off).
    private var plank: some View {
        ZStack {
            WoodFill()
                .overlay(Color.black.opacity(enabled ? 0 : 0.45))
            Rectangle()
                .strokeBorder(.black.opacity(enabled ? 0.35 : 0.2), lineWidth: 1)
            // ONLY THE TICK TURNS. Owner, 1.1(57): "the whole check and the box
            // rotated! not ideal. only rotate the check itself." The box is a
            // fixed part of the furniture - the row it sits in, the label beside
            // it and the plank itself do not move - and what changed is the mark
            // on it, so the mark is what carries the motion. It also reads
            // better in the two directions: ticking swings the check IN to an
            // empty box, unticking swings it OUT and leaves the box standing.
            if shownOn ?? isOn {
                FCheck(size: box - 6)
                    .scaleEffect(x: Self.boxTurn(edgeOn: edgeOn), anchor: .center)
            }
        }
        .frame(width: box, height: box)
    }
}
