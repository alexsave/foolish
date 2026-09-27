// COPIED from pickemup/ios/PickemupKit/Screens/BubbleSnapshot.swift at 8e216923 (itself COPIED from foolish/ios/FoolishKit/Boards/BubbleSnapshot.swift at c3d99192) - replaced by the bubble lift
//
// The transcript bubble's picture: foolish's 300 x 195, baked at insert and
// identical on every phone. A game bubble is the five dice and one line of
// caption; a KEEP bubble shows the kept dice and BLANK slots for the
// rerolling ones, because the sender's kernel has not derived them (T11) and
// the picture could not show them if it wanted to. A lobby bubble is the
// numbered roster.
//
// Rendered synchronously from a value (BubbleContent), taken from the
// resident at the moment of staging: the resident is one slot, so nothing
// reads it across an await.

import SwiftUI
import UIKit

/// What one bubble picture shows, all of it the kernel's.
public struct BubbleContent: Equatable, Sendable {
    public var lobby: Bool
    public var title: String
    public var roster: [String]
    public var dice: [Int]
    public var kept: [Bool]
    public var caption: String

    public init(lobby: Bool = false, title: String = "", roster: [String] = [],
                dice: [Int] = [], kept: [Bool] = [], caption: String = "") {
        self.lobby = lobby
        self.title = title
        self.roster = roster
        self.dice = dice
        self.kept = kept
        self.caption = caption
    }

    /// The picture for the resident as the kernel views it now.
    @MainActor
    public static func of(_ v: TallyView, title: String) -> BubbleContent {
        if v.phase == .lobby {
            return BubbleContent(lobby: true, title: title, roster: v.lobby.rows, caption: v.caption)
        }
        return BubbleContent(dice: v.tray.dice, kept: v.tray.kept, caption: v.caption)
    }
}

public enum BubbleSnapshot {
    public static let size = CGSize(width: 300, height: 195)
    /// T16: the bubble's dice are 44pt, 8pt apart (252pt of the 300).
    public static let dieSide: CGFloat = 44
    public static let dieGap: CGFloat = 8

    @MainActor
    public static func render(_ content: BubbleContent, scheme: ColorScheme, scale: CGFloat? = nil) -> UIImage? {
        let view = ZStack {
            FeltBackground()
            if content.lobby {
                LobbyPicture(content: content)
            } else {
                DicePicture(content: content)
            }
        }
        .frame(width: size.width, height: size.height)
        .environment(\.colorScheme, scheme)
        .dynamicTypeSize(.large)
        let renderer = ImageRenderer(content: view)
        renderer.scale = scale ?? UIScreen.main.scale
        renderer.isOpaque = true
        return renderer.uiImage
    }
}

private struct LobbyPicture: View {
    let content: BubbleContent

    var body: some View {
        VStack(spacing: 6) {
            Text(content.title).font(.system(size: 15, weight: .heavy)).onFeltText()
            VStack(alignment: .leading, spacing: 2) {
                ForEach(Array(content.roster.enumerated()), id: \.offset) { _, row in
                    Text(row).font(.system(size: 12.5, weight: .heavy)).onFeltText().lineLimit(1)
                }
            }
        }
        .padding(.top, 12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }
}

private struct DicePicture: View {
    let content: BubbleContent

    var body: some View {
        VStack(spacing: 18) {
            HStack(spacing: BubbleSnapshot.dieGap) {
                ForEach(0..<TrayModel.diceCount, id: \.self) { i in
                    DiceFace(value: content.dice[safe: i] ?? 0, kept: content.kept[safe: i] ?? false,
                             side: BubbleSnapshot.dieSide)
                }
            }
            Text(content.caption)
                .font(.system(size: 13, weight: .heavy))
                .onFeltText()
                .lineLimit(1)
                .minimumScaleFactor(0.7)
                .padding(.horizontal, 12)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
