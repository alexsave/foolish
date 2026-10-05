// BidPicker.swift - the bid control on the stage's shelf (flat, never
// turned): a quantity stepper, face chips 2 to 6, then Call and Raise (the
// study's order: Liar left, Raise right).
//
// A PURE COMPONENT. It is handed the kernel's `Menu` and two closures; it
// never ranks one bid against another. Raise is lit exactly when the chosen
// quantity is at least the kernel's least quantity for the chosen face
// (`Menu.minQuantityByFace`), and Call exactly when the kernel says
// `callAllowed`. Both verdicts are the static functions below, so the view
// and the tests read the same two lines.
//
// NOTHING IS STAGED BEFORE MY DICE REST (DECISIONS I21): `ready` is the
// stage's verdict (StageDirector.atRest); until it, both verbs are sunk.

import SwiftUI

public struct BidPicker: View {
    public let menu: Menu
    public let raiseTitle: String
    public let callTitle: String
    public let onRaise: (Bid) -> Void
    public let onCall: () -> Void
    /// My dice are at rest: a move may be staged.
    public let ready: Bool

    @State private var quantity: Int
    @State private var face: Int

    public init(menu: Menu, raiseTitle: String, callTitle: String,
                ready: Bool = true, onRaise: @escaping (Bid) -> Void, onCall: @escaping () -> Void) {
        self.ready = ready
        self.menu = menu
        self.raiseTitle = raiseTitle
        self.callTitle = callTitle
        self.onRaise = onRaise
        self.onCall = onCall
        _quantity = State(initialValue: menu.minimumRaise.quantity)
        _face = State(initialValue: menu.minimumRaise.face)
    }

    /// The faces a bid may name (R2: no bid on 1s). A picker's range, like
    /// a keyboard's keys.
    public static let faces = 2...6

    /// Raise is enabled: the kernel ranked this face and the quantity is at
    /// least its least legal one.
    public static func raiseEnabled(quantity: Int, face: Int, menu: Menu) -> Bool {
        guard let least = menu.minQuantity(face: face) else { return false }
        return quantity >= least && quantity <= menu.maxQuantity
    }

    /// Call is enabled: the kernel says so.
    public static func callEnabled(menu: Menu) -> Bool { menu.callAllowed }

    /// The stepper's range: from the least quantity any face allows up to
    /// every die on the table.
    public static func quantityRange(menu: Menu) -> ClosedRange<Int> {
        let least = faces.compactMap { menu.minQuantity(face: $0) }.min() ?? 1
        let lo = max(1, min(least, menu.maxQuantity))
        return lo...max(lo, menu.maxQuantity)
    }

    /// The plate each verb wears (DECISIONS I27): Raise is the glowing bronze
    /// plate when the kernel would take it, Liar the blood plate when the
    /// kernel offers the call, and either is sunk while it is not allowed.
    public static func raiseKind(enabled: Bool) -> PlankKind { enabled ? .glow : .sunk }
    public static func callKind(enabled: Bool) -> PlankKind { enabled ? .call : .sunk }

    /// The study's chip: 26 points (`picker`'s `chip`; 30 from 430 wide).
    static func chipSize(width: CGFloat) -> CGFloat { width >= 430 ? 30 : 26 }

    public var body: some View {
        let range = Self.quantityRange(menu: menu)
        let raiseOn = ready && Self.raiseEnabled(quantity: quantity, face: face, menu: menu)
        let callOn = ready && Self.callEnabled(menu: menu)
        GeometryReader { geo in
            let chip = Self.chipSize(width: geo.size.width + 32)
            VStack(spacing: 10) {
                HStack(spacing: 10) {
                    HStack(spacing: 8) {
                        SquarePlank(glyph: "\u{2212}", kind: quantity > range.lowerBound ? .quiet : .sunk, seed: 2,
                                    accessibility: "minus") { quantity -= 1 }
                        Text(verbatim: "\(quantity)")
                            // the study's own fallback (its Open tab): Fell's old-style 1 is a
                            // small capital I, and "1" read as "I" on the phone, so the
                            // stepper's numeral alone is the system serif's lining figures
                            .font(.system(size: 26, design: .serif).monospacedDigit())
                            .bidInk()
                            .frame(minWidth: 34)
                        SquarePlank(glyph: "+", kind: quantity < range.upperBound ? .bronze : .sunk, seed: 10,
                                    accessibility: "plus") { quantity += 1 }
                    }
                    .frame(maxWidth: .infinity)
                    HStack(spacing: chip * 0.32) {
                        ForEach(Array(Self.faces), id: \.self) { f in
                            faceChip(f, chip: chip)
                        }
                    }
                    .frame(maxWidth: .infinity)
                }
                .frame(height: 40)
                HStack(spacing: 10) {
                    PlankButton(title: callTitle, kind: Self.callKind(enabled: callOn), seed: 5) { onCall() }
                    PlankButton(title: raiseTitle, kind: Self.raiseKind(enabled: raiseOn), seed: 6) {
                        onRaise(Bid(quantity: quantity, face: face))
                    }
                }
            }
            .frame(width: geo.size.width, alignment: .top)
        }
        .frame(height: 90)
        .onChange(of: menu) { _, m in
            quantity = m.minimumRaise.quantity
            face = m.minimumRaise.face
        }
    }

    private func faceChip(_ f: Int, chip: CGFloat) -> some View {
        let chosen = f == face
        return Button {
            Haptics.fire(.pickUp)
            face = f
        } label: {
            Die(face: f, size: chip, counts: chosen, seed: 70 + f)
                .padding(chip * 0.16)
                .contentShape(Rectangle())
                .padding(-chip * 0.16)
        }
        .buttonStyle(FPressStyle())
        .accessibilityAddTraits(chosen ? .isSelected : [])
    }
}
