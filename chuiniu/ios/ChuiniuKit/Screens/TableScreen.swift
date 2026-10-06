// TableScreen.swift - a round being bid: the kernel's table (StageView), the
// bid plate and, on my turn, the picker, both flat over it at the HUD's
// frames (DECISIONS I25). A round's first look on this phone throws every
// seat's cup (I19, I21), once: the kernel keeps the rounds watched; the
// picker's verbs wake when my dice are at rest.

import SwiftUI

public struct TableScreen: View {
    @ObservedObject var host: ChuiniuHost
    @StateObject private var director = StageDirector(stage: KernelSeam.stage())
    @Environment(\.displayScale) private var displayScale
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    public init(host: ChuiniuHost) { self.host = host }

    public var body: some View {
        let t = host.table
        GeometryReader { geo in
            let request = StageRequest(screen: .table, drawer: geo.size, scale: displayScale,
                                       roll: t.rollPending, rollID: t.rollID,
                                       table: StageTableKey(t))
            ZStack(alignment: .topLeading) {
                Color.clear
                if let hud = director.hud {
                    hudLayer(t, hud)
                } else if let menu = t.menu {
                    // the kernel drew nothing (a stale pair): the picker still works
                    VStack {
                        Spacer(minLength: 0)
                        picker(menu).padding(.horizontal, 16).padding(.bottom, 12)
                    }
                }
            }
            // the table runs on under the safe areas; the HUD keeps to them
            .background(alignment: .topLeading) {
                StageView(director: director, names: t.seats.map { StageName(seat: $0) }, outWord: host.word(.out),
                          inset: geo.safeAreaInsets)
                    .ignoresSafeArea()
            }
            .onAppear { begin(request) }
            .onChange(of: request) { _, r in begin(r) }
        }
        .background(Self.glassBackground.ignoresSafeArea())
    }

    /// Under the planks, where nothing is drawn (the study's `.scr.peekB`).
    static let glassBackground = Ink.glass

    private func begin(_ r: StageRequest) {
        director.reduceMotion = reduceMotion
        // THE THROW ONCE A PHONE: the kernel decides whether the round throws
        // (cn_api_roll_pending) and is told when it ran to its end. Reduce
        // Motion is that end at once: the director starts the clock at the
        // throw's total and reports it from the begin.
        let rollID = r.rollID
        director.onRollDone = { [weak host] in host?.rollSeen(rollID) }
        director.begin(r, planMs: host.planMs)
    }

    @ViewBuilder private func hudLayer(_ t: TableModel, _ hud: CnStageHudSnap) -> some View {
        // the plate carries the bid on the table and nothing else: no headline,
        // no ask line (the study dropped both, round nineteen; DECISIONS I28).
        // Whose turn it is is the glow under a name, on the planks.
        // where my throw's held cup passes under the plate (the kernel's
        // `plate_throw`) the plate stands down until my dice rest: it is flat
        // on the glass, over the picture, and hid half my cup
        if let p = hud.plateRect, !t.bidText.isEmpty, Self.plateShown(hud, atRest: director.atRest) {
            BidPlate(text: t.bidText, face: t.bid?.face).at(p)
                .transition(.opacity)
        }
        if let menu = t.menu {
            let s = hud.shelfRect ?? CGRect(x: 0, y: hud.h - 90, width: hud.w, height: 90)
            picker(menu)
                .padding(.horizontal, 16)
                .frame(width: s.width, height: s.height, alignment: .top)
                .position(x: s.midX, y: s.midY)
                .transition(.opacity)
        }
    }

    /// The plate is up unless my throw is in the air and passes under it.
    static func plateShown(_ hud: CnStageHudSnap, atRest: Bool) -> Bool { atRest || hud.plateThrow == 0 }

    private func picker(_ menu: Menu) -> some View {
        BidPicker(menu: menu, raiseTitle: host.word(.raise), callTitle: host.word(.call),
                  ready: director.atRest, onRaise: { host.raise($0) }, onCall: { host.call() })
    }
}
