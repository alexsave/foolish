// BubbleSnapshot.swift - the transcript bubble's picture, 300 by 195 points,
// baked once at staging and the same on every phone.
//
// THE CUPS ARE THE KERNEL'S. Past the lobby, the picture is the stage's own
// bubble (CN_STAGE_BUBBLE, DECISIONS I24): the fixed camera the study turns
// about (150, 150), every seat's cup in a row with its count on the crown, a
// seat that is out lying on its side. Swift paints only what the HUD leaves
// to the host: the felt under the picture, each seat's name in its box and
// the bid plate, at the HUD's places. Nobody's dice are in it, not even
// after a call (the kernel's bubble draws none).
//
// I18: the plate carries my staged raise while it is staged (the committed
// table does not hold it until it is sent), else the bid on the table, else
// the reveal's tally; every name is the seat's bare name, since a bubble is
// seen by every phone.
//
// ONCE A STATE, AND NO ARENA LEFT BEHIND: `TableStage.bubble` draws the one
// frame, copies it out and frees the arena (and puts back a table on show),
// and the conversation keeps the picture per staged link
// (MessagesViewController.bubbleImage). A lobby has no dice to draw, so its
// bubble is the title and the roster.

import SwiftUI
import UIKit

public enum BubbleSnapshot {
    public static let size = CGSize(width: 300, height: 195)

    /// The picture of `table` (the resident, read at staging), at the
    /// extension's `scale`. The stage draws at most 2 pixels a point, and the
    /// whole picture is made at the scale the stage drew at, so nothing in it
    /// is scaled up.
    @MainActor
    public static func render(table: TableModel, title: String, scheme: ColorScheme, scale: CGFloat,
                              stage: TableStage? = nil) -> UIImage? {
        if table.phase != .lobby, let b = (stage ?? KernelSeam.stage()).bubble(scale: scale) {
            return image(StagePicture(table: table, bubble: b), scheme: scheme, scale: b.frame.shot.scale)
        }
        return image(LobbyPicture(table: table, title: title), scheme: scheme, scale: scale)
    }

    @MainActor
    private static func image(_ picture: some View, scheme: ColorScheme, scale: CGFloat) -> UIImage? {
        let content = ZStack {
            FeltBackground()
            picture
        }
        .frame(width: size.width, height: size.height)
        .environment(\.colorScheme, scheme)
        .dynamicTypeSize(.large)
        let renderer = ImageRenderer(content: content)
        renderer.scale = max(1, scale)
        renderer.isOpaque = true
        return renderer.uiImage
    }
}

private struct LobbyPicture: View {
    let table: TableModel
    let title: String

    var body: some View {
        VStack(spacing: 6) {
            Text(title).font(.system(size: 15, weight: .heavy)).onFeltText()
            VStack(alignment: .leading, spacing: 2) {
                ForEach(table.seats) { seat in
                    Text(seat.name).font(.system(size: 12.5, weight: .heavy)).onFeltText().lineLimit(1)
                }
            }
        }
        .padding(.top, 12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }
}

/// The stage's frame at its canvas, the names in their boxes, the plate.
private struct StagePicture: View {
    let table: TableModel
    let bubble: BubbleFrame

    /// A CN_NAME_BOX (cn_lay.h): 80 by 30, centred on the anchor's x, its
    /// top 8 above the anchor's y.
    private static let nameBox = CGSize(width: 80, height: 30)
    private static let nameUp: CGFloat = 8

    var body: some View {
        let hud = bubble.hud
        let shot = bubble.frame.shot
        ZStack(alignment: .topLeading) {
            Image(decorative: bubble.frame.image, scale: shot.scale)
                .resizable()
                .frame(width: shot.canvas[2], height: shot.canvas[3])
                .offset(x: shot.canvas[0], y: shot.canvas[1])
            ForEach(table.seats.filter { $0.id < hud.seats }) { seat in
                Text(seat.name)
                    .font(.system(size: 11, weight: .semibold).smallCaps())
                    .tracking(1.2)
                    .onFeltText(lit(seat) ? FColor.textPrimary : FColor.textDim)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                    .frame(width: Self.nameBox.width, height: Self.nameBox.height)
                    .offset(x: hud.nameX[seat.id] - Self.nameBox.width / 2, y: hud.nameY[seat.id] - Self.nameUp)
            }
            if hud.hasPlate != 0, let p = plate {
                BubblePlate(text: p.text, face: p.face)
                    .frame(width: hud.plate[2], height: hud.plate[3])
                    .offset(x: hud.plate[0], y: hud.plate[1])
            }
        }
        .frame(width: BubbleSnapshot.size.width, height: BubbleSnapshot.size.height, alignment: .topLeading)
    }

    /// The move this bubble carries (I18).
    private var plate: (text: String, face: Int?)? {
        if let b = table.stagedBid { return (table.stagedBidText, b.face) }
        if let r = table.reveal { return (r.tally, nil) }
        if let b = table.bid, !table.bidText.isEmpty { return (table.bidText, b.face) }
        return nil
    }

    /// The seat whose move the plate carries is lit, the rest dim (the
    /// study's bubble).
    private func lit(_ seat: SeatModel) -> Bool {
        guard seat.alive else { return false }
        if table.stagedBid != nil { return seat.isMe }
        if table.reveal != nil { return true }      // a reveal: every seat still in
        return seat.id == table.bidder
    }
}

/// The bid plate, as the study's bubble draws it at 160 by 56: a dark plate
/// with a brass bevel and four rivets, the words and the face beside them.
private struct BubblePlate: View {
    let text: String
    let face: Int?

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 6)
                .fill(LinearGradient(colors: [Color(hex: 0x2F4A40), Color(hex: 0x1C2E28)],
                                     startPoint: .top, endPoint: .bottom))
            RoundedRectangle(cornerRadius: 4)
                .strokeBorder(FColor.win.opacity(0.55), lineWidth: 1)
                .padding(3)
            GeometryReader { g in
                ForEach(0..<4, id: \.self) { i in
                    Circle().fill(FColor.win.opacity(0.8))
                        .frame(width: 5, height: 5)
                        .position(x: i % 2 == 0 ? 8.5 : g.size.width - 8.5, y: i < 2 ? 8.5 : g.size.height - 8.5)
                }
            }
            HStack(spacing: 8) {
                Text(text)
                    .font(.system(size: 24, weight: .heavy, design: .serif))
                    .onFeltText()
                    .lineLimit(1)
                    .minimumScaleFactor(0.5)
                if let face { Die(face: face, size: 28) }
            }
            .padding(.horizontal, 14)
        }
        .shadow(color: .black.opacity(0.45), radius: 3, y: 2)
    }
}
