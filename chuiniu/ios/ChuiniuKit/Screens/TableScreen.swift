// TableScreen.swift - a round being bid: the kernel's table (StageView), the
// bid plate and, on my turn, the picker, both flat over it at the HUD's
// frames (DECISIONS I25). A round's first look throws every seat's cup
// (I19, I21); the picker's verbs wake when my dice are at rest.

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
                                       roll: t.rollID != host.playedRoll, rollID: t.rollID,
                                       table: StageTableKey(t))
            ZStack(alignment: .topLeading) {
                StageView(director: director, names: t.seats.map { StageName(seat: $0) }, outWord: host.word(.out))
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
            .onAppear { begin(request) }
            .onChange(of: request) { _, r in begin(r) }
        }
        .background(Self.glassBackground.ignoresSafeArea())
    }

    /// Under the planks, where nothing is drawn (the study's `.scr.peekB`).
    static let glassBackground = Color(hex: 0x050807)

    private func begin(_ r: StageRequest) {
        director.reduceMotion = reduceMotion
        let rollID = r.rollID
        director.onRollDone = { [weak host] in host?.playedRoll = rollID }
        director.begin(r, planMs: host.planMs)
        // Reduce Motion plays no throw: the round counts as played at once
        if reduceMotion, r.roll { host.playedRoll = rollID }
    }

    @ViewBuilder private func hudLayer(_ t: TableModel, _ hud: CnStageHudSnap) -> some View {
        if let p = hud.plateRect {
            if !t.bidText.isEmpty {
                BidPlate(text: t.bidText, face: t.bid?.face).at(p)
            } else if !t.caption.isEmpty {
                BidPlate(text: t.caption, face: nil, quiet: true).at(p)
            }
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

    private func picker(_ menu: Menu) -> some View {
        BidPicker(menu: menu, raiseTitle: host.word(.raise), callTitle: host.word(.call),
                  ready: director.atRest, onRaise: { host.raise($0) }, onCall: { host.call() })
    }
}
