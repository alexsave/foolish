// TableScreen.swift - a round being bid: the dice table, the kernel's
// caption, and the bid picker when the kernel hands this phone a menu.

import SwiftUI

public struct TableScreen: View {
    @ObservedObject var host: ChuiniuHost

    public init(host: ChuiniuHost) { self.host = host }

    public var body: some View {
        let t = host.table
        VStack(spacing: 10) {
            DiceTable(table: t, youWord: host.word(.yourDice), outWord: host.word(.out),
                      played: host.playedRoll, onPlayed: { host.playedRoll = $0 })
            Text(t.caption)
                .font(.system(size: 14, weight: .semibold))
                .onFeltText()
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .frame(maxWidth: .infinity)
            if let menu = t.menu {
                BidPicker(menu: menu, raiseTitle: host.word(.raise), callTitle: host.word(.call),
                          onRaise: { host.raise($0) }, onCall: { host.call() })
                    .transition(.opacity)
            }
        }
        .padding(.horizontal, 16)
        .padding(.top, 8)
        .padding(.bottom, 12)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(FeltBackground())
        .animation(FMotion.chrome, value: t.menu == nil)
    }
}
