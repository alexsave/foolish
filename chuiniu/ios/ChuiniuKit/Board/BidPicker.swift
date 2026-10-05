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

    public var body: some View {
        let range = Self.quantityRange(menu: menu)
        VStack(spacing: 8) {
            HStack(spacing: 10) {
                stepButton("minus", enabled: quantity > range.lowerBound) { quantity -= 1 }
                Text(verbatim: "\(quantity)")
                    .font(.system(size: 24, weight: .heavy).monospacedDigit())
                    .onFeltText()
                    .frame(minWidth: 40)
                stepButton("plus", enabled: quantity < range.upperBound) { quantity += 1 }
                Spacer(minLength: 6)
                HStack(spacing: 6) {
                    ForEach(Array(Self.faces), id: \.self) { f in
                        faceChip(f)
                    }
                }
            }
            HStack(spacing: 10) {
                WoodButton(title: callTitle, height: 40, enabled: ready && Self.callEnabled(menu: menu)) { onCall() }
                WoodButton(title: raiseTitle, height: 40,
                           enabled: ready && Self.raiseEnabled(quantity: quantity, face: face, menu: menu)) {
                    onRaise(Bid(quantity: quantity, face: face))
                }
            }
        }
        .onChange(of: menu) { _, m in
            quantity = m.minimumRaise.quantity
            face = m.minimumRaise.face
        }
    }

    private func stepButton(_ symbol: String, enabled: Bool, _ action: @escaping () -> Void) -> some View {
        SquareButton(systemImage: symbol, side: 34, accessibility: symbol, action: action)
            .disabled(!enabled)
            .opacity(enabled ? 1 : 0.45)
    }

    private func faceChip(_ f: Int) -> some View {
        let chosen = f == face
        return Button {
            Haptics.fire(.pickUp)
            face = f
        } label: {
            Die(face: f, size: 26)
                .padding(4)
                .background(
                    RoundedRectangle(cornerRadius: 8)
                        .fill(Color.black.opacity(chosen ? 0.35 : 0.12))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .strokeBorder(chosen ? FColor.win : .clear, lineWidth: 2)
                )
        }
        .buttonStyle(FPressStyle())
        .accessibilityAddTraits(chosen ? .isSelected : [])
    }
}
