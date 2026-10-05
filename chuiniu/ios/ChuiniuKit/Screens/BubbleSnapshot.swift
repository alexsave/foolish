// BubbleSnapshot.swift - the transcript bubble's picture, 300 by 195 points,
// baked once at staging and the same on every phone.
//
// THE CUPS ARE THE KERNEL'S. Past the lobby, the picture is the stage's own
// bubble (CN_STAGE_BUBBLE, DECISIONS I24): the fixed camera the study turns
// about (150, 150), every seat's cup in a row with its count on the crown, a
// seat that is out lying on its side. Swift paints only what the HUD leaves
// to the host: the planks under the picture (the study's bubble: the stage's
// overdraw, a plank's middle down the bubble's centre), each seat's name in
// its box in the small caps, and the verdigris bid plate, at the HUD's places
// (DECISIONS I27). Nobody's dice are in it, not even
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
            PlanksBackground(overdraw: true)
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
            Text(title).font(FType.serif(28)).bidInk()
            WaterRule().padding(.horizontal, 40)
            VStack(alignment: .leading, spacing: 4) {
                ForEach(table.seats) { seat in
                    Text(seat.name).font(FType.sc(14)).tracking(FType.nameTracking(14)).onPlanks().lineLimit(1)
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
                    .font(FType.sc(11))
                    .tracking(FType.nameTracking(11))
                    .onPlanks(lit(seat) ? Ink.ink : Ink.inkdim)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                    .frame(width: Self.nameBox.width, height: Self.nameBox.height)
                    .offset(x: hud.nameX[seat.id] - Self.nameBox.width / 2, y: hud.nameY[seat.id] - Self.nameUp)
            }
            if hud.hasPlate != 0, let p = plate {
                BidPlate(text: p.text, face: p.face)
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
